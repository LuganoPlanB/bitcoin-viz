# Vendored source

`nlohmann/json.hpp` is the unmodified single header from nlohmann JSON **3.12.0**:
https://github.com/nlohmann/json/blob/v3.12.0/single_include/nlohmann/json.hpp

SHA256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`

Its MIT license is preserved in `nlohmann/LICENSE.MIT` and in the header.
Builds use this local copy and do not download dependencies.

## Desktop viewer snapshots

| Source | Vendored revision | License notices |
| --- | --- | --- |
| [Gource](https://github.com/acaudwell/Gource) | `70db547c6b8b40745fb41a8db7e8d28897be1a9e` | GPL-3.0-or-later, `Gource/COPYING` |
| [Logstalgia](https://github.com/acaudwell/Logstalgia) | `823a1a4dbdba8f682e2d31851c11e369e50aa0f7` | GPL-3.0-or-later, `Logstalgia/COPYING` |
| [Core](https://github.com/acaudwell/Core) for Gource | `d8d880fd58fda37b9d8fb59844a3c2a712969948` | BSD-style and other notices in `Gource/src/core/` |
| [Core](https://github.com/acaudwell/Core) for Logstalgia | `f7fa400ec164f6fb36bcca5b85d2d2685cd3c7e8` | BSD-style and other notices in `Logstalgia/src/core/` |
| [Dear ImGui](https://github.com/ocornut/imgui) | `367b2c24f399988ddafc0bb4628da0106bcc09be` (1.93.0 WIP, docking) | MIT, `imgui/LICENSE.txt` |
| [imgui-modern-kit](https://github.com/AokiMotohide/imgui-modern-kit) | `986d8d286a74fb7b2f724b9ef5d008af139aa887` (4.1.0) | MIT, `imkit/LICENSE` and `imkit/THIRD_PARTY_NOTICES.md` |

Both engine snapshots include their pinned Core submodule trees as ordinary
files. The two Core revisions have identical headers. Gource's revision adds
the OpenGL compatibility profile request in `display.cpp` and conversion of
indexed textures to RGBA in `texture.cpp`. The viewer links that Core once to
share the window, OpenGL context and graphics managers between the engines.
The local changes in Gource's `src/core/{display,fxfont,shader,texture}.cpp`
release graphics resources while the context is current and clean up partially
loaded resources on failure.
The desktop build also adapts the Windows CRT declarations, native font and
shader paths, and the macOS OpenGL profile. The installed binary notices for
Core, FTGL, TinyXML and the embedded ImGui fonts are in `DESKTOP-NOTICES.md`.

The engine changes are in Gource's `src/formats/{commitlog,custom}.h`,
`src/{file,gource,user}.{h,cpp}` and Logstalgia's
`src/logstalgia.{h,cpp}`. They supply in-memory log input, respect the host's
input ownership, preserve Logstalgia's active entries on resize, and release
pending actions and log entries with their scenes. The host
omits both upstream entry points and renames Logstalgia's `PositionSlider`
at compile time to avoid a symbol collision with Gource. Gource also includes
TinyXML, whose zlib-style notice is in `Gource/src/tinyxml/tinyxml.h`.

The Dear ImGui and ImKit directories contain the source subsets used by the
viewer. ImKit 4.1.0 targets the exact vendored Dear ImGui revision. The host
builds one Dear ImGui core with the SDL2 and OpenGL2 backends.

The viewer resolves `viz-data/Gource` and `viz-data/Logstalgia` beside its
executable on Linux and Windows and under `Contents/Resources` in the macOS
application bundle. The build and install carry the engine textures, Gource
shaders and GNU FreeFont files. Each engine's `data/fonts/README`
contains the fonts' GPL-3.0-or-later notice and font embedding exception.
