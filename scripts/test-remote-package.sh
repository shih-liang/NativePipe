#!/bin/sh
# Run in the Linux build environment against a newly packaged compositor.
set -eu
step='bundle path'
temporary=
finish() {
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "Remote package check failed: $step (status $status)" >&2
    fi
    [ -z "$temporary" ] || rm -rf "$temporary"
    exit "$status"
}
trap finish EXIT
trap 'exit 1' HUP INT TERM
root=$(CDPATH= cd -- "${1:?bundle directory}" && pwd)
binary="$root/libexec/nativepipe-wayland"
step='np-open helper'
test -x "$root/libexec/np-open"
"$root/libexec/np-open" --help > /dev/null
# Compiler "used" alone does not protect unreferenced data from --gc-sections.
step='embedded binary stamps'
# ELF data is not locale-dependent text. In particular, the ARM64/musl
# checker returned no match for a stamp that strings extracted verbatim.
python3 - "$binary" "NPCS:$(sh guest/compositor/source-hash.sh)" <<'PYTHON'
import pathlib, sys
binary = pathlib.Path(sys.argv[1]).read_bytes()
for stamp in (sys.argv[2].encode('ascii'), b'NP_RUNTIME_PROBE:1'):
    if stamp not in binary:
        raise SystemExit(f'Missing binary stamp: {stamp.decode("ascii")}')
PYTHON

# Notices belong only to objects in this bundle. All links must have been
# materialized so the archive does not depend on the builder's filesystem.
step='bundled license inventory'
cmp LICENSE "$root/LICENSE"
cmp LICENSES/NOTICE "$root/LICENSES/NOTICE"
cmp LICENSES/source-inventory.json "$root/LICENSES/source-inventory.json"
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
    step="bundled library: ${library##*/}"
    case "${library##*/}" in
        libav*.so*|libswscale.so*|libswresample.so*|libpostproc.so*|libva*.so*|libvdpau.so*|libvpl.so*|libcuda.so*|libnvidia-*.so*|libx264.so*|libx265.so*|libvpx.so*|libSvtAv1Enc.so*|libmp3lame.so*|libopus.so*)
            echo "Unexpected bundled codec library: $library" >&2; exit 1 ;;
    esac
done
step='ELF dependency resolution'
needed=$(patchelf --print-needed "$binary")
resolved=$(ldd "$binary")
if printf '%s\n' "$needed" | grep -Eq '^lib(avcodec|avutil|swscale|va|aom|yuv|cuda|nvidia)[.-]'; then
    echo 'Unexpected required system codec ABI' >&2; exit 1
fi
for notice in aom/LICENSE aom/PATENTS libyuv/LICENSE libyuv/PATENTS NVIDIA-NVENC-header.txt; do
    step="codec notice: $notice"
    test -s "$root/LICENSES/$notice"
done
step='runtime preflight'
output=$("$root/nativepipe-wayland" --check-runtime)
test -z "$output"

# A missing/incompatible system ABI must fail before any SSH protocol output.
temporary=$(mktemp -d)
step='missing-library fault injection'
mkdir "$temporary/libexec"
cp "$root/nativepipe-wayland" "$temporary/nativepipe-wayland"
cp "$binary" "$temporary/libexec/nativepipe-wayland"
ln -s "$root/lib" "$temporary/lib"
codec=$(printf '%s\n' "$needed" | head -1)
patchelf --replace-needed "$codec" libnativepipe-missing-test.so "$temporary/libexec/nativepipe-wayland"
if "$temporary/nativepipe-wayland" --stdio --session > "$temporary/stdout" 2> "$temporary/stderr"; then
    echo 'A compositor with a missing runtime library unexpectedly started.' >&2; exit 1
fi
step='missing-library stdout remains empty'
test ! -s "$temporary/stdout"
step='missing-library actionable diagnostic'
grep -q 'NativePipe runtime check failed' "$temporary/stderr"
grep -q 'AV1 software encoding is bundled' "$temporary/stderr"

# Exercise the real ELF too, so bypassing the package wrapper cannot announce
# a ready session with broken input. Each failure must be immediate and silent
# on the binary protocol stream. timeout turns an accidental session into failure.
mkdir "$temporary/path" "$temporary/empty-xkb"
ln -s "$(command -v dirname)" "$temporary/path/dirname"
for entry in "$binary" "$root/nativepipe-wayland"; do
    for mode in preflight session; do
        if [ "$mode" = preflight ]; then set -- --check-runtime; else set -- --stdio --session; fi
        step="missing D-Bus: $entry $mode"
        status=0
        timeout 5 env PATH="$temporary/path" "$entry" "$@" > "$temporary/stdout" 2> "$temporary/stderr" || status=$?
        test "$status" -ne 0 && test "$status" -ne 124
        test ! -s "$temporary/stdout"
        grep -q 'dbus-run-session is required' "$temporary/stderr"
        step="missing XKB data: $entry $mode"
        status=0
        # New xkbcommon versions fall back to system data if ROOT is absent.
        # An existing empty root models genuinely missing data on both versions.
        timeout 5 env HOME="$temporary/no-home" XDG_CONFIG_HOME="$temporary/no-config" \
            XKB_CONFIG_ROOT="$temporary/empty-xkb" \
            XKB_CONFIG_EXTRA_PATH="$temporary/empty-xkb" \
            XKB_CONFIG_VERSIONED_EXTENSIONS_PATH="$temporary/empty-xkb" \
            XKB_CONFIG_UNVERSIONED_EXTENSIONS_PATH="$temporary/empty-xkb" \
            "$entry" "$@" > "$temporary/stdout" 2> "$temporary/stderr" || status=$?
        test "$status" -ne 0 && test "$status" -ne 124
        test ! -s "$temporary/stdout"
        grep -q 'keyboard layout' "$temporary/stderr"
        grep -q 'XKB_CONFIG_ROOT' "$temporary/stderr"
        grep -q 'xkeyboard-config' "$temporary/stderr"
    done
done
echo 'Remote package: static AV1, optional NVENC/VA-API, loader/D-Bus/XKB diagnostics PASS'
