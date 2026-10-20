# Third-party code

Two kinds of third-party code live here:

* **Submodules** — `libgit2/` and `libssh2/`. Each is its own git repository,
  pinned to a release tag, so its sources are never committed in this project. A
  clone needs `git clone --recursive`, or `git submodule update --init --recursive`
  in an existing checkout, before the build can configure.
* **Copied files** — `reaper-sdk/`, `WDL/` and `json/`. Only what the build needs
  is checked in.

## Submodules

### libgit2

* Source: https://github.com/libgit2/libgit2
* Pinned: `v1.9.7`
* License: GPL-2.0 **with linking exception**, see `libgit2/COPYING` (the
  exception is what makes linking it into a closed-source plug-in fine)
* Built statically via `add_subdirectory(third_party/libgit2)`: HTTPS through
  OpenSSL on Linux and Schannel on Windows, bundled zlib, built-in regex engine,
  CLI/tests/examples disabled.

### libssh2

* Source: https://github.com/libssh2/libssh2
* Pinned: `libssh2-1.11.1`
* License: see `libssh2/COPYING`
* Built statically (`libssh2_static`) and handed to libgit2 through the
  `LIBSSH2_INCLUDE_DIR` / `LIBSSH2_LIBRARY` cache variables that its
  `find_package(LibSSH2)` looks at. Crypto backend: OpenSSL on Linux, WinCNG on
  Windows.

## Copied files

### reaper-sdk

* Source: https://github.com/justinfrankel/reaper-sdk
* Commit: `c0eafe87863b2bf69c5c822760f1b32a753b211b`
* Copied: `sdk/reaper_plugin.h`, `sdk/reaper_plugin_functions.h`, `sdk/LICENSE`
* License: see `reaper-sdk/LICENSE`

`reaper_plugin.h` expects WDL at `../WDL/swell/swell.h` relative to itself, which
is why the WDL files live in a sibling `WDL/` directory (same layout as
upstream). Keep that layout when updating.

### WDL (SWELL)

* Source: https://github.com/justinfrankel/WDL
* Commit: `9c8cab5842fb3edc1b3f511ac2d98ea010912f13`
* Copied: `WDL/swell/swell.h`, `swell-types.h`, `swell-functions.h`,
  `swell-modstub-generic.cpp`
* License: zlib-style license, reproduced in the header comments of the files
  above (Copyright (C) 2006 and later, Cockos, Inc.)

SWELL is only compiled on non-Windows targets. With `SWELL_PROVIDED_BY_APP`
defined, every SWELL function becomes a function pointer that REAPER fills in
through `SWELL_dllMain()` when it loads the extension, so nothing links against
`libSwell.so`.

### json

* Source: https://github.com/nlohmann/json
* Version: `3.11.3` (single header, `json/json.hpp`)

## Updating

Submodules: check out the new tag and let git record it.

```sh
git -C third_party/libssh2 fetch --tags
git -C third_party/libssh2 checkout libssh2-1.11.1
git add third_party/libssh2
```

Copied files: re-copy from upstream.

```sh
git clone --depth 1 https://github.com/justinfrankel/reaper-sdk /tmp/reaper-sdk
git clone --depth 1 https://github.com/justinfrankel/WDL /tmp/WDL
cp /tmp/reaper-sdk/sdk/{reaper_plugin.h,reaper_plugin_functions.h,LICENSE} third_party/reaper-sdk/
cp /tmp/WDL/WDL/swell/{swell.h,swell-types.h,swell-functions.h,swell-modstub-generic.cpp} third_party/WDL/swell/
```

Then rebuild — API changes show up as compile errors, and new REAPER functions
can be requested with `REAPERAPI_WANT_<name>` in `src/reaper_api.h`.
