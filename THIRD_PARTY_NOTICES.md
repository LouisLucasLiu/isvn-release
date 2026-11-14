# Third-party notices

iSVN itself is licensed under the PolyForm Shield License 1.0.0 (see `LICENSE`).
The components below keep their own licenses.

## Included in the source tree

### BLAKE3 1.8.2

- Path: `third_party/blake3/`
- License: Apache License 2.0 or CC0 1.0 (dual), texts in `third_party/blake3/LICENSE_A2`
  and `third_party/blake3/LICENSE_CC0`.
- Use: compiled into `isvn` (portable C implementation).

## Used at build or run time, not included in the source tree

### SQLite

- License: public domain (<https://www.sqlite.org/copyright.html>).
- Use: linked statically into `isvn`. Release builds compile the amalgamation that
  ships with the Qt 6.7.3 sources (SQLite 3.46.1); source builds use the SQLite
  development package of the build system.

### Qt 6.7.3

- License: GNU LGPL v3 for the modules used (Core, Gui, Qml, Quick, QuickControls2,
  DBus, Network), plus Qt's own third-party notices.
- Use: `isvn-gui` links Qt dynamically. Qt is not included in the source tree or in
  the Portable package; you install Qt yourself and the GUI loads it from that
  installation.

### Subversion

- `isvn` runs the `svn`, `svnadmin` and `svnmucc` programs as separate processes; it
  does not link Subversion libraries. Source builds use the Subversion installed on
  the system.
- The Portable package includes Subversion 1.14.1 and its libraries; their licenses,
  versions and source package names are listed in the package under
  `third_party/subversion-and-deps/`. The source RPMs of the components licensed
  under the GNU GPL or LGPL are published with every release as
  `isvn-<version>-<commit>-third-party-sources.tar.gz`.
