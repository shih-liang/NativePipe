#!/bin/sh
# Assemble exactly the three products from a single workflow's build outputs.
set -eu
input=${1:?Linux artifact directory}
macos=${2:?macOS artifact directory}
out=${3:?release output directory}
[ ! -e "$out" ] || { echo 'Release output must be new.' >&2; exit 1; }
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
for arch in aarch64 x86_64; do
    root="$stage/$arch"
    mkdir -p "$root/guest/compositor/dist" "$root/guest/session/dist"
    for libc in gnu musl; do
        source="$input/nativepipe-linux-$arch-$libc"
        cp "$source/nativepipe-compositor-$arch-$libc.tar.gz" "$out/"
        install -m0755 "$source/guest/compositor/dist/vmpipe-wayland-$arch-$libc" "$root/guest/compositor/dist/"
        for name in nativepipe-session nativepipe-align-blob nativepipe-vulkan-layer; do
            suffix=
            [ "$name" = nativepipe-session ] || suffix=.so
            install -m0755 "$source/guest/session/dist/$name-$arch-$libc$suffix" "$root/guest/session/dist/"
        done
    done
    cp -R guest/session/implicit_layer guest/session/openrc guest/session/profile.d guest/session/systemd "$root/guest/session/"
    cp -R LICENSES "$root/LICENSES"
    COPYFILE_DISABLE=1 tar -C "$root" -czf "$out/nativepipe-vm-compositor-$arch.tar.gz" .
done
cp "$macos/nativepipe-macos-universal.tar.gz" "$out/"
(cd "$out" && sha256sum *.tar.gz > SHA256SUMS)
