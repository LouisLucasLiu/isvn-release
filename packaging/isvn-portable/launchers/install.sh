#!/bin/sh
# Installs this iSVN Portable package for the current user, reusing an
# externally installed Qt 6.7.3 through a `qt` symlink.
# No Qt is copied and no root access is needed.
#
#   ./install.sh [--qt-dir <Qt root>] [--prefix <dir>] [--yes]
#
# Qt root, in this order: --qt-dir; the Qt of an existing iSVN installation in
# the prefix (current/qt); the Qt of a Terminux installation in its default
# location (~/.local/opt/terminux/current/qt).  The new version becomes
# `current` only after isvn-qt-check passes.
set -eu
bundle=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
prefix="$HOME/.local/opt/isvn"
qt_arg=""
yes=0
while [ $# -gt 0 ]; do
    case "$1" in
        --qt-dir) qt_arg=${2:-}; shift 2 ;;
        --prefix) prefix=${2:-}; shift 2 ;;
        --yes) yes=1; shift ;;
        *) echo "usage: install.sh [--qt-dir <Qt root>] [--prefix <dir>] [--yes]" >&2; exit 2 ;;
    esac
done
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

case "$(uname -m)" in x86_64) ;; *) die "only x86_64 is supported" ;; esac
glibc=$(getconf GNU_LIBC_VERSION | awk '{print $2}')
case "$glibc" in 2.3[4-9]*|2.[4-9][0-9]*) ;; *) die "glibc $glibc is older than 2.34" ;; esac
case "$prefix" in /*) ;; *) prefix="$PWD/$prefix" ;; esac
[ "$prefix" != "/" ] && [ "$prefix" != "$HOME" ] || die "the prefix must not be / or HOME"
version=$(cat "$bundle/VERSION")
final="$prefix/$version"
partial="$prefix/$version.partial"
[ ! -e "$final" ] && [ ! -L "$final" ] || die "already installed: $final"
[ ! -e "$partial" ] && [ ! -L "$partial" ] || die "an earlier attempt left $partial; check and remove it first"

(cd "$bundle" && sha256sum --quiet -c SHA256SUMS) || die "package files do not match SHA256SUMS"
echo "Package SHA256SUMS: OK"

source=""
if [ -n "$qt_arg" ]; then
    case "$qt_arg" in /*) qt="$qt_arg" ;; *) qt="$PWD/$qt_arg" ;; esac
    source="--qt-dir"
elif [ -L "$prefix/current/qt" ]; then
    qt=$(readlink -f -- "$prefix/current/qt"); source="existing iSVN installation ($prefix/current/qt)"
elif [ -L "$HOME/.local/opt/terminux/current/qt" ] && [ -d "$HOME/.local/opt/terminux/current/qt" ]; then
    qt=$(readlink -f -- "$HOME/.local/opt/terminux/current/qt"); source="Terminux installation ($HOME/.local/opt/terminux/current/qt)"
else
    die "no Qt root: give --qt-dir <Qt 6.7.3 gcc_64 root>"
fi
[ -d "$qt" ] || die "Qt root does not exist: $qt"
echo "Qt root: $qt"
echo "  from: $source"
echo "  canonical: $(readlink -f -- "$qt")"

if [ $yes -eq 0 ]; then
    printf 'Install iSVN %s into %s? [yes/no] ' "$version" "$final"
    read -r answer
    [ "$answer" = yes ] || die "cancelled"
fi

mkdir -p "$prefix"
mkdir -m 700 "$partial"
cleanup() { [ -d "$partial" ] && rm -rf -- "$partial" && echo "Removed $partial; current is unchanged."; }
trap cleanup EXIT INT TERM
(cd "$bundle" && tar -cf - .) | (cd "$partial" && tar -xf -)
ln -s "$qt" "$partial/qt"
printf '{"requested": "%s", "canonical": "%s", "source": "%s"}\n' \
    "$qt" "$(readlink -f -- "$qt")" "$source" > "$partial/share/qt-reference.json"
"$partial/isvn-qt-check" --prefix "$partial" || die "Qt check failed; nothing was installed"
mv -- "$partial" "$final"
trap - EXIT INT TERM
ln -s "$version" "$prefix/.current-$$"
mv -T -- "$prefix/.current-$$" "$prefix/current"
echo "Installed: $final"
echo "current -> $version"
echo "CLI: $prefix/current/isvn    GUI: $prefix/current/isvn-gui"
