#!/bin/sh
# Run in the Linux build environment against a newly packaged compositor.
set -eu
root=$(CDPATH= cd -- "${1:?bundle directory}" && pwd)
binary="$root/libexec/nativepipe-wayland"
# Compiler "used" alone does not protect unreferenced data from --gc-sections.
grep -aq "NPCS:$(sh guest/compositor/source-hash.sh)" "$binary"
grep -aq 'NP_RUNTIME_PROBE:1' "$binary"

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
        libav*.so*|libswscale.so*|libswresample.so*|libpostproc.so*|libva*.so*|libvdpau.so*|libvpl.so*|libcuda.so*|libnvidia-*.so*|libx264.so*|libx265.so*|libvpx.so*|libSvtAv1Enc.so*|libmp3lame.so*|libopus.so*)
            echo "Unexpected bundled codec library: $library" >&2; exit 1 ;;
    esac
done
needed=$(patchelf --print-needed "$binary")
resolved=$(ldd "$binary")
if printf '%s\n' "$needed" | grep -Eq '^lib(avcodec|avutil|swscale|va|aom|yuv|cuda|nvidia)[.-]'; then
    echo 'Unexpected required system codec ABI' >&2; exit 1
fi
for notice in aom/LICENSE aom/PATENTS libyuv/LICENSE libyuv/PATENTS NVIDIA-NVENC-header.txt; do
    test -s "$root/LICENSES/$notice"
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
codec=$(printf '%s\n' "$needed" | head -1)
patchelf --replace-needed "$codec" libnativepipe-missing-test.so "$temporary/libexec/nativepipe-wayland"
if "$temporary/nativepipe-wayland" --stdio --session > "$temporary/stdout" 2> "$temporary/stderr"; then
    echo 'A compositor with a missing runtime library unexpectedly started.' >&2; exit 1
fi
test ! -s "$temporary/stdout"
grep -q 'NativePipe cannot load its Linux runtime libraries' "$temporary/stderr"
grep -q 'AV1 software encoding is bundled' "$temporary/stderr"
echo 'Remote package: static AV1, optional NVENC/VA-API, runtime diagnostics PASS'
