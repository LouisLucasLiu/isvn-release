# iSVN Portable

The Portable package installs iSVN for one user without root access. It contains
the `isvn` command-line tool, the `isvn-gui` desktop application and the
Subversion tools iSVN uses (`svn`, `svnadmin`, `svnmucc`) with their libraries.
It does not contain Qt: the GUI uses a Qt 6.7.3 installation that you provide.

## Requirements

- Linux x86_64 with glibc 2.34 or newer (for example Rocky Linux 9 or Ubuntu 24.04)
- Qt 6.7.3 for Desktop, gcc_64, installed by you (for example with the Qt online
  installer)
- The X11/xcb system libraries Qt needs; `isvn-qt-check` names any missing library
  and the package that provides it
- No Subversion installation and no root access

## Install

    tar -xzf isvn-<version>-<commit>-linux-x86_64.tar.gz
    cd isvn-<version>-<commit>-linux-x86_64
    ./install.sh --qt-dir /path/to/Qt/6.7.3/gcc_64 --yes

The default location is `~/.local/opt/isvn/<version>`, with `current` pointing to
the installed version; use `--prefix` for another directory. The installer
creates `qt -> <Qt root>` and makes the version current only after
`isvn-qt-check` passes. `--qt-dir` can be left out when the prefix already has an
iSVN installation linked to Qt, or when a Terminux installation is present in its
default location; its Qt is then reused.

## Run

    ~/.local/opt/isvn/current/isvn-preflight
    ~/.local/opt/isvn/current/isvn version --json
    cd <workspace> && ~/.local/opt/isvn/current/isvn-gui

Always start the tools through `isvn`, `isvn-gui` and `svn/bin/*`. Running
`libexec/isvn` directly bypasses the bundled Subversion.

The GUI finds Qt through its RUNPATH and `libexec/qt.conf` (`Prefix=../qt`); no
`LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` or `QML2_IMPORT_PATH` is set.

## Verify

`SHA256SUMS` in the package lists every file:

    sha256sum -c SHA256SUMS

## Licenses

iSVN is licensed under the PolyForm Shield License 1.0.0 (`LICENSE`). Bundled
third-party components are listed in `THIRD_PARTY_NOTICES.md`.
