# iSVN Portable — package recipe

Builds the iSVN Portable package, which **contains no Qt**.  At installation the
package is linked to an external Qt 6.7.3 root:

- `<prefix>/<version>/qt -> <Qt root>`
- the GUI binary carries `DT_RUNPATH $ORIGIN/../qt/lib`
- `libexec/qt.conf` contains `[Paths]` / `Prefix=../qt`
- no `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` or `QML2_IMPORT_PATH` is set

Subversion (`svn`, `svnadmin`, `svnmucc` and their libraries) **is** included,
taken unmodified from Rocky Linux 9 packages.  The Local Demo Kit is a separate
archive and is not built here.

## Build

```sh
# from a git checkout of the source
ISVN_APP_VERSION=<version> packaging/isvn-portable/make-package.sh \
    --source <checkout> --qt-install <Qt install root> --work <new directory>
# or from a source tree without .git (for example a release source archive)
ISVN_APP_VERSION=<version> packaging/isvn-portable/make-package.sh \
    --source <tree> --source-commit <full commit SHA> \
    --qt-install <Qt install root> --work <new directory>
```

`SOURCE_DATE_EPOCH` sets the time stamp of every archive entry (use the commit
time); `ISVN_PACKAGE_STATUS` sets the `status` field of `BUILD_INFO.json`.

`--qt-install` is the Qt online-installer root that contains `6.7.3/gcc_64`,
`6.7.3/Src` and `Tools/{CMake,Ninja}`.  A git `--source` must be a standalone
clone with no tracked changes.  Docker is required; the toolchain and Subversion are
installed only inside disposable `rockylinux:9` containers, so the binaries
need glibc 2.34 and `GLIBCXX_3.4.29` at most.

| Step | Script | Result |
|---|---|---|
| Build and test | `container/build.sh` | `cmake --install` tree; all CTest tests must pass |
| Subversion | `container/collect-svn.sh` | tools, library closure (glibc excluded), package/license/source-RPM records, and the exact source RPMs of the GPL/LGPL components (the build fails if one cannot be downloaded) |
| Package | `assemble.sh` | `isvn-<version>-<commit>-linux-x86_64/` and `.tar.gz`; refuses `-dirty` builds and any Qt file. Also `isvn-<version>-<commit>-third-party-sources.tar.gz` with the source RPMs of the bundled GPL/LGPL components |

Release-only link options set by `container/build.sh`:

| Option | Why |
|---|---|
| `-DOpenGL_GL_PREFERENCE=LEGACY` | link `libGL` instead of `libOpenGL`+`libGLX` (libOpenGL is not in a default Rocky Linux 9 desktop) |
| `-Wl,--as-needed` | drop libraries the binaries never use |
| `-Wl,--enable-new-dtags` | emit `DT_RUNPATH` instead of `DT_RPATH` |

## Package layout

| Path | Purpose |
|---|---|
| `install.sh` | installs into `~/.local/opt/isvn/<version>` (or `--prefix`), links `qt`, runs `isvn-qt-check`, then switches `current` |
| `isvn` | supported CLI entry point: puts `svn/bin` first on `PATH`; the GUI runs this file as its paired CLI |
| `isvn-gui` | GUI entry point; drops caller Qt overrides and other Qt directories on `LD_LIBRARY_PATH` |
| `isvn-qt-check` | checks the Qt root: exact version 6.7.3, required files (`share/qt-requirements.txt`), xcb system libraries, `qt` link, `qt.conf`, and that every Qt library resolves inside the shared root |
| `isvn-preflight` | machine check, including `isvn-qt-check` |
| `libexec/` | the real CLI and GUI binaries, `qt.conf` |
| `svn/` | Subversion tools (`bin/` wrappers set the library path for that process only) |

Running `libexec/isvn` directly bypasses the bundled Subversion and is not a
supported entry point.

## Qt root selection at install time

1. `--qt-dir <root>`
2. the Qt of an existing iSVN installation in the same prefix (`current/qt`)
3. the Qt of a Terminux installation in its default location
   (`~/.local/opt/terminux/current/qt`)

Otherwise the installer stops.  A Terminux installation in another prefix needs
`--qt-dir`.  Only Qt 6.7.3 is accepted.

## Licenses

`package/THIRD_PARTY_NOTICES.md` describes the bundled components; the package
records each bundled package with its license files under
`third_party/subversion-and-deps/`.
