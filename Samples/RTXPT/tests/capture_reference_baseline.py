"""Capture frozen Reference signals and compare repeat runs (Python standard library only)."""

import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def git(sample_dir, *args):
    return subprocess.check_output(
        ["git", "-C", str(sample_dir), *args], encoding="utf-8"
    ).strip()


def asset_hashes(sample_dir, scene_path):
    assets = sample_dir / "assets"
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    paths = {scene_path}
    for model in scene["models"]:
        path = (assets / model).resolve(strict=True)
        paths.add(path)
        if path.suffix.lower() == ".gltf":
            gltf = json.loads(path.read_text(encoding="utf-8"))
            for item in gltf.get("buffers", []) + gltf.get("images", []):
                uri = item.get("uri", "")
                if uri and not uri.startswith("data:"):
                    paths.add((path.parent / uri).resolve(strict=True))

    def visit(node):
        if isinstance(node, dict):
            if "path" in node:
                paths.add((assets / node["path"]).resolve(strict=True))
            for value in node.values():
                visit(value)
        elif isinstance(node, list):
            for value in node:
                visit(value)

    visit(scene.get("graph", []))
    return {str(path): sha256(path) for path in sorted(paths)}


def read_pfm(path):
    with path.open("rb") as source:
        if source.readline().strip() != b"PF":
            raise ValueError(f"Not an RGB PFM: {path}")
        width, height = map(int, source.readline().split())
        scale = float(source.readline())
        if min(width, height) <= 0 or abs(scale) != 1:
            raise ValueError(f"Unsupported PFM dimensions or scale: {path}")
        values = array.array("f")
        values.frombytes(source.read())
    if (scale < 0) != (sys.byteorder == "little"):
        values.byteswap()
    if len(values) != width * height * 3 or not all(map(math.isfinite, values)):
        raise ValueError(f"Incomplete or nonfinite capture: {path}")
    return width, height, values


def difference(lhs, rhs):
    if lhs[:2] != rhs[:2]:
        raise ValueError("Capture dimensions differ")
    errors = sorted(abs(a - b) for a, b in zip(lhs[2], rhs[2]))
    return {
        "rmse": math.sqrt(sum(value * value for value in errors) / len(errors)),
        "p99_abs": errors[int((len(errors) - 1) * 0.99)],
        "max_abs": errors[-1],
    }


def capture(args, label, samples, scene_time):
    prefix = args.output / label
    command = [
        str(args.exe), "--mode", args.api, "--scene", args.scene,
        "--realtime", "false", "--embedded_reference", "false",
        "--show_ui", "false", "--break_on_error", "false",
        "--width", str(args.width), "--height", str(args.height),
        "--asyncPrefix", "false", "--async_queue", "false", "--sharc", "false",
        "--nee_type", str(args.nee_type),
        "--reference_capture_prefix", str(prefix),
        "--reference_capture_samples", str(samples),
        "--reference_time", str(scene_time),
        "--reference_seed_offset", str(args.seed_offset),
        "--gpu_profile", "true", "--gpu_profile_csv", str(prefix) + ".gpu.csv",
        "--exit_after_frames", str(samples + 16),
    ]
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    started = time.monotonic()
    with prefix.with_suffix(".log").open("wb") as log:
        result = subprocess.run(command, cwd=args.sample_dir, stdout=log,
                                stderr=subprocess.STDOUT, timeout=args.timeout,
                                startupinfo=startup)
    record = {"command": command, "cwd": str(args.sample_dir),
              "exit_code": result.returncode, "wall_seconds": time.monotonic() - started}
    if result.returncode:
        raise RuntimeError(f"{label}: process exited {result.returncode}")
    metadata = json.loads(prefix.with_suffix(".json").read_text())
    if not metadata["capture_complete"] or metadata["sample_index"] != samples:
        raise ValueError(f"{label}: wrong or incomplete reference frame")
    if metadata["auto_exposure"] or metadata["scene_time"] != scene_time:
        raise ValueError(f"{label}: exposure/time is not frozen")
    images = {}
    record["capture"] = metadata
    record["images"] = {}
    for signal in ("noisy", "accumulated", "display"):
        path = Path(str(prefix) + f".{signal}.pfm")
        image = read_pfm(path)
        if image[:2] != (args.width, args.height):
            raise ValueError(f"{label}: unexpected {signal} size")
        images[signal] = image
        record["images"][signal] = {
            "sha256": sha256(path), "min": min(image[2]), "max": max(image[2]),
            "mean": sum(image[2]) / len(image[2]),
        }
    if record["images"]["accumulated"]["max"] <= 0:
        raise ValueError(f"{label}: black accumulated reference")
    if samples == 1:
        equality = difference(images["noisy"], images["accumulated"])
        record["first_sample_difference"] = equality
        if equality["max_abs"] != 0:
            raise ValueError(f"{label}: first accumulated sample differs from noisy input")
    return record, images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sample-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--scene", default="convergence-test.scene.json")
    parser.add_argument("--api", choices=("d3d12", "vk"), default="d3d12")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--samples", type=int, default=256)
    parser.add_argument("--time", type=float, default=0.0)
    parser.add_argument("--second-time", type=float)
    parser.add_argument("--seed-offset", type=int, default=0)
    parser.add_argument("--nee-type", type=int, choices=(0, 1, 2), default=0)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--repeat-tolerance", type=float, default=0.0,
                        help="Maximum absolute repeat error, fixed before running (same GPU/API).")
    args = parser.parse_args()
    if min(args.width, args.height, args.samples, args.timeout) <= 0:
        parser.error("Sizes, samples and timeout must be positive")
    if not 0 <= args.seed_offset <= 0xFFFFFFFF:
        parser.error("seed-offset must fit uint32")
    for value in (args.time, args.second_time, args.repeat_tolerance):
        if value is not None and (not math.isfinite(value) or value < 0):
            parser.error("Times and tolerance must be finite and nonnegative")
    args.output = args.output.resolve()
    # A fresh directory prevents stale files from masquerading as a successful capture.
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {
        "status": "running", "api": args.api, "executable": str(args.exe),
        "repeat_tolerance": args.repeat_tolerance, "runs": {},
    }
    report = args.output / "manifest.json"
    try:
        args.exe = args.exe.resolve(strict=True)
        args.sample_dir = args.sample_dir.resolve(strict=True)
        scene_path = (args.sample_dir / "assets" / args.scene).resolve(strict=True)
        changed = set(git(args.sample_dir, "diff", "--name-only", "--relative", "HEAD", "--", ".").splitlines())
        changed.update(git(args.sample_dir, "ls-files", "--others", "--exclude-standard").splitlines())
        index_diff = subprocess.check_output(["git", "-C", str(args.sample_dir), "diff", "--cached", "--binary", "--", "."])
        manifest.update({
            "executable": str(args.exe), "executable_sha256": sha256(args.exe),
            "scene": str(scene_path), "scene_sha256": sha256(scene_path),
            "samples_head": git(args.sample_dir, "rev-parse", "HEAD"),
            "asset_hashes": asset_hashes(args.sample_dir, scene_path),
            "working_tree": git(args.sample_dir, "status", "--short"),
            "changed_source_hashes": {
                name: sha256(args.sample_dir / name) if (args.sample_dir / name).is_file() else None
                for name in sorted(changed)
            },
            "index_diff_sha256": hashlib.sha256(index_diff).hexdigest(),
            "runtime_dlls": {p.name: sha256(p) for p in args.exe.parent.glob("*.dll")},
        })
        first, _ = capture(args, "first", 1, args.time)
        manifest["runs"]["first"] = first
        original, images = capture(args, "reference", args.samples, args.time)
        manifest["runs"]["reference"] = original
        repeated, repeat_images = capture(args, "repeat", args.samples, args.time)
        manifest["runs"]["repeat"] = repeated
        if original["capture"] != repeated["capture"]:
            raise ValueError("Repeat metadata differs: camera, sampling or time was not frozen")
        manifest["repeat_difference"] = {
            signal: difference(images[signal], repeat_images[signal]) for signal in images
        }
        if any(metric["max_abs"] > args.repeat_tolerance
               for metric in manifest["repeat_difference"].values()):
            raise ValueError("Repeat exceeds the preselected tolerance; inspect inputs before relaxing it")
        if args.second_time is not None:
            later, later_images = capture(args, "second_time", args.samples, args.second_time)
            manifest["runs"]["second_time"] = later
            metric = difference(images["accumulated"], later_images["accumulated"])
            manifest["animation_time_difference"] = metric
            if metric["max_abs"] == 0:
                raise ValueError("Second animation time produced identical radiance")
        manifest["status"] = "passed"
    except Exception as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        report.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Reference baseline {manifest['status']}: {report}", flush=True)


if __name__ == "__main__":
    main()
