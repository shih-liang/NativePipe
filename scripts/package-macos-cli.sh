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
version=$(cat Sources/NativePipeStrings/Resources/VERSION)
[ "$("$stage/bin/nativepipe" --version)" = "nativepipe $version" ] || {
    echo "Command version differs from release source" >&2; exit 1
}
cp Sources/NativePipeStrings/Resources/VERSION "$stage/VERSION"
# Execute the independently staged command in each supported language. This
# also checks resource discovery outside the build tree, including SwiftPM's
# lowercase language directory names.
for directory in Sources/NativePipeStrings/Resources/*.lproj; do
    language=$(basename "$directory" .lproj)
    "$stage/bin/nativepipe" --help -AppleLanguages "($language)" >/dev/null
done
# The same module is linked into native application helpers. Exercise their
# Contents/Resources layout with a distinct resource value, so the original
# CI build directory cannot silently satisfy a broken bundle lookup.
app="$stage/NativePipeResourceProbe.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$stage/bin/nativepipe" "$app/Contents/MacOS/"
cp -R "$stage/bin/NativePipe_NativePipeStrings.bundle" "$app/Contents/Resources/"
cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>nativepipe</string>
<key>CFBundleIdentifier</key><string>dev.nativepipe.resource-probe</string>
<key>CFBundlePackageType</key><string>APPL</string>
</dict></plist>
EOF
app_version=$(find "$app/Contents/Resources/NativePipe_NativePipeStrings.bundle" -type f -name VERSION)
[ -f "$app_version" ]
printf '%s.resource-probe\n' "$version" > "$app_version"
[ "$("$app/Contents/MacOS/nativepipe" --version)" = "nativepipe $version.resource-probe" ] || {
    echo "Application resources are not resolved from Contents/Resources" >&2; exit 1
}
"$app/Contents/MacOS/nativepipe" --help -AppleLanguages '(zh-Hans)' >/dev/null
rm -rf "$app"
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
