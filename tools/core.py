#!/usr/bin/env python3
"""Download a pinned Core binary, or run the project's isolated signet node."""

import argparse
import hashlib
import os
import platform
import subprocess
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = "32.1"
ARCHIVE = f"bitcoin-{VERSION}-x86_64-linux-gnu.tar.gz"
# Pin independently of the downloaded checksum file. See docs/VERIFICATION.md.
SHA256 = "61545c17c3120fb7412f896d248b11993af04ec8817aad3649c55b2afb044963"
BASE = f"https://bitcoincore.org/bin/bitcoin-core-{VERSION}/"


def download(url, destination):
    temporary = destination.with_suffix(destination.suffix + ".part")
    try:
        with urllib.request.urlopen(url, timeout=60) as source, temporary.open("wb") as target:
            while chunk := source.read(1024 * 1024): target.write(chunk)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("fetch", "start", "stop", "info"))
    parser.add_argument("--capture", action="store_true", help="enable optional Core P2P message files")
    args = parser.parse_args()
    os.umask(0o077)
    tools = ROOT / ".tools"
    tools.mkdir(exist_ok=True)
    binary = tools / f"bitcoin-{VERSION}" / "bin"
    data = ROOT / ".runtime" / "signet"
    sockets = ROOT / ".runtime" / "socket"
    if args.command == "fetch":
        if platform.system() != "Linux" or platform.machine() != "x86_64":
            parser.error("This pinned helper supports Linux x86_64; use official Core binaries for your platform")
        archive = tools / ARCHIVE
        if not archive.exists(): download(BASE + ARCHIVE, archive)
        with archive.open("rb") as source: actual = hashlib.file_digest(source, "sha256").hexdigest()
        if actual != SHA256: raise SystemExit("Core archive failed pinned SHA256 verification")
        download(BASE + "SHA256SUMS", tools / "SHA256SUMS")
        expected = next((line.split()[0] for line in (tools / "SHA256SUMS").read_text().splitlines()
                         if line.split()[-1] == ARCHIVE), None)
        if expected != SHA256: raise SystemExit("Official checksum and project pin disagree")
        with tarfile.open(archive) as package:
            if binary.parent.exists():
                # Repeated fetch is safe even while bitcoind is running. Linux
                # correctly refuses truncating a mapped executable (ETXTBSY).
                for name in ("bitcoind", "bitcoin-cli"):
                    installed = binary / name
                    with package.extractfile(f"bitcoin-{VERSION}/bin/{name}") as source:
                        expected_binary = hashlib.file_digest(source, "sha256").hexdigest()
                    if not installed.is_file(): raise SystemExit("Incomplete Core install; move its .tools directory aside and fetch again")
                    with installed.open("rb") as source:
                        if hashlib.file_digest(source, "sha256").hexdigest() != expected_binary:
                            raise SystemExit("Installed Core binary differs from pinned archive; move its directory aside and fetch again")
            else:
                if not hasattr(tarfile, "data_filter"):
                    raise SystemExit("Safe extraction needs a patched Python with tarfile.data_filter")
                package.extractall(tools, filter="data")
        print(f"Bitcoin Core {VERSION}; SHA256 verified. Release signatures were not verified by this helper.")
        return
    if not (binary / "bitcoind").exists(): parser.error("Run python tools/core.py fetch first")
    if args.command == "start":
        data.mkdir(parents=True, exist_ok=True, mode=0o700)
        sockets.mkdir(parents=True, exist_ok=True, mode=0o700)
        command = [str(binary / "bitcoind"), f"-datadir={data}", "-signet", "-server=1", "-listen=0",
                   "-prune=550", "-dbcache=64", "-maxconnections=8", "-par=2",
                   f"-zmqpubsequence=unix:{sockets / 'sequence.sock'}"]
        if args.capture: command.append("-capturemessages")
        os.execv(command[0], command)  # Foreground process: Ctrl-C stops it cleanly.
    subprocess.run([str(binary / "bitcoin-cli"), f"-datadir={data}", "-signet",
                    "stop" if args.command == "stop" else "getblockchaininfo"], check=True)


if __name__ == "__main__": main()
