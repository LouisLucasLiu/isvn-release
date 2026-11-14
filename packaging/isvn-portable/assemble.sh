#!/bin/bash
# Assembles the iSVN Portable package for the shared-Qt layout.
#
#   assemble.sh <install-tree> <svn-stage> <source-tree> <out-dir> [--image <id>] [--source-commit <sha>]
#
# <install-tree>  output of `cmake --install` (isvn, isvn-gui, libexec/)
# <svn-stage>     output of container/collect-svn.sh (svn/, meta/)
# The package contains no Qt files; installation links it to an external Qt.
# A build from uncommitted source is refused, so the package identity always
# names a commit.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd -P)
[ $# -ge 4 ] || { echo "usage: assemble.sh <install-tree> <svn-stage> <source-checkout> <out-dir> [--image <id>]" >&2; exit 2; }
install_tree=$(cd -- "$1" && pwd -P); svn_stage=$(cd -- "$2" && pwd -P)
source_tree=$(cd -- "$3" && pwd -P); out=$4; image=""
shift 4; source_commit=""
while [ $# -gt 0 ]; do
    case "$1" in
        --image) image=$2; shift 2 ;;
        --source-commit) source_commit=$2; shift 2 ;;
        *) echo "assemble.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

identity=$("$install_tree/isvn" version --json)
version=$(printf '%s' "$identity" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["version"])')
commit=$(printf '%s' "$identity" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["commit"])')
case "$commit" in *-dirty|unknown) echo "assemble.sh: refusing a build from '$commit' source" >&2; exit 1 ;; esac
full_commit=${source_commit:-$(git -C "$source_tree" rev-parse HEAD)}
case "$full_commit" in "$commit"*) ;; *) echo "assemble.sh: build commit $commit does not match $full_commit" >&2; exit 1 ;; esac

pkg_version="$version-$commit"
name="isvn-$pkg_version-linux-x86_64"
pkg="$out/$name"
[ ! -e "$pkg" ] || { echo "assemble.sh: $pkg already exists" >&2; exit 1; }
mkdir -p "$pkg/libexec" "$pkg/share" "$pkg/svn/bin" "$pkg/third_party/subversion-and-deps"

cp -p "$install_tree/isvn" "$pkg/libexec/isvn"
cp -p "$install_tree/libexec/isvn-gui" "$install_tree/libexec/isvn-gui.qt-version" "$pkg/libexec/"
cp -p "$here/launchers/qt.conf" "$pkg/libexec/qt.conf"
cp -p "$here/launchers/qt-requirements.txt" "$pkg/share/qt-requirements.txt"
for f in isvn isvn-gui isvn-qt-check isvn-preflight install.sh; do cp -p "$here/launchers/$f" "$pkg/$f"; done
cp -p "$here/package/README-PORTABLE.md" "$here/package/THIRD_PARTY_NOTICES.md" "$pkg/"
cp -a "$svn_stage/svn/libexec" "$svn_stage/svn/lib" "$pkg/svn/"
for tool in svn svnadmin svnmucc; do cp -p "$here/launchers/svn-tool" "$pkg/svn/bin/$tool"; done
cp -a "$svn_stage/meta/licenses" "$pkg/third_party/subversion-and-deps/"
cp -p "$svn_stage/meta/packages.tsv" "$svn_stage/meta/srpms.tsv" "$svn_stage/meta/sources_sha256.txt" \
    "$pkg/third_party/subversion-and-deps/"
cp -p "$source_tree/LICENSE" "$pkg/LICENSE"
echo "$pkg_version" > "$pkg/VERSION"
srcname="isvn-$pkg_version-third-party-sources"
python3 -I - "$identity" "$full_commit" "$image" "${ISVN_PACKAGE_STATUS:-development build}" "$srcname.tar.gz" \
    > "$pkg/BUILD_INFO.json" <<'PY'
import json, sys
identity, commit, image, status, sources = json.loads(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
print(json.dumps({
    "product": "iSVN Portable (shared Qt)",
    "status": status,
    "isvn_identity": identity,
    "source_commit": commit,
    "build_image": image or "not recorded",
    "qt": "not included; an external Qt 6.7.3 root is linked at install time "
          "(qt symlink, DT_RUNPATH $ORIGIN/../qt/lib, libexec/qt.conf Prefix=../qt)",
    "third_party_sources": sources,
}, indent=2))
PY

if find "$pkg" \( -name 'libQt6*' -o -name 'libicu*' -o -name qmldir -o -path '*/plugins/*' \) | grep -q .; then
    echo "assemble.sh: Qt files found in the package" >&2; exit 1
fi
(cd "$pkg" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum | sed 's#  \./#  #') \
    > "$pkg/SHA256SUMS"
chmod -R u=rwX,go=rX "$pkg"
# Archive metadata carries no build-account names and no build-time ordering:
# numeric root ownership, name order, one fixed mtime and a gzip header without
# a name or time stamp.
epoch=${SOURCE_DATE_EPOCH:-0}
archive() { # directory name under $out
    (cd "$out" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
            -cf - "$1" | gzip -n -9 > "$1.tar.gz" && sha256sum "$1.tar.gz")
}
archive "$name"

# Source packages of the bundled GPL/LGPL components, published next to the
# Portable package.
src="$out/$srcname"
[ ! -e "$src" ] || { echo "assemble.sh: $src already exists" >&2; exit 1; }
ls "$svn_stage"/sources/*.src.rpm > /dev/null 2>&1 || { echo "assemble.sh: no source packages in $svn_stage/sources" >&2; exit 1; }
mkdir -p "$src"
cp -p "$svn_stage"/sources/*.src.rpm "$src/"
cp -p "$svn_stage/meta/packages.tsv" "$svn_stage/meta/srpms.tsv" "$src/"
cat > "$src/README.txt" <<EOF
Source packages of the third-party components in $name.tar.gz
that are licensed under the GNU GPL or LGPL.  They are the unmodified Rocky
Linux 9 source RPMs of the installed binary packages (srpms.tsv maps each
bundled package to its source package; packages.tsv lists the licenses).
Extract one with:  rpm2cpio <file>.src.rpm | cpio -idm
EOF
(cd "$src" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum | sed 's#  \./#  #') \
    > "$src/SHA256SUMS"
chmod -R u=rwX,go=rX "$src"
archive "$srcname"
