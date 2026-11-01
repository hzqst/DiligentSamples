#!/usr/bin/env python3
"""Validate a Raw readback, hash all resources and convert its noisy RGB to PFM.

This checks storage/finite values and frame provenance, not temporal correctness.
Packed surface records remain shader-version-specific; inactive records are undefined.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def validate(directory: Path, require_host=False):
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["schema"] != 1 or manifest["mode"] != "RealtimeRaw":
        raise ValueError("Unsupported capture schema/mode")
    if not isinstance(manifest["dynamic_motion_validated"], bool):
        raise ValueError("Raw capture must state whether its producer validated dynamic motion")
    if manifest["spp"] != 1:
        raise ValueError("Raw capture must not claim accumulation")
    files = {}
    noisy = None
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
        if fmt in ("TEX_FORMAT_RGBA16_FLOAT", "TEX_FORMAT_R32_FLOAT"):
            values = [v[0] for v in struct.iter_unpack("<e" if "16" in fmt else "<f", data)]
            if not all(math.isfinite(v) for v in values):
                raise ValueError(f"Nonfinite current-frame signal: {path.name}")
            files[path.name].update(min=min(values), max=max(values))
            if resource["name"] == "noisy_radiance":
                noisy = (resource, values)
    if noisy is None:
        raise ValueError("Missing noisy HDR")
    timing = manifest["gpu_timing"]
    if timing["available"]:
        if timing["source_frame"] != manifest["frame_index"]:
            raise ValueError("GPU timings refer to another view frame")
        for stage in ("BUILD", "FILL", "Guides", "Merge", "ToneMapping"):
            if timing["milliseconds"][stage] is None or timing["milliseconds"][stage] < 0:
                raise ValueError(f"Missing Raw timing: {stage}")
    if require_host:
        host = json.loads((directory / "host.json").read_text())
        if host["effective"] != "RealtimeRaw" or host["last_result"] != "ok" or host["reference_samples"] != 0:
            raise ValueError("Host did not execute Raw successfully")
        if host["scene_execution_frame"] != host["executed_frame"]:
            raise ValueError("Host view and scene are from different frames")
        for key, expected in (("scene_gpu_timing", host["scene_prepare_index"]),
                              ("composite_gpu_timing", host["executed_frame"])):
            if host[key]["available"] and host[key]["source_frame"] != expected:
                raise ValueError(f"Wrong source frame: {key}")
    resource, values = noisy
    width, height = resource["width"], resource["height"]
    with (directory / "noisy_radiance.pfm").open("wb") as output:
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
