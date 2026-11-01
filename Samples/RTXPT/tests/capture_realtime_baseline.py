"""Capture a frozen realtime denoiser baseline and its temporal metrics (Python standard library only).

The denoiser is measured against the realtime estimator's own expectation: the mean of many
undenoised frames rendered with the same tracer settings. A Reference image, when given, only
reports the realtime estimator's bias; it is not the denoiser's target.
"""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import time

from capture_reference_baseline import asset_hashes, difference, git, read_pfm, sha256

# Rec. 709 luminance weights for the adjacent-frame stability metric.
LUMINANCE_WEIGHTS = (0.2126, 0.7152, 0.0722)
# Frames rendered after the last capture so the process exits through its normal shutdown path.
EXIT_MARGIN_FRAMES = 16


def luminance(image):
    values = image[2]
    return [sum(weight * values[i + c] for c, weight in enumerate(LUMINANCE_WEIGHTS))
            for i in range(0, len(values), 3)]


def adjacent_luminance_mae(lhs, rhs):
    a, b = luminance(lhs), luminance(rhs)
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def run_sample(args, label, denoiser, start, count, sequence):
    prefix = args.output / label
    capture_path = str(prefix) if sequence else str(prefix) + ".pfm"
    command = [
        str(args.exe), "--mode", args.api, "--scene", args.scene,
        "--realtime", "true", "--embedded_reference", "false",
        "--show_ui", "false", "--break_on_error", "false",
        "--width", str(args.width), "--height", str(args.height),
        "--asyncPrefix", "false", "--async_queue", "false", "--sharc", "false",
        "--spp", "1", "--subsampling", "0", "--aa", "off", "--nee_type", str(args.nee_type),
        "--nrd", "true" if denoiser else "false",
        "--hdr_capture_path", capture_path,
        "--hdr_capture_start", str(start), "--hdr_capture_count", str(count),
        "--hdr_capture_sequence", "true" if sequence else "false",
        "--exit_after_frames", str(start + count + EXIT_MARGIN_FRAMES),
    ]
    if denoiser:
        command += ["--nrd_method", denoiser]
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    started = time.monotonic()
    with prefix.with_suffix(".log").open("wb") as log:
        result = subprocess.run(command, cwd=args.sample_dir, stdout=log, stderr=subprocess.STDOUT,
                                timeout=args.timeout, startupinfo=startup)
    record = {"command": command, "cwd": str(args.sample_dir), "exit_code": result.returncode,
              "wall_seconds": time.monotonic() - started}
    if result.returncode:
        raise RuntimeError(f"{label}: process exited {result.returncode}")
    if not sequence:
        image = read_pfm(Path(capture_path))
        record["image"] = {"sha256": sha256(Path(capture_path)), "frames": count}
        return record, image
    frames = sorted(args.output.glob(f"{label}.*.pfm"), key=lambda p: int(p.name.split(".")[-2]))
    if len(frames) != count:
        raise ValueError(f"{label}: expected {count} sequence frames, found {len(frames)}")
    record["frames"] = [{"file": p.name, "sha256": sha256(p)} for p in frames]
    return record, [read_pfm(p) for p in frames]


def sequence_metrics(images, target):
    per_frame = [difference(image, target) for image in images]
    adjacent = [adjacent_luminance_mae(images[i - 1], images[i]) for i in range(1, len(images))]
    return {"error_vs_mean": per_frame, "adjacent_luminance_mae": adjacent}


def tail(values, fraction):
    count = max(1, int(len(values) * fraction))
    return values[-count:]


def summarize(metrics, tail_fraction):
    rmse = [m["rmse"] for m in metrics["error_vs_mean"]]
    adjacent = metrics["adjacent_luminance_mae"]
    return {
        "tail_mean_rmse": sum(tail(rmse, tail_fraction)) / len(tail(rmse, tail_fraction)),
        "tail_max_rmse": max(tail(rmse, tail_fraction)),
        "tail_mean_adjacent_luminance_mae": sum(tail(adjacent, tail_fraction)) / len(tail(adjacent, tail_fraction)),
        "first_rmse": rmse[0], "last_rmse": rmse[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sample-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--scene", default="convergence-test.scene.json")
    parser.add_argument("--api", choices=("d3d12", "vk"), default="d3d12")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--nee-type", type=int, choices=(0, 1), default=1)
    parser.add_argument("--denoiser", choices=("relax", "reblur"), default="relax")
    parser.add_argument("--warmup", type=int, default=64, help="Frames before the mean capture starts.")
    parser.add_argument("--mean-frames", type=int, default=4096)
    parser.add_argument("--frames", type=int, default=128, help="Sequence length from the first frame.")
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--tail-fraction", type=float, default=0.5)
    parser.add_argument("--reference", type=Path, help="Optional converged Reference PFM for estimator bias.")
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if min(args.width, args.height, args.mean_frames, args.frames, args.repeats, args.timeout) <= 0 or args.warmup < 0:
        parser.error("Sizes, frame counts, repeats and timeout must be positive")
    if not 0 < args.tail_fraction <= 1:
        parser.error("tail-fraction must be in (0, 1]")
    args.output = args.output.resolve()
    # A fresh directory prevents stale files from masquerading as a successful capture.
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {"status": "running", "api": args.api, "denoiser": args.denoiser, "runs": {}}
    report = args.output / "manifest.json"
    try:
        args.exe = args.exe.resolve(strict=True)
        args.sample_dir = args.sample_dir.resolve(strict=True)
        scene_path = (args.sample_dir / "assets" / args.scene).resolve(strict=True)
        manifest.update({
            "executable": str(args.exe), "executable_sha256": sha256(args.exe),
            "scene": str(scene_path), "samples_head": git(args.sample_dir, "rev-parse", "HEAD"),
            "working_tree": git(args.sample_dir, "status", "--short"),
            "asset_hashes": asset_hashes(args.sample_dir, scene_path),
            "runtime_dlls": {p.name: sha256(p) for p in args.exe.parent.glob("*.dll")},
            "settings": {"width": args.width, "height": args.height, "nee_type": args.nee_type,
                         "warmup": args.warmup, "mean_frames": args.mean_frames, "frames": args.frames,
                         "repeats": args.repeats, "tail_fraction": args.tail_fraction},
        })
        manifest["runs"]["mean"], mean = run_sample(args, "mean", None, args.warmup, args.mean_frames, False)
        if args.reference:
            manifest["estimator_bias_vs_reference"] = difference(mean, read_pfm(args.reference.resolve(strict=True)))
        manifest["runs"]["noisy"], noisy = run_sample(args, "noisy", None, 1, args.frames, True)
        manifest["noisy"] = summarize(sequence_metrics(noisy, mean), args.tail_fraction)
        denoised = []
        for index in range(args.repeats):
            label = f"{args.denoiser}{index}"
            manifest["runs"][label], images = run_sample(args, label, args.denoiser, 1, args.frames, True)
            metrics = sequence_metrics(images, mean)
            manifest[label] = {"summary": summarize(metrics, args.tail_fraction), "metrics": metrics}
            denoised.append(images)
        manifest["repeat_difference"] = [
            max(difference(a, b)["max_abs"] for a, b in zip(denoised[0], other)) for other in denoised[1:]
        ]
        manifest["status"] = "passed"
    except Exception as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        report.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Realtime baseline {manifest['status']}: {report}", flush=True)


if __name__ == "__main__":
    main()
