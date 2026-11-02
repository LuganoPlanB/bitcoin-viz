# Versions and native binary releases

The `Native release` GitHub Actions workflow runs on pushes to `main` and can
also be started with **Run workflow**. It uses the pinned
[`ietf-tools/semver-action`](https://github.com/ietf-tools/semver-action) action
(`ietf-tools/semver` is shorthand; the published action repository has the
`-action` suffix).

`feat`/`feature` commits advance the minor version; breaking changes advance the
major version. Fix-oriented commits advance the patch version, with other new
commits falling back to a patch. Rerunning an already tagged commit reuses its
version and does not overwrite an already published release's assets.
`v0.2.0` anchors the original dual implementation as the versioning baseline.

## Build matrix

| Archive target | Runner | Toolchain and dependencies |
|---|---|---|
| Linux x86_64 | Ubuntu 24.04 x86_64 | `dyne/musl-action`, Dyne musl v2.5.1, static libraries |
| Linux arm64 | Ubuntu 24.04 x86_64 | `dyne/musl-action` with `target_arch: arm_64`, QEMU execution, static libraries |
| macOS x86_64 | macOS 15 Intel | Apple Clang, static third-party libraries, deployment target macOS 11 |
| macOS arm64 | macOS 15 Apple Silicon | Apple Clang, static third-party libraries, deployment target macOS 11 |
| Windows x86_64 | Windows Server 2022 | Visual Studio 2022, static third-party libraries and static MSVC runtime |

The musl action and toolchain release are pinned. vcpkg and its registry baseline
are pinned to the same commit in the workflow and `vcpkg.json`. Custom triplets
under `ci/triplets` select static/release-only dependency builds. Linux's chainload
toolchain consumes the musl action's `CC`/`CXX` outputs without replacing the
compiler used for vcpkg's host tools. Dependency packages are cached per platform,
architecture, toolchain and source pin.

Linux packages must have no ELF interpreter segment; the arm64 native contracts
execute through `qemu-aarch64`. Desktop builds run native contracts, Python
contracts, cross-language parity, observer integration and release-tool tests.
An additional Ubuntu quality job runs the complete suite in release and
ASan/UBSan configurations. The Windows integration test uses loopback TCP rather
than Unix-domain sockets and exercises blocked-output shutdown.

## Exact versioned source

Before compiling, the version job prepares a local release commit that updates
`VERSION`, `pyproject.toml` and Python's `__version__` together. That exact commit
is distributed to the build jobs in a Git bundle. Every build checks out the
same commit, and the C++ executable gets its version from CMake's generated
header. Both implementations therefore identify the same release version.

Only after **all** builds and quality gates succeed does the publish job push
the version tag and create a draft release. It verifies that all five platform
archives exist and that every manifest has the expected version, architecture,
platform and commit. It uploads the archives and `SHA256SUMS`, then publishes
the draft as the latest release. A failed matrix job cannot publish a partial
release. A rerun can resume a draft, while existing published assets are kept.

If `main` has not moved during the build, the tested version commit is also
fast-forwarded onto `main`. That push uses the workflow's `GITHUB_TOKEN` and a
`[skip ci]` release message, so it does not start a release loop. If newer work
has arrived, the tested commit is tagged without overwriting that work.

Read-only jobs calculate/build/test. Only the final publish job receives
`contents: write`. It creates the tag from the exact tested commit, rather than
rebuilding from a moving branch. Workflow concurrency serializes releases.

## Package contents and use

Each `.tar.gz` (Linux/macOS) or `.zip` (Windows) contains:

- `bin/bitcoin-wizard-cave-native` or `bin/bitcoin-wizard-cave-native.exe`;
- `VERSION` and `MANIFEST.json`, including the source commit and dependency baseline;
- GPL, MIT and dependency license files under `share/bitcoin-wizard-cave`;
- `README.md` and `SOURCE.txt` linking corresponding source and build pins.

Use `SHA256SUMS` to verify archive downloads. Extract the archive and run the
binary from `bin/`; Gource and Bitcoin Core remain separately installed tools.
The native executable does not require Python. These builds are not signed or
Apple-notarized; a checksum establishes artifact integrity, not publisher identity.

For Bitcoin Core on Windows, use its local RPC cookie and a loopback sequence
publisher, for example:

```ini
server=1
signet=1
[signet]
zmqpubsequence=tcp://127.0.0.1:28332
```

```powershell
bin\bitcoin-wizard-cave-native.exe live --datadir C:\bitcoin-signet --mode state |
  gource --log-format custom --realtime --disable-auto-skip --file-idle-time 0 -
```

For Linux/macOS, the documented `unix:/...` / `ipc:///...` configuration remains
available. Native defaults use Core's per-platform data directory; explicit
`--datadir` and `--cookie` work on every platform. State remains a node-local
observed window, and the binaries do not imply complete signet synchronization.

## Local validation

```sh
python3 -m unittest discover -s tools/tests -v
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

`tools/release.py` is the CI implementation for source preparation, packaging and
complete-asset verification. Its `prepare` command creates a local version commit
and is intended for a clean disposable CI checkout. The release-tool tests use
temporary repositories and fixture executables; they do not publish anything.
