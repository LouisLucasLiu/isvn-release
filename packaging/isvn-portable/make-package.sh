#!/bin/bash
# Builds the iSVN Portable (shared-Qt) package from a clean commit.
#
#   make-package.sh --source <clean git checkout> --qt-install <Qt 6.7.3 install root> \
#                   --work <new directory> [--image rockylinux:9]
#   make-package.sh --source <exported source tree> --source-commit <40-hex sha> ...
#
# An exported source tree has no .git; its commit is then given explicitly and
# is compiled into the binaries and recorded in BUILD_INFO.json.
#
# --qt-install is the directory that contains 6.7.3/gcc_64, 6.7.3/Src and
# Tools/{CMake,Ninja} (the Qt online installer layout).  It is mounted
# read-only into the build container and is not copied into the package.
# Requires docker; packages are installed only inside disposable containers.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd -P)
image=rockylinux:9; source=""; qt=""; work=""; source_commit=""
while [ $# -gt 0 ]; do
    case "$1" in
        --source) source=$2; shift 2 ;;
        --qt-install) qt=$2; shift 2 ;;
        --work) work=$2; shift 2 ;;
        --image) image=$2; shift 2 ;;
        --source-commit) source_commit=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$source" ] && [ -n "$qt" ] && [ -n "$work" ] || {
    echo "usage: make-package.sh --source <dir> --qt-install <dir> --work <new dir> [--image <image>]" >&2; exit 2; }
source=$(cd -- "$source" && pwd -P); qt=$(cd -- "$qt" && pwd -P)
if [ -n "$source_commit" ]; then
    printf '%s' "$source_commit" | grep -Eqx '[0-9a-f]{40}' || {
        echo "make-package.sh: --source-commit must be a full lowercase SHA-1" >&2; exit 2; }
    [ ! -e "$source/.git" ] || { echo "make-package.sh: --source-commit is for trees without .git" >&2; exit 2; }
else
    # The build reads the commit inside a container, so the checkout must carry
    # its own .git directory (a linked worktree points outside the mount).
    [ -d "$source/.git" ] || { echo "make-package.sh: $source must be a standalone clone" >&2; exit 1; }
    [ -z "$(git -C "$source" status --porcelain --untracked-files=no)" ] || {
        echo "make-package.sh: $source has uncommitted changes" >&2; exit 1; }
    source_commit=$(git -C "$source" rev-parse HEAD)
fi
[ ! -e "$work" ] || { echo "make-package.sh: $work already exists" >&2; exit 1; }
[ -n "${ISVN_APP_VERSION:-}" ] || { echo "make-package.sh: set ISVN_APP_VERSION (for example 0.1.0)" >&2; exit 2; }
mkdir -p "$work/build" "$work/svn-stage" "$work/dnf-cache" "$work/dist"
uid=$(id -u)
image_id=$(docker image inspect "$image" --format '{{.Id}}')
echo "source $source_commit; image $image $image_id"

docker run --rm -e BUILD_UID="$uid" -e ISVN_SOURCE_COMMIT="$source_commit" \
    -e ISVN_APP_VERSION="$ISVN_APP_VERSION" \
    -v "$source:/src:ro" -v "$qt:/qt:ro" -v "$work/build:/work" \
    -v "$work/dnf-cache:/var/cache/dnf" -v "$here:/recipe:ro" \
    "$image" bash /recipe/container/build.sh

docker run --rm -e BUILD_UID="$uid" \
    -v "$work/svn-stage:/out" -v "$work/dnf-cache:/var/cache/dnf" -v "$here:/recipe:ro" \
    "$image" bash /recipe/container/collect-svn.sh

"$here/assemble.sh" "$work/build/install" "$work/svn-stage" "$source" "$work/dist" --image "$image_id" \
    --source-commit "$source_commit"
