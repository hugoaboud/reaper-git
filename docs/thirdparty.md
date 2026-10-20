# Third-party dependencies

Everything the extension needs lives under `third_party/` and is linked
statically, so the shipped module depends only on the system's runtime and
OpenSSL. Two of those folders — libgit2 and libssh2 — are git submodules pinned to
a release tag; the rest are plain vendored copies. Clone the project with
`git clone --recursive` (or run `git submodule update --init --recursive`) to get
them; without them the build cannot configure.

## Contents

- [Purpose](#purpose)
- [Dependencies](#dependencies)
  - [libgit2](#libgit2)
  - [libssh2](#libssh2)
  - [REAPER SDK](#reaper-sdk)
  - [WDL/SWELL](#wdlshell)
  - [nlohmann/json](#nlohmannjson)
- [How the pieces fit together](#how-the-pieces-fit-together)
- [Build configuration](#build-configuration)
- [Platform notes](#platform-notes)
- [Caveats](#caveats)
- [Changing dependencies](#changing-dependencies)

## Purpose

The extension is a single module (`.so` on Linux, `.dll` on Windows) that talks
to git repositories and draws its own windows. Two capabilities are needed from
outside: a git implementation and a way to create windows from one code base on
both platforms. Both are compiled into the module; nothing is installed at run
time.

## Dependencies

Every entry below uses the same layout: where it comes from, why it is present,
which parts are actually used, and what to watch out for.

### libgit2

- **Origin:** `third_party/libgit2`, a submodule of
  https://github.com/libgit2/libgit2 pinned at tag `v1.9.7`. Build output and
  sources are upstream, unmodified.
- **Why:** every repository operation is a libgit2 call. The extension never
  starts an external `git` process, so behaviour does not depend on what the
  user has installed.
- **Used:** opening repositories, references and revwalks, diffs, checkout,
  merge analysis, remotes, credential callbacks, stash and configuration.
- **Caveats:**
  - Built with bundled zlib and the built-in regular expression engine, so the
    module does not need `libpcre2` at run time.
  - Version 1.9 has no `git_error_clear` and no `git_error_set_str`; errors are
    read once with `git_error_last()`.
  - Its default-configuration probing can leave an unrelated error behind
    ("ProgramData", "doesn't exist"); the git layer filters those out.
  - `git_stash_save` needs an explicit signature, and `git_stash_drop` takes
    only the repository and the index.

### libssh2

- **Origin:** `third_party/libssh2`, a submodule of
  https://github.com/libssh2/libssh2 pinned at tag `libssh2-1.11.1`, built as a
  static library by the top-level `CMakeLists.txt`.
- **Why:** SSH transport for `ssh://host:port` servers and `user@host:path`
  URLs, which is how private git servers are usually reached.
- **Used:** through libgit2's SSH transport; both password and key
  authentication are exercised, with an optional key passphrase.
- **Caveats:**
  - Needs `_DEFAULT_SOURCE` on Linux: strict ANSI mode hides the BSD types
    (`u_int`, `u_char`) used by its chacha/poly1305 code.
  - Crypto backend is OpenSSL on Linux and WinCNG on Windows.

### REAPER SDK

- **Origin:** `third_party/reaper-sdk` (`reaper_plugin.h`,
  `reaper_plugin_functions.h`, `LICENSE`) from the official REAPER SDK.
- **Why:** the module entry point, plug-in registration hooks and the API
  function table come from here.
- **Used:** only the functions requested in `src/reaper_api.h`; see
  [reaper.md](reaper.md) for the list and the reasons.
- **Caveats:** the headers are C. `REAPERAPI_IMPLEMENT` is defined in exactly one
  translation unit (`src/extension.cpp`), every other file only sees the
  pointers.

### WDL/SWELL

- **Origin:** `third_party/WDL/swell`, pinned commit recorded in
  `third_party/README.md`.
- **Why:** on Linux REAPER does not expose Win32, so the extension draws its
  windows through SWELL. The rest of the code is written once against the Win32
  API and works on both platforms.
- **Used:** dialog management, controls (buttons, edits, statics, combo boxes,
  list views), message boxes, file and folder pickers, owner-draw notifications.
- **Caveats:**
  - SWELL defines `min`/`max` macros that break `<algorithm>`; the target
    compiles with `NOMINMAX`.
  - Only `swell-modstub-generic.cpp` is compiled, with
    `SWELL_PROVIDED_BY_APP`: every SWELL call becomes a pointer that REAPER
    fills in when it loads the module, so nothing links against `libSwell.so`.
  - Its list view honours per-item text colour but not background colour.
  - Its combo popup is a native menu; long work started from the popup's
    notification disturbs it (see [ui.md](ui.md)).

### nlohmann/json

- **Origin:** `third_party/json`, version 3.11.3, single header.
- **Why:** reads and writes the settings file.
- **Used:** serialising and parsing `reaper_git.json` (see
  [reaper.md](reaper.md)).
- **Caveats:** header-only, and no other part of the project depends on it.

## How the pieces fit together

```
REAPER ──► reaper-sdk    entry point, API table, menu and timer hooks
   │
   ├── ui layer ──────►  SWELL (Linux) / Win32 (Windows)
   │
   └── git layer ─────►  libgit2 ──► libssh2 ──► SSH server
                            └─────► OpenSSL / Schannel ──► HTTPS server
   settings ──────────►  nlohmann/json ──► reaper_git.json
```

## Build configuration

`CMakeLists.txt` owns every third-party option; there are no patches to any
third-party source.

- libssh2 is added as a sub-directory, static output only, OpenSSL or WinCNG as
  crypto backend, archive placed in `build/lib`, and its include directory and
  library path are seeded into the cache variables that libgit2 looks for.
- libgit2 is configured with `USE_SSH=libssh2`, `USE_HTTPS=OpenSSL` (Linux) or
  `Schannel` (Windows), `USE_BUNDLED_ZLIB=Bundled`, `REGEX_BACKEND=builtin`, and
  with its CLI, tests and examples disabled.
- The module links `libgit2package` and `libssh2_static`; on Linux also OpenSSL,
  threads and the dl library, on Windows `user32`, `shell32`, `ole32`,
  `crypt32`, `secur32` and `ws2_32`.
- Third-party include directories are `SYSTEM`, so their warnings stay out of
  the project build log.
- `REAPER_GIT_VERSION` (default: the project version) is compiled into the panel
  as its version label. `scripts/build.sh` asks for it and passes it as
  `-DREAPER_GIT_VERSION=…`; the presets in `CMakePresets.json` build into
  `build/linux` and `build/windows`.

## Platform notes

- **Linux:** the module links `libssl`, `libcrypto`, `libstdc++`, `libgcc`,
  `libc` and `libm`, and is installed into REAPER's `UserPlugins` folder.
- **Windows:** the module links only system libraries; it can be built with
  MSVC or cross-compiled with the MinGW preset in `CMakePresets.json`.
- All application code is shared; only the SWELL stub and the link libraries
  differ per platform.

## Caveats

Each item is marked either **hard limit** (blocked by a dependency, with the
evidence) or **tradeoff** (a choice this project made that can be revisited).

- **Tradeoff:** updating a library means re-checking the shims. SWELL message
  behaviour, libgit2 callback structures and stash signatures have changed across
  versions before; the compile errors point at the rest.
- **Tradeoff:** secrets are obfuscated, not encrypted (see
  [reaper.md](reaper.md)). OpenSSL is already linked, so real encryption is
  possible; it was left out to keep the settings model simple.
- **Tradeoff:** a plain `git clone` leaves the two submodule folders empty and
  the build cannot configure until they are fetched; `scripts/build.sh` runs
  `git submodule update --init --recursive` when they are missing.
- **Tradeoff:** the vendored copies (REAPER SDK, WDL, json) are trimmed to what
  the build needs; the copy commands in `third_party/README.md` restore a full
  upstream checkout.

## Changing dependencies

1. Read the existing compile definitions in `CMakeLists.txt` first: several of
   them exist because of concrete build failures (`_DEFAULT_SOURCE`,
   `NOMINMAX`, `SWELL_PROVIDED_BY_APP`).
2. Keep everything static, so the module never requires extra shared objects
   beyond OpenSSL.
3. Update `third_party/README.md` with the new version and commit, and adjust
   this file wherever behaviour changed.
