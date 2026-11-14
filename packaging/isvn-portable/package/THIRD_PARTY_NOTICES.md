# Third-party components in the iSVN Portable package

iSVN itself is licensed under the PolyForm Shield License 1.0.0 (`LICENSE`).
The components below keep their own licenses.

## Subversion 1.14 and its libraries (`svn/`)

The Subversion command-line tools and the shared libraries they load are taken
unmodified from Rocky Linux 9 packages. iSVN runs these tools as separate
programs; the iSVN binaries do not link them.

- `third_party/subversion-and-deps/packages.tsv`: package, version, license and
  upstream URL of every bundled component
- `third_party/subversion-and-deps/srpms.tsv`: the source package of each component
- `third_party/subversion-and-deps/licenses/<package>/`: the license and notice
  files shipped with each package

Source code: the source RPMs of the bundled components licensed under the GNU GPL
or LGPL are published with every release as
`isvn-<version>-<commit>-third-party-sources.tar.gz`, next to this package.
`third_party/subversion-and-deps/sources_sha256.txt` lists them with their SHA-256.

## Linked into the iSVN binaries

- BLAKE3 1.8.2: Apache License 2.0 or CC0 1.0, included in the iSVN source tree
- SQLite 3.46.1: public domain, compiled from the amalgamation that ships with the
  Qt 6.7.3 sources
- The C and C++ runtime of the system (glibc, libstdc++, libgcc) is used and is not
  included

## Qt — used, not included

The package contains no Qt files. At installation it is linked to a Qt 6.7.3
installation that you provide (`qt -> <Qt root>`). The Qt modules iSVN uses are
licensed under the GNU LGPL version 3; Qt's own third-party notices are part of
that Qt installation.
