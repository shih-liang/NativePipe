#!/bin/sh
# Run in the Linux build environment against a newly packaged compositor.
set -eu
root=$(CDPATH= cd -- "${1:?bundle directory}" && pwd)
binary="$root/libexec/nativepipe-wayland"

# Notices belong only to objects in this bundle. All links must have been
# materialized so the archive does not depend on the builder's filesystem.
test ! -e "$root/LICENSES/distribution"
test -z "$(find "$root/LICENSES" -type l -print)"
while IFS="$(printf '\t')" read -r object package version; do
    test -n "$object" && test -n "$version"
    test -s "$root/LICENSES/packages/$package/package.txt"
done < "$root/LICENSES/bundled-packages.tsv"
for notice in "$root/LICENSES/packages"/*; do
    package=${notice##*/}
    awk -F '\t' -v package="$package" '$2 == package { found=1 } END { exit !found }' \
        "$root/LICENSES/bundled-packages.tsv"
done

# Checking only libav* would miss the old ldd closure's codec dependencies.
for library in "$root"/lib/*.so*; do
    case "${library##*/}" in
        libav*.so*|libswscale.so*|libswresample.so*|libpostproc.so*|libva*.so*|libvdpau.so*|libvpl.so*|libx264.so*|libx265.so*|libvpx.so*|libSvtAv1Enc.so*|libmp3lame.so*|libopus.so*)
            echo "Unexpected bundled codec library: $library" >&2; exit 1 ;;
    esac
done
needed=$(patchelf --print-needed "$binary")
resolved=$(ldd "$binary")
for library in libavcodec libavutil libswscale; do
    printf '%s\n' "$needed" | grep -q "^$library[.]so[.]"
    path=$(printf '%s\n' "$resolved" | awk -v prefix="$library.so." 'index($1, prefix) == 1 && $2 == "=>" {print $3; exit}')
    case "$path" in
        "$root"/*) echo "$library must resolve outside the bundle" >&2; exit 1 ;;
        /*) test -f "$path" ;;
        *) echo "System $library is unavailable: $path" >&2; exit 1 ;;
    esac
done
output=$("$root/nativepipe-wayland" --check-runtime)
test -z "$output"

# A missing/incompatible system ABI must fail before any SSH protocol output.
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
mkdir "$temporary/libexec"
cp "$root/nativepipe-wayland" "$temporary/nativepipe-wayland"
cp "$binary" "$temporary/libexec/nativepipe-wayland"
ln -s "$root/lib" "$temporary/lib"
codec=$(printf '%s\n' "$needed" | grep '^libavcodec[.]so[.]')
patchelf --replace-needed "$codec" libavcodec-nativepipe-missing-test.so "$temporary/libexec/nativepipe-wayland"
if "$temporary/nativepipe-wayland" --stdio --session > "$temporary/stdout" 2> "$temporary/stderr"; then
    echo 'A compositor with missing FFmpeg unexpectedly started.' >&2; exit 1
fi
test ! -s "$temporary/stdout"
grep -q 'NativePipe cannot load its Linux runtime libraries' "$temporary/stderr"
grep -q 'library major versions must match' "$temporary/stderr"
echo 'Remote package: system FFmpeg, no FFmpeg-only codec dependencies, runtime diagnostics PASS'
