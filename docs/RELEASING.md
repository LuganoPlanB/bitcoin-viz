# Versions and binary releases

The `Bitcoin Viz release` workflow runs on pushes to `main` and manual dispatch.
The pinned `ietf-tools/semver-action` computes the next version: feature commits
advance the minor version, breaking commits advance the major version, and other
new commits fall back to a patch. Existing version tags remain the baseline.

## Build matrix

| Archive target | Runner | Toolchain |
|---|---|---|
| Linux x86_64 | Ubuntu 24.04 | Static Dyne musl v2.5.1 CLI, native OpenGL desktop |
| Linux arm64 | Ubuntu 24.04 and Ubuntu 24.04 Arm | Static Dyne musl v2.5.1 CLI with QEMU contract execution, native OpenGL desktop |
| macOS x86_64 | macOS 15 Intel | Apple Clang, static dependency libraries, macOS 11 minimum |
| macOS arm64 | macOS 15 Apple Silicon | Apple Clang, static dependency libraries, macOS 11 minimum |
| Windows x86_64 | Windows Server 2022 | Visual Studio 2022, static libraries and MSVC runtime |

vcpkg and its baseline are pinned to the same commit. Triplets under `ci/triplets`
select release-only static dependencies. Linux's chainload consumes the musl
action's `CC`/`CXX` outputs without replacing vcpkg host compilers. Packages are
cached per platform, architecture, toolchain and source pin. Linux CLI binaries must
have no ELF interpreter. The macOS and Windows CLI builds run the native
contracts, CLI checks and release-tool fixtures. Ubuntu also runs them under
ASan/UBSan.

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
The desktop archives use `bitcoin-viz-desktop-vVERSION-PLATFORM-ARCH` with the
same extensions. They carry the viewer, its assets and notices, and the CLI.
Launch `bin/bitcoin-viz-desktop` on Linux, `bin/bitcoin-viz-desktop.exe` on
Windows, or `bin/bitcoin-viz-desktop.app` on macOS. Extract the whole archive
so the application can find its resources.

`ci/release/collect.cmake` requires five CLI and five desktop archives. It verifies their
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
not Apple-notarized. For the CLI, install Bitcoin Core and external visualizers
separately.

## Desktop builds

The interactive viewer is opt-in and uses a C++20 compiler, CMake 3.20 or
later, and the system OpenGL 2.1-compatible driver. Install Python 3 for the
desktop session tests. To include the real-node regtest in CTest, add
`-DBITCOIN_VIZ_TEST_CORE_BIN_DIR=/path/to/core/bin` when configuring, using a
Bitcoin Core build with ZMQ and wallet support. The test starts a private
regtest with a temporary data directory.

To connect to a local Bitcoin Core or Bitcoin Roots node, launch the viewer
without `--demo` and open the **Node** tab. Startup checks the default mainnet
cookie's file metadata and the loopback TCP port. **Test Connection** verifies
the cookie and chain over RPC. **Connect** starts collection after a successful
test with the current settings. Edit the network, port, data directory or cookie
to connect to a custom local node. Desktop RPC supports direct loopback only.

### Linux

On Debian or Ubuntu, install the CLI and graphics development packages:

```sh
sudo apt install build-essential cmake ninja-build pkg-config ccache python3 \
  libzmq3-dev libsqlite3-dev libcurl4-openssl-dev libssl-dev \
  libsdl2-dev libsdl2-image-dev libglew-dev libgl1-mesa-dev libglu1-mesa-dev \
  libglm-dev libpng-dev libfreetype-dev libboost-filesystem-dev libpcre2-dev
cmake --preset desktop
cmake --build --preset desktop --parallel 2
ctest --preset desktop
cmake --install build/desktop --prefix "$HOME/Bitcoin Viz"
"$HOME/Bitcoin Viz/bin/bitcoin-viz-desktop" --demo
```

The desktop uses the distribution's shared libraries and graphics stack. Keep
the corresponding runtime libraries installed on systems where you run it.
Move the entire prefix to keep `bin/viz-data` and `share/bitcoin-viz` with the
binary. The Linux desktop build uses the native system toolchain.

### macOS

Install the Xcode command-line tools, CMake, Ninja and Python 3. From the repo
root, bootstrap the same vcpkg revision used by the CLI releases:

```sh
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout 80aae04504a834399456d74605204bfe514762bf
.tools/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT="$PWD/.tools/vcpkg"
arch=$(uname -m)
case "$arch" in
  arm64) triplet=arm64-osx-release ;;
  x86_64) triplet=x64-osx-release ;;
esac
cmake --preset desktop-macos -DCMAKE_OSX_ARCHITECTURES="$arch" -DVCPKG_TARGET_TRIPLET="$triplet"
cmake --build --preset desktop-macos --parallel 2
ctest --preset desktop-macos
cmake --install build/desktop-macos --prefix "$HOME/Bitcoin Viz"
open "$HOME/Bitcoin Viz/bin/bitcoin-viz-desktop.app" --args --demo
```

The macOS 11+ application bundle uses Apple's legacy OpenGL profile and static
vcpkg libraries. You can move the whole `.app` or open it from Finder. The assets
and notices live in `Contents/Resources`. To read startup diagnostics from a
terminal, run `Contents/MacOS/bitcoin-viz-desktop` inside the bundle. The source
build recipe does not include Developer ID signing or notarization.

### Windows

Install Visual Studio 2022 with Desktop development with C++, the Windows SDK,
CMake and Python 3. Run these commands in PowerShell at the repo root:

```powershell
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout 80aae04504a834399456d74605204bfe514762bf
& .\.tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = (Resolve-Path .tools/vcpkg).Path
cmake --preset desktop-windows
cmake --build --preset desktop-windows --parallel 2
ctest --preset desktop-windows
$stage = Join-Path $env:USERPROFILE 'Bitcoin Viz'
cmake --install build/desktop-windows --config Release --prefix $stage
& (Join-Path $stage 'bin/bitcoin-viz-desktop.exe') --demo
```

The x64 preset links the vcpkg libraries and MSVC runtime statically. Run the
viewer in a graphical Windows session with the graphics vendor's OpenGL
driver. Keep `bin/viz-data` and `share/bitcoin-viz` with the binary when moving
the install prefix. Core can publish ZMQ sequence over a loopback TCP address.

### Installed package checks

The [desktop checks](../.github/workflows/desktop.yml) build, test and package on
native runners. The release workflow calls the same checks on the versioned
candidate and publishes the packages that passed. The checks launch the extracted viewer from
an unrelated directory at a path containing spaces and Unicode characters.
Each run records the package checksum, OS, OpenGL renderer and version, logical
and drawable size, and overlay scale. The Linux CI check uses Xvfb and Mesa
software rendering. Native X11 input also exercises startup prefill, explicit
verification and connection against an owned loopback fixture. Windows CI uses
the checksum-pinned Mesa for Windows 25.3.6 MSVC build for software rendering on
its hosted runner. The smoke harness places the two Mesa OpenGL DLLs beside the
extracted viewer in its temporary directory. The archive contains the original
staged application. A hardware check uses the system driver in a graphical
session on a GPU-equipped machine. [Windows Mesa release](https://github.com/pal1000/mesa-dist-win/releases/tag/25.3.6),
[Mesa app-local setup](https://docs.mesa3d.org/drivers/llvmpipe.html#windows).

To repeat the installed smoke against a local prefix:

```sh
python3 ci/desktop/installed.py "$HOME/Bitcoin Viz" ./build/desktop-evidence \
  --version "$(cat VERSION)"
```

Use `python` on Windows. On Linux with `xvfb` and `xauth` installed, append
`--software-x11` to exercise software rendering in a private X server. An
unavailable graphics driver or display causes the smoke check to fail with a
startup diagnostic. The desktop check workflow uploads its evidence as CI
artifacts. The existing versioned release workflow builds CLI archives.

Add `--regtest-core-bin /path/to/core/bin` to also start a disposable local
regtest node and check both engines against its connected RPC and ZMQ session.
The smoke harness keeps the viewer log for the live run in `viewer-regtest.log`.
