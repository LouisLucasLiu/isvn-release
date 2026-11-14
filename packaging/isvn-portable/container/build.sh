#!/bin/bash
# Builds and tests iSVN inside a disposable rockylinux:9 container, so the
# binaries need no more than glibc 2.34 / GLIBCXX_3.4.29.
#
# Run as root inside the container with these mounts:
#   /src              the iSVN source checkout (read-only; a clean git checkout)
#   /qt               a Qt 6.7.3 installation root containing 6.7.3/gcc_64,
#                     6.7.3/Src and Tools/{CMake,Ninja} (read-only)
#   /work             an empty output directory owned by BUILD_UID
#   /var/cache/dnf    a package cache directory (optional; kept between runs)
#   /recipe           this packaging/isvn-portable directory (read-only)
# Environment: BUILD_UID (default 1000), ISVN_APP_VERSION (default 0.1.0-dev).
#
# Output: /work/install (cmake --install tree), /work/build-logs/.
set -euo pipefail

if [ "${1:-}" != --as-builder ]; then
    echo "keepcache=1" >> /etc/dnf/dnf.conf
    dnf -y -q install gcc-c++ make git-core mesa-libGL-devel libglvnd-devel libxkbcommon-devel \
        fontconfig freetype dbus-libs glib2 libxkbcommon subversion subversion-tools binutils \
        file which findutils >/dev/null 2>&1
    rpm -q gcc-c++ glibc libstdc++ git-core subversion subversion-tools
    chown -R "${BUILD_UID:-1000}" /var/cache/dnf
    useradd -u "${BUILD_UID:-1000}" -M -d /work/home builder
    exec runuser -u builder -- env ISVN_APP_VERSION="${ISVN_APP_VERSION:-0.1.0-dev}" \
        ISVN_SOURCE_COMMIT="${ISVN_SOURCE_COMMIT:-}" \
        bash "$0" --as-builder
fi

export LANG=C.UTF-8 HOME=/work/home GIT_OPTIONAL_LOCKS=0
mkdir -p "$HOME" /work/deps/sqlite/include /work/deps/sqlite/lib /work/build-logs
QT=/qt/6.7.3/gcc_64
CMAKE=/qt/Tools/CMake/bin/cmake
CTEST=/qt/Tools/CMake/bin/ctest
NINJA=/qt/Tools/Ninja/ninja
SQLITE=/qt/6.7.3/Src/qtbase/src/3rdparty/sqlite

echo "toolchain: $(gcc --version | head -n 1); $(ldd --version | head -n 1)"
if [ -d /src/.git ]; then
    echo "source: $(git -C /src rev-parse HEAD); tracked changes: $(git -C /src status --porcelain --untracked-files=no | wc -l)"
else
    echo "source: exported tree, commit ${ISVN_SOURCE_COMMIT:?exported tree needs ISVN_SOURCE_COMMIT}"
fi

# SQLite is linked statically from the amalgamation that ships with Qt.
grep -m1 '#define SQLITE_VERSION ' "$SQLITE/sqlite3.c"
gcc -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c "$SQLITE/sqlite3.c" \
    -o /work/deps/sqlite/sqlite3.o
ar rcs /work/deps/sqlite/lib/libsqlite3.a /work/deps/sqlite/sqlite3.o
cp "$SQLITE/sqlite3.h" /work/deps/sqlite/include/

# Release-only link options for the shared-Qt layout:
#   OpenGL_GL_PREFERENCE=LEGACY  link libGL instead of libOpenGL+libGLX
#   --as-needed                  drop libraries the binaries do not use
#   --enable-new-dtags           emit DT_RUNPATH ($ORIGIN/../qt/lib), not DT_RPATH
"$CMAKE" -S /src -B /work/build -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_BUILD_TYPE=Release -DISVN_BUILD_GUI=ON -DBUILD_TESTING=ON \
    -DISVN_APP_VERSION="$ISVN_APP_VERSION" -DISVN_SOURCE_COMMIT="${ISVN_SOURCE_COMMIT:-}" \
    -DCMAKE_PREFIX_PATH="$QT" \
    -DSQLite3_INCLUDE_DIR=/work/deps/sqlite/include \
    -DSQLite3_LIBRARY=/work/deps/sqlite/lib/libsqlite3.a \
    -DOpenGL_GL_PREFERENCE=LEGACY \
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--as-needed -Wl,--enable-new-dtags" \
    "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--as-needed -Wl,--enable-new-dtags" \
    "-DCMAKE_C_FLAGS=-ffile-prefix-map=/src=. -ffile-prefix-map=/work=." \
    "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=/src=. -ffile-prefix-map=/work=." \
    > /work/build-logs/configure.log 2>&1
"$CMAKE" --build /work/build --parallel "$(nproc)" > /work/build-logs/build.log 2>&1

# All tests, offscreen, with a fresh HOME and TMPDIR and a UTF-8 locale.
(
    export HOME TMPDIR
    HOME=$(mktemp -d /tmp/home.XXXXXX)
    TMPDIR=$(mktemp -d /tmp/tmp.XXXXXX)
    cd /work/build
    QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$QT/lib" "$CTEST" --output-on-failure \
        > /work/build-logs/ctest.log 2>&1
)
ctest_rc=$?
grep -E 'tests passed|tests failed' /work/build-logs/ctest.log
[ $ctest_rc -eq 0 ] || { echo "ctest failed; see build-logs/ctest.log"; exit 1; }

"$CMAKE" --install /work/build --prefix /work/install > /work/build-logs/install.log 2>&1
echo "GUI dynamic section: $(readelf -d /work/install/libexec/isvn-gui | grep -E 'RPATH|RUNPATH' | sed 's/^ *//')"
/work/install/isvn version --json
