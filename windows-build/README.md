# Building Kettle Edition on Windows

Kettle Edition is an unofficial build of qBittorrent 5.2.4. These files rebuild the Windows program the same way the
releases are built. Everything else about the source (CMake options, other platforms) is as in the main `INSTALL` file.

## What you need

Put these three folders next to each other (the build script calls this folder the *tools root*; by default it is the folder that
contains the repository):

| Folder | What | Version used for the releases |
|---|---|---|
| `Qt\` | Qt for MSVC 2022, 64-bit, from `qt.io` or `aqtinstall` | 6.8.3, `Qt\6.8.3\msvc2022_64` |
| `vcpkg\` | a clone of <https://github.com/microsoft/vcpkg> | any recent commit; the versions below are pinned by `vcpkg.json` |
| `deps\installed\` | the libraries vcpkg builds from `vcpkg.json` (this folder) | libtorrent-rasterbar 2.0.11, OpenSSL 3.6.5, zlib 1.3.2, Boost 1.91.0 |

Also install Visual Studio 2022 Build Tools (workload "Desktop development with C++", MSVC v143 and a Windows SDK) and Git for Windows.
CMake and Ninja are needed too; both come with Visual Studio's "C++ CMake tools for Windows" component.

### Libraries

Triplet `x64-windows-static-md` (static libraries, dynamic C runtime). From the tools root:

```powershell
git clone https://github.com/microsoft/vcpkg vcpkg
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install --triplet x64-windows-static-md `
    --x-manifest-root=<repository>\windows-build --x-install-root=deps\installed
```

Keep the tools root path short: vcpkg's build folders can go over the 260-character Windows path limit in a long path.
The first build of the libraries took about 30 minutes.

## Build

From the repository folder:

```powershell
.\windows-build\build.ps1 -Target gui     # desktop app: build-gui\qbittorrent.exe
.\windows-build\build.ps1 -Target nox     # headless build used by the tests: build-nox\qbittorrent-nox.exe
```

Useful options: `-Clean` (start from scratch), `-ToolsRoot <folder>`, `-BuildDir <folder>`.
The script also runs `windeployqt`, so the build folder contains the Qt files the program needs next to it.
The gui build also makes `kettle-update-helper.exe` there (plain Win32, no Qt); it installs updates after the program has exited
and has to be shipped in the same folder as `qbittorrent.exe`.

## Tests

```powershell
$env:QBT_NOX = "<full path of the exe>"        # the gui build also works
python -m unittest discover -s test/kettle_update -v
```

`test/webui_access` needs the helper package `qbtgw` (set `QBT_GATEWAY` to its folder); it is not part of this repository.

## Releases

A release is a `kettle-vX.Y.Z` tag with the program files, `kettle-update.json` and its signature `kettle-update.json.sig`.
The number is `QBT_EDITION_VERSION_*` in `src/base/version.h.in`. Installed copies check the signature against the public key
built into the program, so a build you make yourself will not accept updates signed with somebody else's key and the other way round.
