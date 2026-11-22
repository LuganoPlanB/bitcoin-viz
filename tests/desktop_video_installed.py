# SPDX-License-Identifier: GPL-3.0-or-later
"""Record from a staged desktop viewer through native X11 input."""

import argparse
import contextlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from desktop_overlay import Viewer, close_window, display, wait_for
from desktop_video_readback import clip, distance


@contextlib.contextmanager
def native_viewer(binary, evidence, env, dimensions=(1200, 760), profile=False):
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="viz-installed-video-") as temp:
        work = Path(temp)
        data = work / "data"
        env = dict(env, XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(work / "config"),
                   XDG_CACHE_HOME=str(work / "cache"), SDL_VIDEODRIVER="x11",
                   SDL_VIDEO_X11_XRANDR="0", LIBGL_ALWAYS_SOFTWARE="1")
        env.pop("XAUTHORITY", None)
        with display(env, 96, evidence / "xvfb.log", dimensions) as viewer_env, (
            evidence / "viewer.log"
        ).open("w") as log, subprocess.Popen(
            [binary, "--smoke-test", "--video-profile"] if profile else [binary, "--demo"],
            cwd=work, env=viewer_env, stdout=log, stderr=log,
        ) as process:
            try:
                view = Viewer(process, "xdotool", viewer_env, evidence, 100)
                view.resize(*dimensions)
                view.until_log("Scene: Gource / state")
                yield view, data / "Bitcoin Viz" / "Observer" / "recordings"
                assert "Scene error:" not in view.log()
                close_window(viewer_env["DISPLAY"], view.window)
                assert process.wait(8) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(5)


def take(view, prefs, name, include_overlay):
    files = set(prefs.glob("*.avi"))
    log = view.log()
    if include_overlay:
        view.click(32, 198)  # Include overlay is initially off for each renderer.
    view.click(42, 222)  # Start
    view.until_log("Video capture: Started", log.count("Video capture: Started") + 1)
    view.pointer(view.width - 24, view.height - 24)
    composite = view.capture(f"{name}-panel-reference")
    time.sleep(1.5)
    view.key("r")
    view.until_log("Video capture: Stopped", log.count("Video capture: Stopped") + 1)
    saved, = set(prefs.glob("*.avi")) - files
    path = view.destination / (name + ".avi")
    shutil.move(saved, path)
    return path, composite


def check_pixels(view, frame, scene, panel, label, included):
    # The scene label must be bright enough to be independently recognizable
    # and must survive both overlay settings. The top-left panel corner lies
    # outside the label patch and should match the selected source only.
    label_pixels = view.pixels_in(scene, *label)
    assert sum(min(rgb) > 180 for rgb in zip(label_pixels[::3], label_pixels[1::3], label_pixels[2::3])) > 100
    label_difference = distance(view.pixels_in(frame, *label), label_pixels)
    corner = (18, 15, 100, 52)
    expected, other = (panel, scene) if included else (scene, panel)
    corner_pixels = view.pixels_in(frame, *corner)
    corner_difference = distance(corner_pixels, view.pixels_in(expected, *corner))
    other_difference = distance(corner_pixels, view.pixels_in(other, *corner))
    print(f"Label difference {label_difference:.4%}; panel corner {corner_difference:.4%} "
          f"from expected and {other_difference:.4%} from alternative", flush=True)
    assert label_difference < 0.035, "The decoded scene label differs from the native reference"
    assert corner_difference < 0.035 and corner_difference + 0.10 < other_difference, "Wrong panel pixels"
    assert distance(frame, expected) < 0.02, "Decoded frame differs from the selected picture"


def renderer(view, prefs, name, label):
    view.click(view.width - 24, view.height - 24, scale=False)
    view.key("space")
    view.key("F1")
    previous = None

    def settled():
        nonlocal previous
        frame = view.capture(f"{name}-scene-reference")
        stable = previous is not None and distance(frame, previous) < 0.002
        previous = frame
        return frame if stable else None

    scene = wait_for(f"{name} scene and labels to settle", settled)
    view.key("F1")
    view.click(148, 100)  # Recording tab stays visible for both takes.
    for included in (False, True):
        case = f"{name}-{'overlay' if included else 'scene'}"
        path, panel = take(view, prefs, case, included)
        frame = clip(path, view.width, view.height)
        subprocess.run([
            "ffmpeg", "-nostdin", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
            "-video_size", f"{view.width}x{view.height}", "-i", "-", "-frames:v", "1", "-threads", "1",
            view.destination / (case + "-seek.png"),
        ], input=frame, check=True, timeout=10)
        check_pixels(view, frame, scene, panel, label, included)
    view.click(32, 198)  # Reset Include overlay for the next renderer.
    view.click(44, 100)  # Scene tab
    return path


def play(view, movie):
    player_log = view.destination / "ffplay.log"
    with player_log.open("w") as log, subprocess.Popen([
        "ffplay", "-loglevel", "info", "-autoexit", "-an", "-x", "640", "-y", "400",
        "-window_title", "Independent AVI playback", movie,
    ], env=view.env, stdout=log, stderr=log) as player:
        try:
            window = wait_for("the independent player window", lambda: view.xdo(
                "search", "--onlyvisible", "--pid", player.pid, "--name", "^Independent AVI playback$", check=False,
            ))
            assert player.poll() is None
            time.sleep(0.25)
            subprocess.run([
                "ffmpeg", "-nostdin", "-v", "error", "-f", "x11grab", "-window_id", window,
                "-video_size", "640x400", "-i", view.env["DISPLAY"], "-frames:v", "1", "-threads", "1",
                view.destination / "ffplay.png",
            ], env=view.env, check=True, timeout=8)
            assert player.wait(timeout=10) == 0
        finally:
            if player.poll() is None:
                player.kill()
                player.wait(5)
    playback = player_log.read_text()
    assert "Video: mjpeg" in playback and "M-V:" in playback
    print(f"ffplay displayed {movie.name} and exited after playback", flush=True)


def check_playback(binary, evidence, env):
    with native_viewer(binary, evidence, env) as (view, prefs):
        # Use the protocol projection to retain an asymmetric Gource scene
        # and compare its centered time label, away from the overlay.
        view.click(195, 160)
        view.key("Down")
        view.key("Return")
        view.until_log("Scene: Gource / protocol")
        view.click(175, 268)
        view.type("284960")
        view.click(72, 336)
        view.until_log("Scene appearance updated: protocol")
        renderer(view, prefs, "gource", (400, 14, 400, 26))

        view.click(112, 129)
        view.until_log("Scene: Logstalgia / traffic")
        view.click(195, 160)
        view.key("Down")
        view.key("Return")
        view.until_log("Scene: Logstalgia / mempool")
        view.click(175, 268)
        view.type("343055")
        view.click(72, 301)
        view.until_log("Scene appearance updated: mempool")
        # The "Bitcoin" heading and mempool URI are to the right of the panel.
        movie = renderer(view, prefs, "logstalgia", (800, 0, 280, 46))
        play(view, movie)


def percentiles(samples):
    assert samples, "No timings collected"
    ordered = sorted(samples)
    return {f"p{p}": ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95)}


def measure(binary, evidence, env, recording):
    with native_viewer(binary, evidence, env, (1920, 1080), profile=True) as (view, prefs):
        view.key("F1")  # Both phases have the same unobstructed demo scene.
        time.sleep(2)
        key = "r" if recording else "F9"
        start = time.monotonic()
        view.xdo("key", key)
        view.until_log(f"Video profile started: {'recording' if recording else 'idle'}")
        time.sleep(max(0, start + 30 - time.monotonic()))
        view.xdo("key", key)
        view.until_log("Video profile: ")
        lines = view.log().splitlines()
        renderer, = [line.removeprefix("OpenGL: ") for line in lines if line.startswith("OpenGL: ")]
        raw, = [json.loads(line.removeprefix("Video profile: ")) for line in lines if line.startswith("Video profile: ")]
        assert raw["recording"] is recording and abs(raw["elapsed_ms"] - 30000) < 600
        (evidence / "timings.json").write_text(json.dumps(raw, indent=2) + "\n")
        report = {
            "phase": "recording" if recording else "idle", "drawable": [view.width, view.height],
            "renderer": renderer, "software_gl": "llvmpipe" in renderer.lower() or "softpipe" in renderer.lower(),
            "elapsed_s": raw["elapsed_ms"] / 1000, "render_frame_intervals": len(raw["frame_ms"]),
            "render_frame_ms": percentiles(raw["frame_ms"]),
        }
        if recording:
            saved, = prefs.glob("*.avi")
            avi = evidence / "demo-1920x1080.avi"
            shutil.move(saved, avi)
            probe = json.loads(subprocess.check_output([
                "ffprobe", "-v", "error", "-show_entries",
                "stream=codec_name,codec_tag_string,r_frame_rate,width,height,duration,nb_frames", "-of", "json", avi,
            ], timeout=15))
            stream, = probe["streams"]
            assert (stream["width"], stream["height"], stream["r_frame_rate"], stream["codec_tag_string"]) == (
                1920, 1080, "30/1", "MJPG",
            )
            frames = int(stream["nb_frames"])
            duration = float(stream["duration"])
            encoded = len(raw["encode_ms"])
            assert frames >= encoded == len(raw["readback_ms"]) > 0 and 1 <= raw["peak_queued"] <= 3
            assert abs(duration - report["elapsed_s"]) <= 1 / 30 + 0.01
            report.update(
                quality=95, fps=30, include_overlay=False, output_frames=frames, sampled_frames=encoded,
                missed_samples=frames - encoded, output_duration_s=duration, output_bytes=avi.stat().st_size,
                bytes_per_second=avi.stat().st_size / duration, peak_queued_buffers=raw["peak_queued"],
                encode_and_write_ms=percentiles(raw["encode_ms"]), readback_and_enqueue_ms=percentiles(raw["readback_ms"]),
            )
        else:
            assert not raw["encode_ms"] and not raw["readback_ms"] and raw["peak_queued"] == 0
        (evidence / "measurements.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("viewer", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--measure", choices=("idle", "recording"))
    args = parser.parse_args()
    if args.measure:
        measure(args.viewer.resolve(strict=True), args.evidence.resolve(), os.environ, args.measure == "recording")
    else:
        check_playback(args.viewer.resolve(strict=True), args.evidence.resolve(), os.environ)
