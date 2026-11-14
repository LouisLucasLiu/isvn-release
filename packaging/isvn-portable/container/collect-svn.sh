#!/bin/bash
# Collects the Subversion command-line tools and the shared libraries they
# load (glibc excluded) from Rocky Linux 9 packages, with their package,
# license and source-package records.
#
# Run as root inside a disposable rockylinux:9 container with /out (empty,
# writable) and optionally /var/cache/dnf mounted.  BUILD_UID owns the result.
set -euo pipefail
echo "keepcache=1" >> /etc/dnf/dnf.conf
# Bring the image up to the current repository state first: packages that the
# image preinstalled in an older release can be neither reinstalled (for their
# license files) nor matched to a source package once that release is gone.
dnf -y -q upgrade >/dev/null 2>&1
# svnmucc is in subversion-tools on Rocky Linux 9, not in subversion.
# tsflags= keeps documentation files: some packages ship their license text as
# documentation rather than as a %license file.
dnf -y -q --setopt=tsflags= install subversion subversion-tools file binutils >/dev/null 2>&1
mkdir -p /out/svn/libexec /out/svn/lib /out/meta/licenses
for tool in svn svnadmin svnmucc; do cp -p /usr/bin/$tool /out/svn/libexec/; done

ldd /usr/bin/svn /usr/bin/svnadmin /usr/bin/svnmucc | awk '/=> \//{print $3}' | sort -u \
    > /out/meta/closure.txt
# The host provides the C runtime itself.  libcrypt.so.2 (libxcrypt) is not
# part of it on every target: Ubuntu 24.04 only has libcrypt.so.1.
grep -vE '/(libc|libm|libpthread|libdl|librt|libresolv|ld-linux[^/]*|libutil|libnsl|libgcc_s)\.so' \
    /out/meta/closure.txt > /out/meta/bundled.txt || true
while read -r lib; do cp -pL "$lib" /out/svn/lib/; done < /out/meta/bundled.txt
echo "libraries bundled with svn: $(wc -l < /out/meta/bundled.txt) of $(wc -l < /out/meta/closure.txt)"

for f in /usr/bin/svn /usr/bin/svnadmin /usr/bin/svnmucc $(cat /out/meta/bundled.txt); do
    rpm -qf --qf '%{NAME}\t%{VERSION}-%{RELEASE}.%{ARCH}\t%{LICENSE}\t%{URL}\n' "$f"
done | sort -u > /out/meta/packages.tsv
for f in /usr/bin/svn /usr/bin/svnmucc $(cat /out/meta/bundled.txt); do
    rpm -qf --qf '%{NAME}\t%{SOURCERPM}\n' "$f"
done | sort -u > /out/meta/srpms.tsv
collect_licenses() { # package
    local p=$1
    rpm -qL "$p" 2>/dev/null | while read -r lf; do
        [ -f "$lf" ] && { mkdir -p "/out/meta/licenses/$p"; cp -p "$lf" "/out/meta/licenses/$p/"; }
    done || true
    # No %license file: take license-like files from the package documentation.
    if [ ! -d "/out/meta/licenses/$p" ]; then
        # A package may have no such file; grep then reports no match.
        rpm -qd "$p" 2>/dev/null | grep -Ei '/(copying|copyright|licen[cs]e|notice)[^/]*$' | while read -r lf; do
            [ -f "$lf" ] && { mkdir -p "/out/meta/licenses/$p"; cp -p "$lf" "/out/meta/licenses/$p/"; }
        done || true
    fi
}
for p in $(cut -f1 /out/meta/packages.tsv | sort -u); do
    collect_licenses "$p"
    # Packages preinstalled in the image were installed without documentation.
    if [ ! -d "/out/meta/licenses/$p" ]; then
        dnf -y -q --setopt=tsflags= reinstall "$p" >/dev/null 2>&1 || true
        collect_licenses "$p"
    fi
done

# Source packages.  The source RPM of every bundled component licensed under the
# GNU GPL or LGPL is published with the release; it must be exactly the source
# package recorded for the installed binary package, otherwise the build fails.
# bzip2: some upstream sources in source RPMs are .tar.bz2 archives.
dnf -y -q install dnf-plugins-core cpio unzip bzip2 >/dev/null 2>&1
mkdir -p /out/sources /work-src
source_rpm() { # package destination-directory -> path of the downloaded source RPM
    local p=$1 dest=$2 srpm
    srpm=$(rpm -q --qf '%{SOURCERPM}' "$p")
    if [ ! -f "$dest/$srpm" ]; then
        dnf -q download --source --enablerepo='*-source' --destdir "$dest" "$(rpm -q "$p")" >/dev/null 2>&1 || true
    fi
    [ -f "$dest/$srpm" ] || { echo "source package $srpm of $p could not be downloaded" >&2; return 1; }
    echo "$dest/$srpm"
}
awk -F'\t' '$3 ~ /GPL/ {print $1}' /out/meta/packages.tsv | sort -u > /out/meta/copyleft_packages.txt
while read -r p; do source_rpm "$p" /out/sources > /dev/null; done < /out/meta/copyleft_packages.txt
(cd /out/sources && sha256sum -- *.src.rpm) > /out/meta/sources_sha256.txt
echo "source packages published: $(wc -l < /out/meta/sources_sha256.txt)"

# Packages still without a license file.
srpm_of() { rpm -q --qf '%{SOURCERPM}' "$1"; }
for p in $(cut -f1 /out/meta/packages.tsv | sort -u); do
    [ -d "/out/meta/licenses/$p" ] && continue
    # Another bundled package built from the same source package carries it.
    for q in $(cut -f1 /out/meta/packages.tsv | sort -u); do
        if [ "$q" != "$p" ] && [ -d "/out/meta/licenses/$q" ] && [ "$(srpm_of "$q")" = "$(srpm_of "$p")" ]; then
            cp -a "/out/meta/licenses/$q" "/out/meta/licenses/$p"
            break
        fi
    done
    [ -d "/out/meta/licenses/$p" ] && continue
    # The license file of the upstream source in the source package.
    if s=$(source_rpm "$p" /work-src); then
        d=$(mktemp -d /work-src/unpack.XXXXXX)
        (cd "$d" && rpm2cpio "$s" | cpio -idm --quiet) || true
        for t in "$d"/*.tar.* "$d"/*.tgz "$d"/*.zip; do
            [ -f "$t" ] || continue
            case "$t" in *.zip) (cd "$d" && unzip -q -o "$t") || true ;; *) tar -xf "$t" -C "$d" || true ;; esac
        done
        find "$d" -mindepth 1 -maxdepth 2 -type f | grep -Ei '/(copying|copyright|licen[cs]e|notice)[^/]*$' \
            | while read -r lf; do mkdir -p "/out/meta/licenses/$p"; cp -p "$lf" "/out/meta/licenses/$p/"; done || true
    fi
    [ -d "/out/meta/licenses/$p" ] && continue
    # Public domain components have no license text; record what the package states.
    lic=$(awk -F'\t' -v p="$p" '$1 == p {print $3}' /out/meta/packages.tsv)
    url=$(awk -F'\t' -v p="$p" '$1 == p {print $4}' /out/meta/packages.tsv)
    if [ "$lic" = "Public Domain" ]; then
        mkdir -p "/out/meta/licenses/$p"
        printf 'The %s package states its license as "Public Domain" and ships no license text.\nUpstream: %s\n' \
            "$p" "$url" > "/out/meta/licenses/$p/PUBLIC-DOMAIN.txt"
    fi
done
for p in $(cut -f1 /out/meta/packages.tsv | sort -u); do
    [ -d "/out/meta/licenses/$p" ] || echo "no license file found for package: $p"
done
find /var/cache/dnf -name '*.rpm' -exec sha256sum {} + | sed 's#  .*/#  #' | sort -k2 \
    > /out/meta/rpm_cache_sha256.txt
cat /out/meta/packages.tsv
chown -R "${BUILD_UID:-1000}" /out /var/cache/dnf
