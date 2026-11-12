#!/usr/bin/env python3
"""Validate a realtime readback, hash all resources and convert its color to PFM.

Raw exports its noisy current-frame HDR, Native its denoised HDR and each layer's history confidence.
DLSSRR exports the inputs ray reconstruction received and its display-size HDR, and no NRD or SR work.
A frame that applied Bloom exports its display-size HDR after Bloom and lists Bloom as available.
A frame with auto exposure exports the exposure it used and the state its meter wrote for the next frame; RR read the
used exposure, so RR's reported exposure is null and its exposure texture must hold the used one.

This checks storage/finite values and frame provenance, not temporal correctness.
RR inputs are also checked against the value ranges of their contract (the frozen D6 input checks).
Packed surface records remain shader-version-specific; inactive records are undefined.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


# Per mode: HDR color resource, required GPU timing stages and accepted denoisers (REBLUR by default, RELAX as a
# validation control).
NRD_DENOISERS = ("NRD_REBLUR", "NRD_RELAX")
MODES = {
    "RealtimeRaw": ("noisy_radiance", ("BUILD", "FILL", "Guides", "Merge", "ToneMapping"), ()),
    "RealtimeNative": ("denoised_radiance", ("BUILD", "FILL", "Guides", "NRD", "ToneMapping"), NRD_DENOISERS),
    "RealtimeDLSSSR": ("upscaled_radiance", ("BUILD", "FILL", "Guides", "NRD", "SRInputs", "SR", "ToneMapping"), NRD_DENOISERS),
    "RealtimeDLSSRR": ("reconstructed_radiance", ("BUILD", "FILL", "Guides", "RRPrepare", "RR", "ToneMapping"), ()),
}
# The sun shadow denoiser of layer 0 in NRD frames with a sun.
SUN_SHADOW_DENOISER = "SIGMA_SHADOW"
NATIVE_LAYER_COUNT = 3
PIXEL_BYTES = {'TEX_FORMAT_RGBA16_FLOAT': 8, 'TEX_FORMAT_R32_FLOAT': 4, 'TEX_FORMAT_RG16_FLOAT': 4,
               'TEX_FORMAT_R8_UNORM': 1, 'TEX_FORMAT_RGBA8_TYPELESS': 4, 'TEX_FORMAT_RGBA32_FLOAT': 16}
FLOAT_FORMATS = ("TEX_FORMAT_RGBA16_FLOAT", "TEX_FORMAT_RG16_FLOAT", "TEX_FORMAT_R32_FLOAT", "TEX_FORMAT_RGBA32_FLOAT")
SR_FORMATS = {'denoised_radiance': 'TEX_FORMAT_RGBA16_FLOAT',
              'upscaled_radiance': 'TEX_FORMAT_RGBA16_FLOAT',
              'sr_depth': 'TEX_FORMAT_R32_FLOAT', 'sr_motion': 'TEX_FORMAT_RG16_FLOAT',
              'sr_ignore_history': 'TEX_FORMAT_R8_UNORM', 'display': 'TEX_FORMAT_RGBA8_TYPELESS'}
RR_FORMATS = {'rr_color': 'TEX_FORMAT_RGBA16_FLOAT', 'reconstructed_radiance': 'TEX_FORMAT_RGBA16_FLOAT',
              'rr_depth': 'TEX_FORMAT_R32_FLOAT', 'rr_motion': 'TEX_FORMAT_RG16_FLOAT',
              'rr_specular_motion': 'TEX_FORMAT_RG16_FLOAT', 'rr_normal_roughness': 'TEX_FORMAT_RGBA16_FLOAT',
              'rr_diffuse_albedo': 'TEX_FORMAT_RGBA16_FLOAT', 'rr_specular_albedo': 'TEX_FORMAT_RGBA16_FLOAT',
              'rr_exposure': 'TEX_FORMAT_R32_FLOAT', 'display': 'TEX_FORMAT_RGBA8_TYPELESS'}
# Per upscaling mode: the name its errors use, its resource formats and the display-size resources among them. The
# others are render-size, except RR's exposure, a single texel.
UPSCALING = {'RealtimeDLSSSR': ('SR', SR_FORMATS, ('display', 'upscaled_radiance')),
             'RealtimeDLSSRR': ('RR', RR_FORMATS, ('display', 'reconstructed_radiance'))}
RR_EXPOSURE = 'rr_exposure'
RR_ALBEDOS = ('rr_diffuse_albedo', 'rr_specular_albedo')
RR_QUALITIES = ('Native', 'Quality', 'Balanced', 'Performance')
# 1 for a left-handed view, -1 for a right-handed one (forward = -Z).
RR_FORWARD_SIGNS = (-1, 1)
# The RR depth of sky and invalid paths (kEnvironmentMapSceneDistance), and the normal length tolerance of D6.
RR_SKY_DEPTH = 5000000.0
RR_NORMAL_LENGTH_TOLERANCE = 2e-3
RR_NORMAL_ROUGHNESS_COMPONENTS = 4
# The display-size HDR after Bloom and before tone mapping.
BLOOM = 'bloom_radiance'
BLOOM_FORMATS = {BLOOM: 'TEX_FORMAT_RGBA16_FLOAT'}
# The exposure the frame used and the state its meter wrote (RTXPTAutoExposureShared.h): base exposure, initialized,
# target and event, then mean log2 luminance, valid samples, adaptation weight and the log2 of the used exposure.
AE_USED = 'auto_exposure_used'
AE_NEXT = 'auto_exposure_next'
AE_FORMATS = {AE_USED: 'TEX_FORMAT_R32_FLOAT', AE_NEXT: 'TEX_FORMAT_RGBA32_FLOAT'}
AE_EXTENTS = {AE_USED: [1, 1], AE_NEXT: [2, 1]}
AE_STATE_USED_EV = 7
# The used exposure is exp2 of the state's used exposure, both in 32-bit float.
AE_USED_RELATIVE_TOLERANCE = 1e-5


def validate_frame_jitter(manifest, label):
    if manifest['frame_index'] < 1 or manifest['sample_base_index'] != manifest['frame_index'] - 1:
        raise ValueError('Invalid successful frame/jitter index')
    if len(manifest['jitter']) != 2 or not all(math.isfinite(v) for v in manifest['jitter']):
        raise ValueError(f'Invalid {label} jitter')


def validate_rr_metadata(manifest, names):
    rr = manifest['ray_reconstruction']
    if not rr['provider'] or rr['quality'] not in RR_QUALITIES or not rr['preset']:
        raise ValueError('Invalid RR provider/quality/preset')
    exposure = rr['exposure']
    if manifest.get('exposure_mode') == 'Auto':
        exposure_valid = exposure is None and rr.get('exposure_source') == 'auto_exposure_used'
    else:
        exposure_valid = (rr.get('exposure_source', 'display_exposure') == 'display_exposure' and
                          math.isfinite(exposure) and exposure > 0)
    if (rr['depth_encoding'] != 'forward_positive_linear_view_depth_sky_5000000' or
            rr['motion_convention'] != 'current_to_previous_render_pixels_without_jitter' or
            rr['exposure_policy'] != 'explicit_exposure_texture_preexposure_1_scale_1' or
            rr['micro_jitter'] != 0 or rr['view_forward_sign'] not in RR_FORWARD_SIGNS or not exposure_valid):
        raise ValueError('Unexpected RR input semantics')
    if manifest['nrd_instances'] != 0 or any(name.startswith(('history_confidence', 'sr_')) for name in names):
        raise ValueError('RR capture must contain no NRD or super resolution work')
    unavailable = set(manifest['unavailable_stages'])
    if 'RR' in unavailable or not {'NRD', 'SR'} <= unavailable:
        raise ValueError('RR capture reports the wrong unavailable stages')


def validate_layout(manifest, resource, label, formats, display_names):
    name, fmt = resource['name'], resource['format']
    if fmt != formats[name]:
        raise ValueError(f'Unexpected {label} format: {name}')
    if name == RR_EXPOSURE:
        extent = [1, 1]
    else:
        extent = manifest['display_size' if name in display_names else 'render_size']
    if [resource['width'], resource['height']] != extent or resource['slices'] != 1:
        raise ValueError(f'Invalid {label} extent: {name}')
    if resource['row_bytes'] != resource['width'] * PIXEL_BYTES[fmt]:
        raise ValueError(f'Invalid {label} row format: {name}')


def validate_rr_values(name, values, rr):
    if name == 'rr_depth' and not all(0 < v <= RR_SKY_DEPTH for v in values):
        raise ValueError('RR depth must be positive and at most the sky depth')
    if name == 'rr_normal_roughness':
        for x, y, z, roughness in zip(*[iter(values)] * RR_NORMAL_ROUGHNESS_COMPONENTS):
            if abs(math.sqrt(x * x + y * y + z * z) - 1) > RR_NORMAL_LENGTH_TOLERANCE:
                raise ValueError('RR normal is not a unit vector')
            if not 0 <= roughness <= 1:
                raise ValueError('RR roughness outside [0,1]')
    if name in RR_ALBEDOS and not all(0 <= v <= 1 for v in values):
        raise ValueError(f'RR albedo outside [0,1]: {name}')
    if name == RR_EXPOSURE and rr['exposure'] is not None and values != [rr['exposure']]:
        raise ValueError('RR exposure texture differs from the reported exposure')


def validate_auto_exposure(manifest, names, values):
    mode = manifest.get('exposure_mode', 'Manual')
    if mode not in ('Manual', 'Auto'):
        raise ValueError('Unknown exposure mode')
    auto = mode == 'Auto'
    if auto != (manifest.get('auto_exposure') is not None) or auto != (AE_FORMATS.keys() <= names):
        raise ValueError('Auto exposure resources must be exported exactly with auto exposure')
    if not auto:
        if AE_FORMATS.keys() & names:
            raise ValueError('Manual exposure capture exports auto exposure resources')
        return
    used, state = values[AE_USED][0], values[AE_NEXT]
    if not used > 0:
        raise ValueError('Auto exposure used a non-positive exposure')
    if abs(2.0 ** state[AE_STATE_USED_EV] - used) > AE_USED_RELATIVE_TOLERANCE * used:
        raise ValueError('Auto exposure state was written by another frame than the used exposure')
    if manifest['mode'] == 'RealtimeDLSSRR' and values.get(RR_EXPOSURE) != [used]:
        raise ValueError('RR read another exposure than the frame used')


def validate(directory: Path, require_host=False):
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["schema"] not in (1, 2) or manifest["mode"] not in MODES:
        raise ValueError("Unsupported capture schema/mode")
    mode = manifest["mode"]
    color_name, timing_stages, denoisers = MODES[mode]
    if not isinstance(manifest["dynamic_motion_validated"], bool):
        raise ValueError("Realtime capture must state whether its producer validated dynamic motion")
    if manifest["spp"] != 1:
        raise ValueError("Realtime capture must not claim accumulation")
    denoiser = manifest.get("denoiser")
    if (denoiser not in denoisers) if denoisers else (denoiser is not None):
        raise ValueError(f"{mode} capture must state one of the denoisers {denoisers or (None,)}")
    if manifest.get("sun_shadow_denoiser") not in ((None, SUN_SHADOW_DENOISER) if denoiser else (None,)):
        raise ValueError(f"{mode} capture states an unexpected sun shadow denoiser")
    names = {resource["name"] for resource in manifest["resources"]}
    if len(names) != len(manifest['resources']):
        raise ValueError('Duplicate resource name')
    if mode in UPSCALING:
        label, formats, _ = UPSCALING[mode]
        if manifest['schema'] != 2:
            raise ValueError(f'{label} requires schema 2')
        if not formats.keys() <= names:
            raise ValueError(f'Missing {label} resource')
        validate_frame_jitter(manifest, label)
    if mode == 'RealtimeDLSSRR':
        validate_rr_metadata(manifest, names)
    if (BLOOM in names) == ('Bloom' in manifest.get('unavailable_stages', ['Bloom'])):
        raise ValueError('Bloom radiance must be exported exactly when Bloom is available')
    if mode == 'RealtimeDLSSSR':
        sr = manifest['super_resolution']
        if not sr['provider'] or sr['quality'] not in ('Quality', 'Balanced', 'Performance'):
            raise ValueError('Invalid SR provider/quality')
        if (sr['depth_encoding'] != 'forward_projection_0_1_far_1' or
                sr['motion_convention'] != 'current_to_previous_render_pixels_without_jitter' or
                sr['exposure_policy'] != 'explicit_1_preexposure_1_scale_1'):
            raise ValueError('Unexpected SR input semantics')
    if denoiser:
        confidence = {f"history_confidence_layer{layer}" for layer in range(NATIVE_LAYER_COUNT)}
        if not confidence <= names or "noisy_radiance" in names:
            raise ValueError("Native capture must contain every layer's history confidence and no noisy HDR")
    files = {}
    color = None
    exposures = {}
    for resource in manifest["resources"] + [manifest["layers"]]:
        path = directory / resource["file"]
        if path.resolve().parent != directory.resolve():
            raise ValueError("Resource must be inside capture directory")
        data = path.read_bytes()
        if len(data) != resource["bytes"]:
            raise ValueError(f"Truncated resource: {path.name}")
        files[path.name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if "format" not in resource:
            continue
        if len(data) != resource["row_bytes"] * resource["height"] * resource["slices"]:
            raise ValueError(f"Invalid row/slice layout: {path.name}")
        fmt = resource["format"]
        if not resource.get('valid', False):
            raise ValueError('Invalid current-frame resource')
        if manifest['schema'] == 2 and resource.get('frame_index') != manifest['frame_index']:
            raise ValueError('Resource refers to another frame')
        if mode in UPSCALING and resource['name'] in UPSCALING[mode][1]:
            validate_layout(manifest, resource, *UPSCALING[mode])
        if resource['name'] == BLOOM:
            validate_layout(manifest, resource, 'Bloom', BLOOM_FORMATS, (BLOOM,))
        if resource['name'] in AE_FORMATS:
            if (fmt != AE_FORMATS[resource['name']] or [resource['width'], resource['height']] != AE_EXTENTS[resource['name']] or
                    resource['slices'] != 1 or resource['row_bytes'] != resource['width'] * PIXEL_BYTES[fmt]):
                raise ValueError(f'Invalid auto exposure layout: {resource["name"]}')
        if fmt in FLOAT_FORMATS:
            values = [v[0] for v in struct.iter_unpack("<e" if "16" in fmt else "<f", data)]
            if not all(math.isfinite(v) for v in values):
                raise ValueError(f"Nonfinite current-frame signal: {path.name}")
            files[path.name].update(min=min(values), max=max(values))
            if resource['name'] == 'sr_depth' and not all(0 <= v <= 1 for v in values):
                raise ValueError('SR depth outside forward projection range')
            if mode == 'RealtimeDLSSRR':
                validate_rr_values(resource['name'], values, manifest['ray_reconstruction'])
            if resource["name"] == color_name:
                color = (resource, values)
            if resource['name'] in AE_FORMATS or resource['name'] == RR_EXPOSURE:
                exposures[resource['name']] = values
    validate_auto_exposure(manifest, names, exposures)
    if color is None:
        raise ValueError(f"Missing {color_name} HDR")
    timing = manifest["gpu_timing"]
    if timing["available"]:
        if timing["source_frame"] != manifest["frame_index"]:
            raise ValueError("GPU timings refer to another view frame")
        for stage in timing_stages:
            if timing["milliseconds"][stage] is None or timing["milliseconds"][stage] < 0:
                raise ValueError(f"Missing {mode} timing: {stage}")
    if require_host:
        host = json.loads((directory / "host.json").read_text())
        if manifest['schema'] == 2 and host.get('view_frame_index') != manifest['frame_index']:
            raise ValueError('Host metadata refers to another view frame')
        if host["effective"] != mode or host["last_result"] != "ok" or host["reference_samples"] != 0:
            raise ValueError(f"Host did not execute {mode} successfully")
        if host["scene_execution_frame"] != host["executed_frame"]:
            raise ValueError("Host view and scene are from different frames")
        for key, expected in (("scene_gpu_timing", host["scene_prepare_index"]),
                              ("composite_gpu_timing", host["executed_frame"])):
            if host[key]["available"] and host[key]["source_frame"] != expected:
                raise ValueError(f"Wrong source frame: {key}")
    resource, values = color
    width, height = resource["width"], resource["height"]
    with (directory / f"{color_name}.pfm").open("wb") as output:
        output.write(f"PF\n{width} {height}\n-1.0\n".encode("ascii"))
        for y in reversed(range(height)):
            for x in range(width):
                pixel = (y * width + x) * 4
                output.write(struct.pack("<3f", *values[pixel:pixel + 3]))
    report = {"frame_index": manifest["frame_index"], "dynamic_motion_validated": manifest["dynamic_motion_validated"],
              "resources": files}
    (directory / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--require-host", action="store_true")
    args = parser.parse_args()
    print(json.dumps(validate(args.directory, args.require_host), indent=2))
