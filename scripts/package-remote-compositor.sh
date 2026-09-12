#!/bin/sh
# Package the remote compositor with its non-system userspace libraries.
# Keep the target's NVIDIA/VA-API driver stack, libc and GLib/GIO authoritative.
set -eu
arch=${1:?architecture}
libc=${2:?libc}
binary="guest/compositor/dist/nativepipe-wayland-$arch-$libc"
out=${3:?output directory}
[ ! -d "$out" ] || [ -z "$(ls -A "$out")" ] || {
    echo "Package output must be empty: $out (old libraries must not survive a rebuild)." >&2
    exit 1
}
mkdir -p "$out/libexec" "$out/lib/pixbuf" "$out/LICENSES"
install -m0755 "$binary" "$out/libexec/nativepipe-wayland"
collect_license() (
    object=$(readlink -f "$1")
    package= origin= license= version=
    if command -v dpkg-query >/dev/null; then
        owner=$(dpkg-query -S "$object" 2>/dev/null || dpkg-query -S "$1")
        package=$(printf '%s\n' "$owner" | head -1 | sed 's/: \/.*//')
        version=$(dpkg-query -W -f='${Version}' "$package")
        package=${package%%:*}
    elif command -v pacman >/dev/null; then
        package=$(pacman -Qqo "$object")
        version=$(pacman -Q "$package" | cut -d ' ' -f2-)
        license=$(LC_ALL=C pacman -Qi "$package" | sed -n 's/^Licenses *: *//p')
    elif command -v apk >/dev/null; then
        owner=$(apk info --who-owns "$object" | sed 's/.* owned by //')
        # Read metadata for this exact installed package (not a name/version
        # guess, or the unrelated packages installed on the builder).
        metadata=$(awk -v owner="$owner" 'BEGIN { RS=""; FS="\n" }
            { p=v=o=l=""; for (i=1;i<=NF;i++) {
                if ($i ~ /^P:/) p=substr($i,3); if ($i ~ /^V:/) v=substr($i,3);
                if ($i ~ /^o:/) o=substr($i,3); if ($i ~ /^L:/) l=substr($i,3);
              } if (p "-" v == owner) printf "%s\n%s\n%s\n%s\n",p,v,o,l;
            }' /lib/apk/db/installed)
        package=$(printf '%s\n' "$metadata" | sed -n '1p')
        version=$(printf '%s\n' "$metadata" | sed -n '2p')
        origin=$(printf '%s\n' "$metadata" | sed -n '3p')
        license=$(printf '%s\n' "$metadata" | sed -n '4p')
    fi
    [ -n "$package" ] || { echo "Cannot identify package owning $object" >&2; exit 1; }
    printf '%s\t%s\t%s\n' "${1##*/}" "$package" "$version" >> "$out/LICENSES/bundled-packages.tsv"
    notice="$out/LICENSES/packages/$package"
    [ ! -d "$notice" ] || return 0
    mkdir -p "$notice"
    printf 'Package: %s\nVersion: %s\nLicense: %s\n' "$package" "$version" "$license" > "$notice/package.txt"
    for name in "$package" ${origin:+"$origin"}; do
        if [ -d "/usr/share/licenses/$name" ]; then
            # Materialize links to shared notices; dangling links fail the
            # package build instead of shipping a broken attribution tree.
            cp -RL "/usr/share/licenses/$name/." "$notice/"
        fi
        if [ -r "/usr/share/doc/$name/copyright" ]; then
            cp -L "/usr/share/doc/$name/copyright" "$notice/copyright"
            for common in $(grep -Eo '/usr/share/common-licenses/[A-Za-z0-9.+-]+' "$notice/copyright" | sed 's/[.]$//' | sort -u); do
                [ ! -f "$common" ] || cp -L "$common" "$notice/"
            done
        fi
    done
    # Arch stores standard license texts centrally instead of per package.
    for identifier in $license; do
        if [ -f "/usr/share/licenses/spdx/$identifier.txt" ]; then
            cp -L "/usr/share/licenses/spdx/$identifier.txt" "$notice/"
        fi
    done
)
copy_library() (
    library=$1
    name=$2
    [ ! -e "$out/lib/$name" ] || return 0
    install -m0755 "$library" "$out/lib/$name"
    # Resolve only this object's dependencies from the private directory.
    # Unlike LD_LIBRARY_PATH, RUNPATH cannot leak into spawned desktop apps.
    patchelf --set-rpath '$ORIGIN' "$out/lib/$name"
    printf '%s\n' "$name" >> "$out/LICENSES/bundled-libraries.txt"
    collect_license "$library"
    dependencies "$library"
)
dependencies() (
    # Follow direct DT_NEEDED edges; system driver/GLib dependencies remain
    # authoritative and are not pulled into the private image-decoder closure.
    needed=$(patchelf --print-needed "$1")
    resolved=$(ldd "$1")
    printf '%s\n' "$needed" | while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$name" in
            libc.so*|libc.musl-*.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|ld-*|libgcc_s.so*|libstdc++.so*|libEGL.so*|libGL*.so*|libgbm.so*|libdrm*.so*|libglib-2.0.so*|libgobject-2.0.so*|libgio-2.0.so*|libgmodule-2.0.so*|libgthread-2.0.so*) continue ;;
            libavcodec.so*|libavdevice.so*|libavfilter.so*|libavformat.so*|libavutil.so*|libswresample.so*|libswscale.so*|libpostproc.so*|libva.so*|libva-*.so*|libvdpau.so*|libvpl.so*|libcuda.so*|libnvidia-*.so*) continue ;;
        esac
        library=$(printf '%s\n' "$resolved" | awk -v name="$name" '$1 == name && $2 == "=>" && $3 ~ /^\// {print $3; exit}')
        [ -f "$library" ] || { echo "Cannot resolve $name required by $1" >&2; exit 1; }
        copy_library "$library" "$name"
    done
)
dependencies "$binary"
patchelf --set-rpath '$ORIGIN/../lib' "$out/libexec/nativepipe-wayland"

# ldd cannot discover GdkPixbuf modules. Ship its matching PNG/XPM modules.
# GIO deliberately stays with the distribution and its dconf/GVfs modules.
loaders=$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)
query=$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0)
test -x "$query"
install -m0755 "$query" "$out/libexec/gdk-pixbuf-query-loaders"
collect_license "$query"
dependencies "$query"
patchelf --set-rpath '$ORIGIN/../lib' "$out/libexec/gdk-pixbuf-query-loaders"
for loader in "$loaders/libpixbufloader-png.so" "$loaders/libpixbufloader-xpm.so"; do
    [ -f "$loader" ] || continue
    install -m0755 "$loader" "$out/lib/pixbuf/$(basename "$loader")"
    collect_license "$loader"
    dependencies "$loader"
    patchelf --set-rpath '$ORIGIN/..' "$out/lib/pixbuf/$(basename "$loader")"
done
install -m0755 guest/compositor/remote-launcher.sh "$out/nativepipe-wayland"
cp LICENSES/* "$out/LICENSES/"

# AV1 encoder and pixel conversion are statically linked from pinned sources.
codec_prefix=${CODEC_PREFIX:-.build/codecs/$arch-$libc}
test -s "$codec_prefix/LICENSES/aom/LICENSE"
test -s "$codec_prefix/LICENSES/aom/PATENTS"
cp -R "$codec_prefix/LICENSES/." "$out/LICENSES/"
sed -n '1,25p' guest/encoder/vendor/nvEncodeAPI.h > "$out/LICENSES/NVIDIA-NVENC-header.txt"
