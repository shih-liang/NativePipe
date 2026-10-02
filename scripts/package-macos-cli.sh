#!/bin/sh
# Package the universal command, its UI translations and license notices.
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
resources="$(dirname "$binary")/NativePipe_NativePipeStrings.bundle"
[ -d "$resources" ] || { echo "missing NativePipe localization bundle" >&2; exit 1; }
cp -R "$resources" "$stage/bin/"
for directory in Sources/NativePipeStrings/Resources/*.lproj; do
    language=$(basename "$directory" .lproj)
    # Native SwiftPM uses lowercase directories, including on case-sensitive disks.
    lowercase=$(printf '%s' "$language" | tr '[:upper:]' '[:lower:]')
    if [ ! -s "$stage/bin/NativePipe_NativePipeStrings.bundle/$language.lproj/Localizable.strings" ] &&
       [ ! -s "$stage/bin/NativePipe_NativePipeStrings.bundle/$lowercase.lproj/Localizable.strings" ] &&
       [ ! -s "$stage/bin/NativePipe_NativePipeStrings.bundle/Contents/Resources/$language.lproj/Localizable.strings" ] &&
       [ ! -s "$stage/bin/NativePipe_NativePipeStrings.bundle/Contents/Resources/$lowercase.lproj/Localizable.strings" ]; then
        echo "missing NativePipe translations: $language" >&2; exit 1
    fi
done
codesign --force --sign - "$stage/bin/nativepipe"
codesign --verify --strict "$stage/bin/nativepipe"
# Execute the independently staged command in each supported language. This
# also checks resource discovery outside the build tree, including SwiftPM's
# lowercase language directory names.
for directory in Sources/NativePipeStrings/Resources/*.lproj; do
    language=$(basename "$directory" .lproj)
    "$stage/bin/nativepipe" --help -AppleLanguages "($language)" >/dev/null
done
cp LICENSE "$stage/LICENSE"
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
