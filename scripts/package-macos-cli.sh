#!/bin/sh
# The CLI is a single universal Mach-O plus its license notices.
set -eu
binary=${1:?nativepipe binary}
out=${2:?output directory}
lipo "$binary" -verify_arch arm64
lipo "$binary" -verify_arch x86_64
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir "$stage/bin"
install -m0755 "$binary" "$stage/bin/nativepipe"
codesign --force --sign - "$stage/bin/nativepipe"
codesign --verify --strict "$stage/bin/nativepipe"
"$stage/bin/nativepipe" --help >/dev/null
cp -R LICENSES "$stage/LICENSES"
cp -R .build/codecs/macos/LICENSES/. "$stage/LICENSES/"
if otool -L "$binary" | grep -E '(libdav1d|libyuv|libaom)'; then
    echo "Codec dependencies must be statically linked" >&2; exit 1
fi
cat > "$stage/README.txt" <<'EOF'
NativePipe for macOS 14 or later (Apple Silicon and Intel)

Run: bin/nativepipe --install-compositor user@linux-host application
Run: bin/nativepipe --help

The command is ad-hoc signed, not notarized with an Apple Developer ID.
Linux compositor packages are separate assets in the same release.
EOF
COPYFILE_DISABLE=1 tar --no-xattrs -C "$stage" -czf "$out/nativepipe-macos-universal.tar.gz" .
