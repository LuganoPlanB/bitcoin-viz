# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke-test both executables extracted from a release archive."""

import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time


WINDOWS_MESA_SHA256 = "ca56d92bb2254a9a25557c95cfe27f02071a77b26daff4cbf9bc125ed3347fb5"


def run(args, cwd, env, log, input_text=None):
    try:
        result = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                                input=input_text, timeout=60)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b"") + b"\nTimed out after 60 seconds\n")
        raise
    log.write_text(result.stdout, encoding="utf-8")
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    return result.stdout


def install_windows_software_gl(archive, bindir, work, evidence):
    archive = archive.resolve(strict=True)
    digest = sha256(archive.read_bytes()).hexdigest()
    if digest != WINDOWS_MESA_SHA256:
        sys.exit(f"Windows Mesa fixture checksum mismatch: {digest}")
    fixture = work / "mesa-fixture"
    fixture.mkdir()
    members = ["x64/opengl32.dll", "x64/libgallium_wgl.dll"]
    run(["cmake", "-E", "tar", "xf", archive, "--", *members], fixture,
        os.environ.copy(), evidence / "windows-mesa-extract.log")
    for member in members:
        source = fixture / member
        shutil.copyfile(source, bindir / source.name)
    (evidence / "windows-mesa-fixture.txt").write_text(
        f"mesa-dist-win 25.3.6 release-msvc x64 SHA256 {digest}\n", encoding="utf-8",
    )


def run_regtest_smoke(smoke, core_bin, work, env, evidence):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
    from desktop_regtest import Core

    core_bin = core_bin.resolve(strict=True)
    suffix = ".exe" if sys.platform == "win32" else ""
    with Core(core_bin / ("bitcoind" + suffix), core_bin / ("bitcoin-cli" + suffix),
              work / "desktop smoke node", env) as core:
        core.start(sequence=True)
        core.rpc("createwallet", "desktop-smoke")
        address = core.rpc("getnewaddress", wallet="desktop-smoke")
        core.rpc("generatetoaddress", 2, address)
        request = json.dumps({"datadir": str(core.data), "cookie": str(core.cookie), "rpc_url": core.rpc_url})
        return run([*smoke, "--regtest"], work, env, evidence / "viewer-regtest.log", input_text=request)


def check_smoke(result, projection, windows_software):
    for token in ("OpenGL: ", "Overlay: ", "Scene: Gource / state", f"Scene: Logstalgia / {projection}",
                  "Desktop smoke passed:"):
        if token not in result:
            sys.exit(f"Missing viewer smoke marker {token!r}")
    if windows_software:
        renderer = next((line for line in result.splitlines() if line.startswith("OpenGL: ")), "")
        if not any(driver in renderer.lower() for driver in ("llvmpipe", "softpipe")):
            sys.exit(f"Expected a Mesa software renderer, observed {renderer!r}")


def check_windows_recording(viewer, work, env, evidence):
    collector = viewer.with_name("bitcoin-viz.exe")
    seed = work / "recording.sqlite"
    # The CLI creates the fixture using a portable relative filename.
    run([collector, "demo", "--db", seed.name], work, env, evidence / "recording-fixture.log")
    recording = seed.with_name("recording 録画 📈.sqlite")
    seed.rename(recording)
    log = evidence / "viewer-recording.log"
    with log.open("wb") as output, subprocess.Popen(
        [viewer, "--db", recording], cwd=work, env=env, stdout=output, stderr=subprocess.STDOUT,
    ) as viewer_process:
        try:
            deadline = time.monotonic() + 15
            while viewer_process.poll() is None and time.monotonic() < deadline:
                if b"Scene: Gource / state" in log.read_bytes():
                    print("Desktop opened Unicode recording through --db", flush=True)
                    return
                time.sleep(0.05)
            raise RuntimeError(f"Viewer did not open Unicode recording: {log.read_text('utf-8', errors='replace')}")
        finally:
            if viewer_process.poll() is None:
                viewer_process.kill()
            viewer_process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--version", required=True, help="expected desktop version")
    parser.add_argument("--regtest-core-bin", type=Path, metavar="DIRECTORY",
                        help="also render the installed viewer against an isolated Bitcoin Core regtest")
    software = parser.add_mutually_exclusive_group()
    software.add_argument("--software-x11", action="store_true", help="run under Xvfb with Mesa software rendering")
    software.add_argument("--windows-software-gl", type=Path, metavar="ARCHIVE",
                          help="use the pinned Mesa DLLs in the extracted Windows smoke directory")
    args = parser.parse_args()
    if args.software_x11 and sys.platform != "linux":
        parser.error("--software-x11 requires Linux")
    if args.windows_software_gl and sys.platform != "win32":
        parser.error("--windows-software-gl requires Windows")

    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    archive = args.archive.resolve(strict=True)
    windows = sys.platform == "win32"
    macos = sys.platform == "darwin"
    suffix = ".zip" if windows else ".tar.gz"
    if not archive.name.endswith(suffix):
        parser.error(f"expected a {suffix} release archive")
    prefix = archive.name.removesuffix(suffix)
    identity = f"{sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n"
    (evidence / "SHA256SUMS").write_text(identity, encoding="utf-8")
    (evidence / "platform.txt").write_text(platform.platform() + "\n", encoding="utf-8")
    print(platform.platform(), identity, sep="\n", end="", flush=True)

    with tempfile.TemporaryDirectory(prefix="bitcoin-viz-install-") as scratch:
        work = Path(scratch)
        destination = work / "Relocated viewer – café"
        shutil.unpack_archive(archive, destination)
        relocated = destination / prefix
        bindir = relocated / "bin"
        if macos:
            contents = bindir / "bitcoin-viz-desktop.app" / "Contents"
            viewer = contents / "MacOS" / "bitcoin-viz-desktop"
            assets = contents / "Resources" / "viz-data"
            notices = contents / "Resources" / "licenses"
        else:
            viewer = bindir / ("bitcoin-viz-desktop.exe" if windows else "bitcoin-viz-desktop")
            assets = bindir / "viz-data"
            notices = relocated / "share" / "bitcoin-viz"

        for relative in ("LICENSE", "third-party.md", "DESKTOP-NOTICES.md", "Gource/COPYING",
                         "Logstalgia/COPYING", "imgui/LICENSE.txt", "imkit/LICENSE", "nlohmann-LICENSE.MIT"):
            if not (notices / relative).is_file():
                sys.exit(f"Missing installed notice {relative}")
        for renderer in ("Gource", "Logstalgia"):
            if not (assets / renderer / "fonts" / "README").is_file():
                sys.exit(f"Missing installed font notice for {renderer}")
        for filename in ("Inter-Regular.ttf", "Inter-Bold.ttf", "Inter-OFL.txt"):
            if not (assets / "Logstalgia" / "fonts" / filename).is_file():
                sys.exit(f"Missing Logstalgia font asset {filename}")

        env = os.environ.copy()
        if windows:
            env["APPDATA"] = str(work / "roaming")
            env["LOCALAPPDATA"] = str(work / "local")
            env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
        else:
            for variable in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
                env.pop(variable, None)
            env["HOME"] = str(work / "home")
            env["XDG_CONFIG_HOME"] = str(work / "config")
            env["XDG_DATA_HOME"] = str(work / "data")
        if macos:
            links = run(["otool", "-L", viewer], work, env, evidence / "linkage.log")
            for line in links.splitlines()[1:]:
                if not line.strip().startswith(("/usr/lib/", "/System/Library/")):
                    sys.exit(f"Unexpected macOS dependency {line.strip()}")
        elif not windows:
            run(["ldd", viewer], work, env, evidence / "linkage.log")

        collector = bindir / ("bitcoin-viz.exe" if windows else "bitcoin-viz")
        cli_version = run([collector, "--version"], work, env, evidence / "cli-version.log").strip()
        if cli_version != f"{args.version} (C++17)":
            sys.exit(f"Expected collector {args.version} (C++17), observed {cli_version}")
        version = run([viewer, "--version"], work, env, evidence / "version.log").strip()
        if version != args.version:
            sys.exit(f"Expected viewer {args.version}, observed {version}")

        smoke = [viewer, "--smoke-test"]
        if args.software_x11:
            env.update(SDL_VIDEODRIVER="x11", LIBGL_ALWAYS_SOFTWARE="1")
            smoke = ["xvfb-run", "-a", *smoke]
        if args.windows_software_gl:
            install_windows_software_gl(args.windows_software_gl, bindir, work, evidence)
            env["GALLIUM_DRIVER"] = "llvmpipe"
        result = run(smoke, work, env, evidence / "viewer.log")
        check_smoke(result, "traffic", args.windows_software_gl)
        if windows:
            check_windows_recording(viewer, work, env, evidence)
        if args.regtest_core_bin:
            result = run_regtest_smoke(smoke, args.regtest_core_bin, work, env, evidence)
            check_smoke(result, "blocks", args.windows_software_gl)
            if "Desktop smoke node: regtest / Connected / RPC + ZMQ sequence" not in result:
                sys.exit("Missing connected regtest sequence source in the installed smoke")


if __name__ == "__main__":
    main()
