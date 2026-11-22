# SPDX-License-Identifier: GPL-3.0-or-later
"""Check SDL/OpenGL recording via the native desktop smoke harness."""

import argparse
import contextlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time

from desktop_overlay import Viewer, close_window, display, leaf_center, wait_for, wait_for_fixture
from desktop_session import fixture_env, node_server


@contextlib.contextmanager
def window_manager(binary, env, log):
    # Minimize and restore need an X11 window manager on our private display.
    with log.open("w") as output, subprocess.Popen(
        [binary, "--sm-disable"], env=env, stdout=output, stderr=output,
    ) as manager:
        try:
            yield
        finally:
            manager.terminate()
            manager.wait(6)


def run(*command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25)
    assert result.returncode == 0, f"{command}: {result.stderr.decode(errors='replace')}"
    return result.stdout


def clip(path, width, height):
    probe = json.loads(run(
        "ffprobe", "-v", "error", "-count_frames", "-show_streams", "-show_frames", "-show_entries",
        "stream=codec_name,codec_tag_string,r_frame_rate,width,height,nb_read_frames:frame=width,height",
        "-of", "json", path,
    ))
    streams = probe["streams"]
    assert len(streams) == 1, streams
    stream = streams[0]
    assert (stream["codec_name"], stream["codec_tag_string"], stream["r_frame_rate"]) == (
        "mjpeg", "MJPG", "30/1",
    ), stream
    assert (stream["width"], stream["height"]) == (width, height), stream
    frames = int(stream["nb_read_frames"])
    assert frames >= 4, f"{path}: short take ({frames} frames)"
    assert len(probe["frames"]) == frames and all(
        (frame["width"], frame["height"]) == (width, height) for frame in probe["frames"]
    ), f"{path}: frame geometry changed during the take"
    run("ffmpeg", "-nostdin", "-v", "error", "-xerror", "-threads", "1", "-i", path, "-f", "null", "-")
    # Seek into the final quarter and decode one frame independently of the full pass.
    seek = 3 * frames // 4
    frame = run(
        "ffmpeg", "-nostdin", "-v", "error", "-xerror", "-ss", f"{seek / 30:.6f}", "-threads", "1",
        "-i", path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-threads", "1", "-",
    )
    assert len(frame) == width * height * 3, f"{path}: wrong seek frame dimensions"
    print(f"{path.name}: {width}x{height}, {frames} decoded frames, seek to frame {seek} passed", flush=True)
    return frame


def distance(a, b, tolerance=16):
    # JPEG can shift edge pixels by a few levels. Count only larger differences.
    return sum(abs(x - y) > tolerance for x, y in zip(a, b, strict=True)) / len(a)


def check_picture(view, image, correct, other, name):
    width, height = view.width, view.height
    pitch = width * 3
    patch = lambda frame: view.pixels_in(frame, 23, 17, 250, 300)
    upside_down = b"".join(correct[y * pitch:(y + 1) * pitch] for y in range(height - 1, -1, -1))
    right = distance(image, correct)
    flipped = distance(image, upside_down)
    right_panel = distance(patch(image), patch(correct))
    wrong_panel = distance(patch(image), patch(other))
    print(f"{name}: differing channels image={right:.4%} flipped={flipped:.4%} "
          f"panel={right_panel:.4%} other={wrong_panel:.4%}", flush=True)
    assert right < 0.02 and right + 0.002 < flipped, f"{name}: wrong scene or orientation"
    assert right_panel < 0.025 and right_panel + 0.07 < wrong_panel, f"{name}: wrong overlay pixels"


class Takes:
    def __init__(self, view):
        self.view = view
        self.count = 0

    def start(self, overlay):
        self.count += 1
        before = self.view.log()
        size = f"{self.view.width}x{self.view.height}"
        started = f"Video capture: Started {size}"
        restored = f"Video smoke: restored GL read buffer and pack state ({'composited' if overlay else 'scene'})"
        self.view.key("F7" if overlay else "F6")
        self.view.until_log(started, before.count(started) + 1)
        self.view.until_log(restored, before.count(restored) + 1)
        # Allow at least seven 30 fps slots before exercising the stop path.
        time.sleep(0.25)
        return self.view.destination / f"take-{self.count}.avi"

    def stop(self):
        stopped = "Video capture: Stopped"
        count = self.view.log().count(stopped)
        self.view.key("F8")
        self.view.until_log(stopped, count + 1)

    def capture(self, overlay):
        movie = self.start(overlay)
        self.stop()
        return clip(movie, self.view.width, self.view.height)


def renderer_pictures(view, takes, name, background):
    view.click(view.width - 30, view.height - 30, scale=False)
    view.pointer(view.width - 25, view.height - 26)
    view.key("space")  # Hold the asymmetric fixture scene while taking references.
    view.key("F1")
    # Pausing event playback still lets Gource ease the camera toward its target.
    # Wait for native screenshots to agree before using one as the reference.
    previous = None

    def stable_scene():
        nonlocal previous
        current = view.capture(f"{name}-scene-reference")
        stable = previous is not None and distance(previous, current) < 0.003
        previous = current
        return current if stable else None

    scene = wait_for(f"{name} paused scene to settle", stable_scene)
    view.assert_background(scene, background)
    view.key("F1")
    composite = view.capture(f"{name}-composited-reference")
    image = takes.capture(False)
    check_picture(view, image, scene, composite, f"{name} scene, panel visible")
    image = takes.capture(True)
    check_picture(view, image, composite, scene, f"{name} composited, panel visible")
    view.key("F1")
    image = takes.capture(True)
    check_picture(view, image, scene, composite, f"{name} composited, panel hidden")
    after = view.capture(f"{name}-scene-after")
    view.assert_background(after, background)
    scene_change = distance(scene, after)
    assert scene_change < 0.015, f"{name}: recording changed the rendered scene"
    print(f"{name}: {scene_change:.4%} scene channels changed past JPEG tolerance after recording", flush=True)
    if name == "gource":
        before_camera = leaf_center(after, view.width)
        movie = takes.start(False)
        view.hold("Right", 0.08)
        moved = view.capture("gource-camera-while-recording")
        takes.stop()
        clip(movie, view.width, view.height)
        camera_move = math.dist(before_camera, leaf_center(moved, view.width))
        assert camera_move > 12, "Camera did not respond during recording"
        print(f"gource: camera input moved fixture leaves by {camera_move:.2f}px during recording", flush=True)
    view.key("F1")


def exercise(view, node, data, cookie):
    takes = Takes(view)
    view.resize(801, 601)
    view.until_log("Scene: Gource / state")
    view.click(195, 160)
    view.key("Down")
    view.key("Return")
    view.until_log("Scene: Gource / protocol")
    view.click(175, 268)
    view.type("284960")
    view.click(72, 336)
    view.until_log("Scene appearance updated: protocol")
    renderer_pictures(view, takes, "gource", "284960")

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
    renderer_pictures(view, takes, "logstalgia", "343055")

    # Reuse the same SDL window and capture through both engine and source changes.
    view.click(54, 129)
    view.until_log("Scene: Gource / protocol", 2)
    take = takes.start(False)
    view.click(112, 129)
    view.until_log("Scene: Logstalgia / mempool", 2)
    view.click(96, 100)
    view.click(175, 237)
    view.type(str(cookie))
    view.click(175, 167)
    view.type(node.url)
    view.click(65, 303)
    wait_for("capture source verification", lambda: node.requests == 1)
    view.click(150, 303)
    wait_for_fixture(node, data)
    view.until_log("Scene: Logstalgia / mempool", 3)
    assert view.find_window() == view.window
    assert view.log().count("Video capture: Stopped") == takes.count - 1
    assert "Video capture: Drawable size changed" not in view.log()

    view.resize(835, 623)
    view.until_log("Video capture: Drawable size changed")
    clip(take, 801, 601)
    print("Same-size engine and source switches preserved the take; native resize finalized it", flush=True)

    view.key("F1")
    take = takes.start(True)
    view.key("f")
    full_width, full_height = map(int, view.xdo("getdisplaygeometry").split())
    view.until_log(f"Overlay: {full_width}x{full_height} logical")
    view.until_log("Video capture: Drawable size changed", 2)
    clip(take, 835, 623)
    view.key("f")
    view.until_log("Overlay: 835x623 logical", 2)

    take = takes.start(False)
    view.xdo("windowminimize", view.window)
    view.until_log("Video capture: Window minimized")
    clip(take, view.width, view.height)
    view.xdo("windowmap", view.window)
    view.xdo("windowactivate", "--sync", view.window)
    wait_for("restored native window", lambda: view.find_window() == view.window)
    return takes.start(False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("viewer", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--xdotool", default="xdotool")
    parser.add_argument("--openbox", default="openbox")
    args = parser.parse_args()
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="viz-readback-") as temp, node_server("main", 9061) as node:
        root = Path(temp)
        env, _ = fixture_env(root)
        Path(env["HOME"]).mkdir()
        cookie = node.set_cookie(root / "manual.cookie")
        data = root / "data"
        env.update(XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"), SDL_VIDEODRIVER="x11",
                   SDL_VIDEO_X11_XRANDR="0", LIBGL_ALWAYS_SOFTWARE="1")
        env.pop("XAUTHORITY", None)
        with display(env, 96, destination / "xvfb.log") as viewer_env, window_manager(
            args.openbox, viewer_env, destination / "openbox.log",
        ):
            def manager_ready():
                result = subprocess.run([args.xdotool, "get_num_desktops"], env=viewer_env,
                                        capture_output=True, timeout=5)
                return result.returncode == 0 and result.stdout.strip().isdigit()

            wait_for("X11 window manager", manager_ready)
            command = [args.viewer.resolve(), "--smoke-test", "--video", destination]
            with (destination / "viewer.log").open("w") as log, subprocess.Popen(
                command, env=viewer_env, cwd=root, stdout=log, stderr=log,
            ) as app:
                try:
                    view = Viewer(app, args.xdotool, viewer_env, destination, 100)
                    quit_clip = exercise(view, node, data, cookie)
                    close_window(viewer_env["DISPLAY"], view.window)
                    assert app.wait(8) == 0
                    log = (destination / "viewer.log").read_text()
                    assert "Video capture: Window closed" in log
                    assert "Scene error:" not in log
                    clip(quit_clip, view.width, view.height)
                    print("Fullscreen, minimize and native close each finalized a seekable take; restarts passed",
                          flush=True)
                finally:
                    if app.poll() is None:
                        app.kill()
                        app.wait(5)


if __name__ == "__main__":
    main()
