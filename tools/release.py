#!/usr/bin/env python3
"""Prepare an exact release source commit and package/verify native artifacts."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {("linux", "x86_64"), ("linux", "arm64"), ("macos", "x86_64"), ("macos", "arm64"), ("windows", "x86_64")}


def version(value):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value):
        raise ValueError("Release version must be strict major.minor.patch")
    return value


def git(root, *args):
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()


def prepare(root, release_version, base, bundle):
    version(release_version)
    if git(root, "rev-parse", "HEAD") != base:
        raise ValueError("Release candidate must start at the requested commit")
    if git(root, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Release candidate requires a clean tracked checkout")
    package = root / "pyproject.toml"
    init = root / "src/bitcoin_wizard_cave/__init__.py"
    substitutions = [(package, r'(?m)^version = "[^"]+"$', f'version = "{release_version}"'),
                     (init, r'(?m)^__version__ = "[^"]+"$', f'__version__ = "{release_version}"')]
    replacements = []
    for path, pattern, replacement in substitutions:
        updated, count = re.subn(pattern, replacement, path.read_text())
        if count != 1: raise ValueError(f"Ambiguous version field in {path}")
        replacements.append((path, updated))
    for path, updated in replacements: path.write_text(updated)
    (root / "VERSION").write_text(release_version + "\n")
    git(root, "add", "VERSION", "pyproject.toml", "src/bitcoin_wizard_cave/__init__.py")
    if subprocess.run(["git", "diff", "--cached", "--quiet"], cwd=root).returncode:
        git(root, "-c", "user.name=github-actions[bot]", "-c", "user.email=41898282+github-actions[bot]@users.noreply.github.com",
            "commit", "-m", f"chore(release): v{release_version} [skip ci]")
    candidate = git(root, "rev-parse", "HEAD")
    bundle = bundle.resolve()
    bundle.parent.mkdir(parents=True, exist_ok=True)
    temporary = bundle.with_suffix(".tmp")
    git(root, "bundle", "create", str(temporary), "HEAD")
    temporary.replace(bundle)
    return candidate


def archive_name(release_version, platform, arch):
    version(release_version)
    if (platform, arch) not in TARGETS: raise ValueError("Unknown release target")
    suffix = ".zip" if platform == "windows" else ".tar.gz"
    return f"bitcoin-wizard-cave-v{release_version}-{platform}-{arch}{suffix}"


def package(root, stage, output, release_version, platform, arch, dependencies=None):
    name = archive_name(release_version, platform, arch)
    executable = "bitcoin-wizard-cave-native" + (".exe" if platform == "windows" else "")
    if not (stage / "bin" / executable).is_file(): raise ValueError("Installed executable missing")
    if not (stage / "share/bitcoin-wizard-cave/LICENSE").is_file(): raise ValueError("Installed GPL license missing")
    if not (stage / "share/bitcoin-wizard-cave/nlohmann-LICENSE.MIT").is_file(): raise ValueError("Vendored license missing")
    if dependencies is not None:
        if not dependencies.is_dir(): raise ValueError("Dependency installation missing")
        licenses = stage / "share/bitcoin-wizard-cave/third-party"
        licenses.mkdir(parents=True, exist_ok=True)
        for copyright_file in sorted(dependencies.glob("share/*/copyright")):
            shutil.copyfile(copyright_file, licenses / (copyright_file.parent.name + ".txt"))
        for required in ("curl", "openssl", "sqlite3", "zeromq"):
            if not (licenses / (required + ".txt")).is_file(): raise ValueError(f"Dependency license missing: {required}")
    manifest = {"version": release_version, "tag": "v" + release_version, "commit": git(root, "rev-parse", "HEAD"),
                "platform": platform, "architecture": arch, "executable": "bin/" + executable,
                "dependency_baseline": json.loads((root / "vcpkg.json").read_text())["builtin-baseline"]}
    (stage / "MANIFEST.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (stage / "VERSION").write_text(release_version + "\n")
    (stage / "SOURCE.txt").write_text(f"Corresponding source: https://github.com/jaromil/bitcoin-wizard-cave/tree/v{release_version}\n"
        "Build instructions and exact dependency source pins: docs/RELEASING.md, vcpkg.json, and ci/.\n")
    shutil.copyfile(root / "README.md", stage / "README.md")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / name
    directory = name.removesuffix(".zip").removesuffix(".tar.gz")
    if platform == "windows":
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as target:
            for path in sorted(stage.rglob("*")):
                if path.is_file(): target.write(path, directory + "/" + path.relative_to(stage).as_posix())
    else:
        with tarfile.open(archive, "w:gz") as target: target.add(stage, arcname=directory)
    return archive


def collect(directory, release_version, commit):
    expected = {archive_name(release_version, platform, arch): (platform, arch) for platform, arch in TARGETS}
    found = {p.name for p in directory.iterdir() if p.name.endswith((".zip", ".tar.gz"))}
    if found != set(expected): raise ValueError(f"Release asset set differs: missing={set(expected) - found}, extra={found - set(expected)}")
    checksums = []
    for name, (platform, arch) in sorted(expected.items()):
        path = directory / name
        prefix = name.removesuffix(".zip").removesuffix(".tar.gz")
        member = prefix + "/MANIFEST.json"
        if name.endswith(".zip"):
            with zipfile.ZipFile(path) as archive:
                if archive.getinfo(member).file_size > 65536: raise ValueError("Manifest too large")
                manifest = json.loads(archive.read(member))
                assert_binary = lambda file: file in archive.namelist()
                present = assert_binary(prefix + "/" + manifest["executable"])
        else:
            with tarfile.open(path) as archive:
                if archive.getmember(member).size > 65536: raise ValueError("Manifest too large")
                manifest = json.load(archive.extractfile(member))
                present = prefix + "/" + manifest["executable"] in archive.getnames()
        if not present or manifest["version"] != release_version or manifest["commit"] != commit or (manifest["platform"], manifest["architecture"]) != (platform, arch):
            raise ValueError(f"Artifact manifest mismatch: {name}")
        with path.open("rb") as source: digest = hashlib.file_digest(source, "sha256").hexdigest()
        checksums.append(f"{digest}  {name}")
    sums = directory / "SHA256SUMS"
    sums.write_text("\n".join(checksums) + "\n")
    return sums


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("prepare")
    p.add_argument("--version", required=True)
    p.add_argument("--base", required=True)
    p.add_argument("--bundle", type=Path, required=True)
    p.add_argument("--github-output", type=Path)
    p = commands.add_parser("package")
    p.add_argument("--version", required=True)
    p.add_argument("--stage", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--platform", required=True)
    p.add_argument("--arch", required=True)
    p.add_argument("--dependencies", type=Path)
    p = commands.add_parser("collect")
    p.add_argument("--version", required=True)
    p.add_argument("--commit", required=True)
    p.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "prepare":
        candidate = prepare(ROOT, args.version, args.base, args.bundle)
        if args.github_output:
            with args.github_output.open("a") as target: target.write(f"commit={candidate}\n")
        print(candidate)
    elif args.command == "package":
        print(package(ROOT, args.stage, args.output, args.version, args.platform, args.arch, args.dependencies))
    else: print(collect(args.directory, args.version, args.commit))


if __name__ == "__main__": main()
