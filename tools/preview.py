#!/usr/bin/env python3
"""Render current scenes through Gource stdin, using a private Xvfb display."""

import argparse
import os
import shutil
import subprocess
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("docs/previews"))
    parser.add_argument("--max-visible", type=int, default=200)
    parser.add_argument("--native-binary", type=Path, help="render through the standalone C++ executable")
    args = parser.parse_args()
    for executable in ("Xvfb", "gource", "ffmpeg"):
        if not shutil.which(executable): parser.error(f"Required executable: {executable}")
    args.output.mkdir(parents=True, exist_ok=True)
    read_fd, write_fd = os.pipe()
    display = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "960x640x24", "-ac", "-nolisten", "tcp"],
                               pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    os.close(write_fd)
    try:
        with os.fdopen(read_fd) as stream: number = stream.readline().strip()
        if not number: raise RuntimeError("Private Xvfb display did not start")
        environment = {**os.environ, "DISPLAY": ":" + number, "LIBGL_ALWAYS_SOFTWARE": "1"}
        for mode in ("state", "protocol", "utxo"):
            log = args.output / f"{mode}.gource"
            video = args.output / f"{mode}.mp4"
            png = args.output / f"{mode}.png"
            with log.open("w") as target:
                if args.native_binary:
                    subprocess.run([str(args.native_binary.resolve()), "render", "--db", str(args.db), "--mode", mode,
                                    "--max-visible", str(args.max_visible), "--once"], stdout=target, check=True)
                else:
                    from bitcoin_wizard_cave.render import Renderer
                    renderer = Renderer(args.db, mode, target, args.max_visible)
                    renderer.snapshot()
                    renderer.db.close()
            with log.open("rb") as source, (args.output / f"{mode}.stderr").open("wb") as diagnostics:
                gource = subprocess.Popen(["gource", "-960x640", "--log-format", "custom", "--realtime",
                    "--disable-auto-skip", "--file-idle-time", "0", "--max-file-lag", "0.1", "--stop-at-time", "5",
                    "--title", f"Bitcoin Wizard Cave · Signet · {mode}" + (" · native" if args.native_binary else "") + (" · observed output subset" if mode == "utxo" else ""),
                    "--hide", "usernames,filenames,mouse,progress", "--output-ppm-stream", "-", "--output-framerate", "25", "-"],
                    stdin=source, stdout=subprocess.PIPE, stderr=diagnostics, env=environment)
                try:
                    ffmpeg = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "image2pipe", "-vcodec", "ppm",
                        "-r", "25", "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(video)],
                        stdin=gource.stdout, stderr=diagnostics, timeout=60)
                    gource.stdout.close()
                    code = gource.wait(timeout=10)
                    if code != 0 or ffmpeg.returncode != 0: raise RuntimeError(f"{mode}: renderer failed; see .stderr file")
                finally:
                    if gource.poll() is None:
                        gource.terminate()
                        gource.wait(timeout=10)
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-sseof", "-0.1", "-i", str(video), "-frames:v", "1", str(png)], check=True)
            print(f"{mode}: Gource stdin accepted; video and image saved to {args.output}")
    finally:
        display.terminate()
        display.wait(timeout=10)


if __name__ == "__main__": main()
