#!/usr/bin/env python3
"""Validate a realtime readback, hash all resources and convert its color to PFM.

Raw exports its noisy current-frame HDR, Native its denoised HDR and each layer's history confidence.

This checks storage/finite values and frame provenance, not temporal correctness.
Packed surface records remain shader-version-specific; inactive records are undefined.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


# Per mode: HDR color resource, required GPU timing stages and denoiser.
MODES = {
    "RealtimeRaw": ("noisy_radiance", ("BUILD", "FILL", "Guides", "Merge", "ToneMapping"), None),
    "RealtimeNative": ("denoised_radiance", ("BUILD", "FILL", "Guides", "NRD", "ToneMapping"), "NRD_RELAX"),
    "RealtimeDLSSSR": ("upscaled_radiance", ("BUILD", "FILL", "Guides", "NRD", "SRInputs", "SR", "ToneMapping"), "NRD_RELAX"),
}
NATIVE_LAYER_COUNT = 3


def validate(directory: Path, require_host=False):
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["schema"] not in (1, 2) or manifest["mode"] not in MODES:
        raise ValueError("Unsupported capture schema/mode")
    mode = manifest["mode"]
    color_name, timing_stages, denoiser = MODES[mode]
    if not isinstance(manifest["dynamic_motion_validated"], bool):
        raise ValueError("Realtime capture must state whether its producer validated dynamic motion")
    if manifest["spp"] != 1:
        raise ValueError("Realtime capture must not claim accumulation")
    if manifest.get("denoiser") != denoiser:
        raise ValueError(f"{mode} capture must state denoiser {denoiser}")
    names = {resource["name"] for resource in manifest["resources"]}
    if len(names) != len(manifest['resources']):
        raise ValueError('Duplicate resource name')
    sr_formats = {'denoised_radiance': 'TEX_FORMAT_RGBA16_FLOAT',
                  'upscaled_radiance': 'TEX_FORMAT_RGBA16_FLOAT',
                  'sr_depth': 'TEX_FORMAT_R32_FLOAT', 'sr_motion': 'TEX_FORMAT_RG16_FLOAT',
                  'sr_ignore_history': 'TEX_FORMAT_R8_UNORM', 'display': 'TEX_FORMAT_RGBA8_TYPELESS'}
    if mode == 'RealtimeDLSSSR':
        if manifest['schema'] != 2:
            raise ValueError('SR requires schema 2')
        if not sr_formats.keys() <= names:
            raise ValueError('Missing SR resource')
        if manifest['frame_index'] < 1 or manifest['sample_base_index'] != manifest['frame_index'] - 1:
            raise ValueError('Invalid successful frame/jitter index')
        if len(manifest['jitter']) != 2 or not all(math.isfinite(v) for v in manifest['jitter']):
            raise ValueError('Invalid SR jitter')
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
        if mode == 'RealtimeDLSSSR' and resource['name'] in sr_formats:
            name = resource['name']
            if fmt != sr_formats[name]:
                raise ValueError(f'Unexpected SR format: {name}')
            extent = manifest['display_size' if name in ('display', 'upscaled_radiance') else 'render_size']
            if [resource['width'], resource['height']] != extent or resource['slices'] != 1:
                raise ValueError(f'Invalid SR extent: {name}')
            pixel_bytes = {'TEX_FORMAT_RGBA16_FLOAT': 8, 'TEX_FORMAT_R32_FLOAT': 4,
                           'TEX_FORMAT_RG16_FLOAT': 4, 'TEX_FORMAT_R8_UNORM': 1,
                           'TEX_FORMAT_RGBA8_TYPELESS': 4}[fmt]
            if resource['row_bytes'] != resource['width'] * pixel_bytes:
                raise ValueError(f'Invalid SR row format: {name}')
        if fmt in ("TEX_FORMAT_RGBA16_FLOAT", "TEX_FORMAT_RG16_FLOAT", "TEX_FORMAT_R32_FLOAT"):
            values = [v[0] for v in struct.iter_unpack("<e" if "16" in fmt else "<f", data)]
            if not all(math.isfinite(v) for v in values):
                raise ValueError(f"Nonfinite current-frame signal: {path.name}")
            files[path.name].update(min=min(values), max=max(values))
            if resource['name'] == 'sr_depth' and not all(0 <= v <= 1 for v in values):
                raise ValueError('SR depth outside forward projection range')
            if resource["name"] == color_name:
                color = (resource, values)
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
