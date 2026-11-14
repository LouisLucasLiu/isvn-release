#!/bin/sh
# Starts the isvn GUI against the Qt installation selected for this install.
#
# The install directory holds a "qt" symlink to the Qt prefix chosen at install
# or deploy time.  That prefix is put first on the library search path so a Qt
# from the caller's environment cannot replace it, and Qt plugin/QML overrides
# are cleared so plugins and QML modules come from the same prefix.  The GUI
# only starts the isvn CLI (which starts svn); neither uses the Qt libraries.
set -eu

self=$(readlink -f -- "$0")
dir=${self%/*}
gui="$dir/libexec/isvn-gui"
qt="$dir/qt"

fail() {
    printf 'isvn-gui: %s\n' "$*" >&2
    exit 127
}

[ -x "$gui" ] || fail "GUI binary not found: $gui"

want=""
[ -r "$gui.qt-version" ] && want=$(cat "$gui.qt-version")

[ -e "$qt/lib/libQt6Core.so.6" ] ||
    fail "Qt ${want:-6} is not configured for this installation ($qt)." \
         "Install Qt first, then set its prefix with 'install.sh --qt-prefix <dir>'" \
         "or 'deploy_release.sh --qt-prefix <dir>'."

if [ -n "$want" ]; then
    core=$(readlink -f -- "$qt/lib/libQt6Core.so.6")
    case "$core" in
        *".so.$want") ;;
        *) fail "Qt version mismatch: built with Qt $want, but $qt provides ${core##*/}" ;;
    esac
fi

LD_LIBRARY_PATH="$qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
unset QT_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH QT_QPA_PLATFORM_PLUGIN_PATH

exec "$gui" "$@"
