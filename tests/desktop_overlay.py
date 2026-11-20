# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive Bitcoin Viz through native X11 input on a private Xvfb display."""

import argparse
import contextlib
import ctypes as ct
import json
import math
import os
from pathlib import Path
import select
import sqlite3
import subprocess
import tempfile
import time

from desktop_session import fixture_env, node_server


def wait_for(description, condition):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(0.08)
    raise AssertionError(f"Timed out waiting for {description}")


@contextlib.contextmanager
def display(env, dpi, output):
    reader, writer = os.pipe()
    with output.open("w") as log, subprocess.Popen(
        ["Xvfb", "-displayfd", str(writer), "-screen", "0", "1600x1000x24",
         "-dpi", str(dpi), "-nolisten", "tcp", "-ac"],
        pass_fds=[writer], stdout=log, stderr=log,
    ) as server:
        os.close(writer)
        try:
            assert select.select([reader], [], [], 10)[0], "Private X server did not start"
            number = os.read(reader, 64).decode().strip()
            assert number.isdigit(), output
            yield dict(env, DISPLAY=":" + number)
        finally:
            os.close(reader)
            server.terminate()
            server.wait(5)


def close_window(display_name, window):
    class ClientMessage(ct.Structure):
        _fields_ = [
            ("type", ct.c_int), ("serial", ct.c_ulong), ("send_event", ct.c_int),
            ("display", ct.c_void_p), ("window", ct.c_ulong), ("message_type", ct.c_ulong),
            ("format", ct.c_int), ("data", ct.c_long * 5),
        ]

    class Event(ct.Union):
        _fields_ = [("message", ClientMessage), ("padding", ct.c_long * 24)]

    xlib = ct.CDLL("libX11.so.6")
    xlib.XOpenDisplay.argtypes = [ct.c_char_p]
    xlib.XOpenDisplay.restype = ct.c_void_p
    xlib.XInternAtom.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_int]
    xlib.XInternAtom.restype = ct.c_ulong
    xlib.XSendEvent.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_long, ct.POINTER(Event)]
    xlib.XCloseDisplay.argtypes = [ct.c_void_p]
    connection = xlib.XOpenDisplay(display_name.encode())
    assert connection
    try:
        event = Event()
        event.message.type = 33  # ClientMessage
        event.message.window = int(window)
        event.message.message_type = xlib.XInternAtom(connection, b"WM_PROTOCOLS", 0)
        event.message.format = 32
        event.message.data[0] = xlib.XInternAtom(connection, b"WM_DELETE_WINDOW", 0)
        assert xlib.XSendEvent(connection, int(window), 0, 0, ct.byref(event))
    finally:
        xlib.XCloseDisplay(connection)


class Viewer:
    def __init__(self, app, xdotool, env, destination, scale):
        self.app = app
        self.xdotool = xdotool
        self.env = env
        self.destination = destination
        self.scale = scale
        self.window = wait_for("Bitcoin Viz window", self.find_window)
        self.xdo("windowfocus", self.window)

    def xdo(self, *args, check=True):
        return subprocess.run(
            [self.xdotool, *map(str, args)], env=self.env, check=check,
            capture_output=True, text=True, timeout=6,
        ).stdout.strip()

    def find_window(self):
        return self.xdo("search", "--onlyvisible", "--pid", self.app.pid, "--name", "^Bitcoin Viz$", check=False)

    def log(self):
        assert self.app.poll() is None, "Viewer exited during the journey"
        return (self.destination / "viewer.log").read_text()

    def until_log(self, message, count=1):
        wait_for(message, lambda: self.log().count(message) >= count)

    def resize(self, width, height):
        self.xdo("windowsize", self.window, width, height)
        self.until_log(f"Overlay: {width}x{height} logical, {width}x{height} drawable, {self.scale}% content scale")
        wait_for("native window geometry", lambda: f"Geometry: {width}x{height}" in self.xdo("getwindowgeometry", self.window))
        self.width, self.height = width, height
        time.sleep(0.25)

    def pointer(self, x, y):
        self.xdo("mousemove", "--window", self.window, x, y)
        time.sleep(0.12)

    def click(self, x, y, scale=True):
        multiplier = self.scale / 100 if scale else 1
        self.pointer(round(x * multiplier), round(y * multiplier))
        self.xdo("mousedown", 1)
        time.sleep(0.1)
        self.xdo("mouseup", 1)
        time.sleep(0.15)

    def key(self, key):
        self.xdo("key", key)
        time.sleep(0.18)

    def hold(self, key, seconds):
        self.xdo("keydown", key)
        try:
            time.sleep(seconds)
        finally:
            self.xdo("keyup", key)
        time.sleep(0.35)

    def type(self, text):
        self.key("ctrl+a")
        self.xdo("type", "--clearmodifiers", "--delay", 24, text)
        time.sleep(0.12)

    def capture(self, name):
        path = self.destination / (name + ".png")
        subprocess.run(
            ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "x11grab", "-window_id", self.window, "-video_size", f"{self.width}x{self.height}",
             "-draw_mouse", "0", "-i", self.env["DISPLAY"], "-frames:v", "1", "-threads", "1", path],
            env=self.env, check=True, timeout=9,
        )
        data = subprocess.check_output(
            ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-i", path,
             "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-threads", "1", "-"],
            env=self.env, timeout=9,
        )
        assert len(data) == self.width * self.height * 3
        return data

    def pixels_in(self, frame, x, y, width, height):
        rows = (frame[3 * (row * self.width + x):3 * (row * self.width + x + width)] for row in range(y, y + height))
        return b"".join(rows)

    def assert_background(self, frame, rgb):
        colour = bytes.fromhex(rgb)
        for fraction in (0.35,0.46,0.74):
            patch = self.pixels_in(frame,self.width - 45,round(self.height * fraction),20,20)
            if sum(pixel == colour for pixel in (patch[i:i+3] for i in range(0,len(patch),3))) > 350:
                return
        raise AssertionError(f"Expected background #{rgb} in the rendered scene")


def changed(before, after):
    old = zip(before[::3],before[1::3],before[2::3])
    new = zip(after[::3],after[1::3],after[2::3])
    return sum(a != b for a, b in zip(old,new,strict=True))


def leaf_center(frame, width):
    # Locate the fixture's gold files. The animated bloom changes nearby
    # pixels while paused, so comparing the whole frame would count that glow.
    count = sum_x = sum_y = 0
    for pixel, (red, green, blue) in enumerate(zip(frame[::3],frame[1::3],frame[2::3])):
        if red > 220 and green > 145 and red - blue > 60:
            y, x = divmod(pixel,width)
            count += 1
            sum_x += x
            sum_y += y
    assert count > 80, "Could not identify fixture leaves"
    return sum_x/count,sum_y/count


def wait_for_fixture(node, data):
    def synchronized():
        for path in data.rglob("main-*.sqlite"):
            try:
                with contextlib.closing(sqlite3.connect(path)) as database:
                    row = database.execute("SELECT value FROM meta WHERE key='tip'").fetchone()
                    if row and json.loads(row[0])["hash"] == node.genesis:
                        return True
            except sqlite3.OperationalError:
                continue  # The new database may still be initializing its schema.
        return False

    wait_for("fixture chain synchronization", synchronized)


def exercise(view, node, data):
    view.until_log("Scene: Gource / state")

    # Change projection through the actual combo using native keyboard navigation.
    view.click(195, 160)
    view.key("Down")
    view.key("Return")
    view.until_log("Scene: Gource / protocol")
    original = view.capture("gource-before")

    view.click(175, 233)
    view.type("1.7")
    view.key("Tab")
    view.type("284960")
    view.click(72, 336)
    view.until_log("Scene appearance updated: protocol")
    gource = view.capture("gource-scene")
    view.assert_background(gource, "284960")
    changed_tree = changed(original, gource)
    assert changed_tree > 10000

    view.click(175, 233)
    view.type("2oops")
    view.click(72, 336)
    view.until_log("Scene error: time-scale has an invalid value")
    view.assert_background(view.capture("gource-invalid"), "284960")
    assert view.log().count("Scene: Gource / protocol") == 1, "Invalid input replaced the active scene"
    view.click(158, 336)

    # Pause playback so captured scene pixels isolate camera input from animation.
    view.click(view.width - 28, view.height - 28, scale=False)
    view.key("space")
    time.sleep(1)
    view.key("F1")
    hidden = view.capture("camera-overlay-hidden")
    initial_center = leaf_center(hidden,view.width)
    view.key("F1")
    view.click(175, 233)
    view.capture("camera-field-before")
    view.hold("Right", 0.55)
    view.key("F1")
    editor_kept_camera = view.capture("camera-field-after")
    blocked_center = leaf_center(editor_kept_camera,view.width)
    captured_change = math.dist(initial_center,blocked_center)
    assert captured_change <= 2, f"Arrow key moved scene while editing ({captured_change:.2f} px)"

    view.hold("Right", 0.08)
    moved = view.capture("camera-after-arrow")
    free_change = math.dist(blocked_center,leaf_center(moved,view.width))
    assert free_change > 12, f"Arrow key did not move scene with the overlay hidden ({free_change:.2f} px)"
    view.key("F1")
    assert changed(moved, view.capture("camera-overlay-reopened")) > 10000

    view.click(112, 129)
    view.until_log("Scene: Logstalgia / traffic")
    view.click(195, 160)
    view.key("Down")
    view.key("Return")
    view.until_log("Scene: Logstalgia / mempool")
    log_before = view.capture("logstalgia-before")
    view.click(175, 268)
    view.type("343055")
    view.click(72, 301)
    view.until_log("Scene appearance updated: mempool")
    logstalgia = view.capture("logstalgia-scene")
    view.assert_background(logstalgia, "343055")
    changed_flow = changed(log_before, logstalgia)
    assert changed_flow > 10000

    # A resize updates drawable geometry without reopening the selected engine.
    width, height = view.width, view.height
    view.resize(width - 30, height - 30)
    view.resize(width, height)
    view.assert_background(view.capture("logstalgia-resized"), "343055")
    assert view.log().count("Scene: Logstalgia / mempool") == 1
    view.key("F1")
    no_panel = view.capture("logstalgia-hidden")
    view.key("F1")
    assert changed(no_panel, view.capture("logstalgia-reopened")) > 10000
    assert view.find_window() == view.window

    view.click(96, 100)
    view.capture("node-before")
    view.click(175, 167)
    view.type("http://127.0.0.1:70000")
    view.click(49, 307)
    view.until_log("Scene: Logstalgia / mempool", 2)
    assert node.attempts == 0, "Invalid RPC port made a network request"
    multiplier = view.scale / 100

    def invalid_status():
        frame = view.capture("node-invalid")
        patch = view.pixels_in(frame,round(22*multiplier),round(70*multiplier),
                              round(255*multiplier),round(17*multiplier))
        return any(red > 205 and 130 < green < 207 and blue < 170
                   for red, green, blue in zip(patch[::3],patch[1::3],patch[2::3]))

    wait_for("inline RPC error", invalid_status)

    view.click(175, 167)
    # Replace only the port digits in the existing RPC URL.
    view.key("End")
    for _ in range(5):
        view.key("BackSpace")
    view.xdo("type", "--delay", 24, str(node.server_port))
    view.click(49, 307)

    wait_for_fixture(node,data)
    view.until_log("Scene: Logstalgia / mempool", 3)
    assert node.attempts > 0 and view.find_window() == view.window
    view.capture("node-connected")
    print(
        f"{width}x{height} at {view.scale}%: Gource {changed_tree} and Logstalgia {changed_flow} changed pixels. "
        f"Camera while editing/released: {captured_change:.2f}/{free_change:.2f} px. "
        f"RPC fixture synchronized at edited loopback port {node.server_port}.",
        flush=True,
    )


def source_isolation(view, node, data):
    image = data.parent / "background.ppm"
    image_bytes = b"P6\n4 4\n255\n" + b"\xff\x88\x00" * 16
    image.write_bytes(image_bytes)
    view.until_log("Scene: Gource / state")

    view.click(160,268)
    view.type("284960")
    view.click(75,357)  # More parameters
    view.click(172,386)
    view.click(100,434)  # background-image
    view.click(172,411)
    view.type(str(image))
    view.click(72,336)
    view.until_log("Scene: Gource / state",2)
    view.assert_background(view.capture("source-preview"),"284960")
    image.unlink()

    def choose_protocol():
        view.click(195,160)
        view.key("Down")
        view.key("Return")

    error = "Scene error: background-image needs an image file"
    choose_protocol()
    view.until_log(error)
    view.assert_background(view.capture("same-source-retained"),"284960")
    assert "Scene: Gource / protocol" not in view.log()

    image.write_bytes(image_bytes)
    choose_protocol()
    view.until_log("Scene: Gource / protocol")
    view.assert_background(view.capture("same-source-recovered"),"284960")
    image.unlink()

    view.click(96,100)
    view.click(175,167)
    view.type(node.url)
    view.click(49,307)
    view.until_log(error,2)
    wait_for_fixture(node,data)
    view.capture("live-source-overlay")
    view.key("F1")
    # A cleared scene contains none of the old tree, image or background tint.
    frame = view.capture("live-source-cleared")
    blank = bytes.fromhex("1a1a1a") * (view.width * view.height)
    assert frame == blank, "Previous synthetic scene remained visible after connecting to the new source"
    assert node.attempts > 0 and view.find_window() == view.window
    print("Invalid asset retained its scene on the same source; connecting to the RPC fixture cleared the old synthetic view.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("viewer", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--scale", type=int, choices=(100,150), required=True)
    parser.add_argument("--xdotool", default="xdotool")
    parser.add_argument("--source-isolation", action="store_true", help="exercise source isolation at 1200x760")
    args = parser.parse_args()
    if args.source_isolation and args.scale != 100:
        parser.error("--source-isolation uses the 100% overlay layout")
    args.destination.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="viz-overlay-") as temp, node_server("main", 9061) as node:
        root = Path(temp)
        env, base = fixture_env(root)
        node.set_cookie(base / ".cookie")
        data = root / "data"
        # Xvfb publishes the requested physical screen size to Xlib. Its virtual
        # XRandR output reports no physical dimensions.
        env.update(XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(root / "config"),
                   SDL_VIDEODRIVER="x11", SDL_VIDEO_X11_XRANDR="0", LIBGL_ALWAYS_SOFTWARE="1")
        env.pop("XAUTHORITY", None)
        with display(env,96 * args.scale // 100,args.destination / "xvfb.log") as viewer_env:
            with (args.destination / "viewer.log").open("w") as log, subprocess.Popen(
                [args.viewer.resolve(), "--demo"], env=viewer_env, cwd=root, stdout=log, stderr=log,
            ) as app:
                try:
                    view = Viewer(app,args.xdotool,viewer_env,args.destination,args.scale)
                    view.resize(*(1200,760) if args.scale == 100 else (800,600))
                    if args.source_isolation:
                        source_isolation(view,node,data)
                    else:
                        exercise(view,node,data)
                    stdout = view.log()
                    if not args.source_isolation:
                        assert stdout.count("Scene error:") == 1
                    assert node.cookie not in stdout and node.authorization not in stdout
                    close_window(viewer_env["DISPLAY"],view.window)
                    assert app.wait(8) == 0
                finally:
                    if app.poll() is None:
                        app.kill()
                        app.wait(5)


if __name__ == "__main__":
    main()
