# Versions and binary releases

The `Bitcoin Viz release` workflow runs on pushes to `main` and manual dispatch.
The pinned `ietf-tools/semver-action` computes the next version: feature commits
advance the minor version, breaking commits advance the major version, and other
new commits fall back to a patch. Existing version tags remain the baseline.

## Build matrix

| Archive target | Runner | Toolchain |
|---|---|---|
| Linux x86_64 | Ubuntu 24.04 | Dyne musl v2.5.1, static libraries |
| Linux arm64 | Ubuntu 24.04 | Dyne musl v2.5.1, QEMU contract execution |
| macOS x86_64 | macOS 15 Intel | Apple Clang, static dependency libraries, macOS 11 minimum |
| macOS arm64 | macOS 15 Apple Silicon | Apple Clang, static dependency libraries, macOS 11 minimum |
| Windows x86_64 | Windows Server 2022 | Visual Studio 2022, static libraries and MSVC runtime |

vcpkg and its baseline are pinned to the same commit. Triplets under `ci/triplets`
select release-only static dependencies. Linux's chainload consumes the musl
action's `CC`/`CXX` outputs without replacing vcpkg host compilers. Packages are
cached per platform, architecture, toolchain and source pin. Linux binaries must
have no ELF interpreter. Desktop builds run the native contracts, CLI checks and
release-tool fixtures. Ubuntu also runs them under ASan/UBSan.

## Exact source and complete publication

`ci/release/prepare.cmake` requires a clean tracked checkout at the expected base.
It updates the sole version source, `VERSION`, commits it if needed, and bundles
the exact candidate. All jobs check out that commit. CMake generates the version
header from `VERSION`; `BITCOIN_VIZ_VERSION` can explicitly override it.

`ci/release/package.cmake` packages the installed executable and licenses, gathers
dependency copyrights, and adds `MANIFEST.json`, `VERSION`, `README.md` and
`SOURCE.txt`. Source links point to the exact commit in
`luganoplanb/bitcoin-viz`. Archives are named
`bitcoin-viz-vVERSION-PLATFORM-ARCH.tar.gz` (`.zip` on Windows), containing
`bin/bitcoin-viz` or `bin/bitcoin-viz.exe` and `share/bitcoin-viz/` licenses.

`ci/release/collect.cmake` requires exactly five archives. It verifies their
manifests against the expected commit, version, platform, architecture and binary
name, then writes `SHA256SUMS`. Only after all builds and checks pass does the
publish job tag the tested candidate and publish the complete release. Existing
published assets are preserved; a draft can be resumed. If `main` still points to
the starting commit, it is fast-forwarded to the tested version commit with
`[skip ci]`. Only publication has `contents: write`; releases are serialized.

These scripts use CMake and Git, with no interpreter dependency. For a local
installed build:

```sh
cmake --install build/release --prefix "$PWD/.release/stage"
cmake -DVERSION=0.3.0 -DPLATFORM=linux -DARCH=x86_64 \
  -DSTAGE=.release/stage -DOUTPUT=.release/assets -P ci/release/package.cmake
```

Use `-DDEPENDENCIES=...` for vcpkg releases; it requires curl, OpenSSL, SQLite and
ZeroMQ license files. `prepare.cmake` is intended for disposable CI checkouts.
Local release tests use temporary repositories and fixture binaries and publish
nothing. Checksums establish download integrity; packages are unsigned and are
not Apple-notarized. Bitcoin Core and Gource are installed separately.
