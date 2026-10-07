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
app="$stage/NativePipe.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
install -m0755 "$binary" "$app/Contents/MacOS/nativepipe"
# The CLI remains a command; its actual process has a fixed signed application
# identity for native macOS notifications. No bundle is generated at runtime.
cat > "$stage/bin/nativepipe" <<'EOF'
#!/bin/sh
entry=$0
links=0
while [ -L "$entry" ]; do
    links=$((links + 1))
    [ "$links" -le 32 ] || { echo "nativepipe: command symlink loop" >&2; exit 1; }
    directory=$(CDPATH= cd -- "$(dirname -- "$entry")" && pwd)
    target=$(readlink "$entry")
    case "$target" in /*) entry=$target ;; *) entry=$directory/$target ;; esac
done
root=$(CDPATH= cd -- "$(dirname -- "$entry")/.." && pwd)
exec "$root/NativePipe.app/Contents/MacOS/nativepipe" "$@"
EOF
chmod 0755 "$stage/bin/nativepipe"
resources="$(dirname "$binary")/NativePipe_NativePipeStrings.bundle"
[ -d "$resources" ] || { echo "missing NativePipe localization bundle" >&2; exit 1; }
cp -R "$resources" "$app/Contents/Resources/"
for directory in Sources/NativePipeStrings/Resources/*.lproj; do
    language=$(basename "$directory" .lproj)
    # Native SwiftPM uses lowercase directories, including on case-sensitive disks.
    lowercase=$(printf '%s' "$language" | tr '[:upper:]' '[:lower:]')
    if [ ! -s "$app/Contents/Resources/NativePipe_NativePipeStrings.bundle/$language.lproj/Localizable.strings" ] &&
       [ ! -s "$app/Contents/Resources/NativePipe_NativePipeStrings.bundle/$lowercase.lproj/Localizable.strings" ] &&
       [ ! -s "$app/Contents/Resources/NativePipe_NativePipeStrings.bundle/Contents/Resources/$language.lproj/Localizable.strings" ] &&
       [ ! -s "$app/Contents/Resources/NativePipe_NativePipeStrings.bundle/Contents/Resources/$lowercase.lproj/Localizable.strings" ]; then
        echo "missing NativePipe translations: $language" >&2; exit 1
    fi
done
version=$(cat Sources/NativePipeStrings/Resources/VERSION)
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>nativepipe</string>
<key>CFBundleIdentifier</key><string>com.nativepipe.cli</string>
<key>CFBundleName</key><string>NativePipe</string>
<key>CFBundleDisplayName</key><string>NativePipe</string>
<key>CFBundleDevelopmentRegion</key><string>en</string>
<key>CFBundleLocalizations</key><array>
<string>en</string><string>zh-Hans</string><string>zh-Hant</string>
<string>ja</string><string>ko</string><string>fr</string><string>de</string>
<string>es</string><string>pt-BR</string><string>it</string><string>ru</string>
</array>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$version</string>
<key>CFBundleVersion</key><string>$version</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
codesign --force --sign - "$app/Contents/MacOS/nativepipe"
codesign --force --sign - "$app"
codesign --verify --strict "$app"
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
# Validate that the command really runs with its app resource layout. The
# temporary distinct value prevents the CI source/build tree satisfying lookup.
app_version=$(find "$app/Contents/Resources/NativePipe_NativePipeStrings.bundle" -type f -name VERSION)
[ -f "$app_version" ]
printf '%s.resource-probe\n' "$version" > "$app_version"
[ "$("$stage/bin/nativepipe" --version)" = "nativepipe $version.resource-probe" ] || {
    echo "Application resources are not resolved from Contents/Resources" >&2; exit 1
}
cp Sources/NativePipeStrings/Resources/VERSION "$app_version"
codesign --force --sign - "$app"
codesign --verify --strict "$app"
[ "$("$stage/bin/nativepipe" --version)" = "nativepipe $version" ]
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

The command runs from the included NativePipe.app for its macOS identity.
The app is ad-hoc signed, not notarized with an Apple Developer ID.
Linux compositor packages are separate assets in the same release.
EOF
COPYFILE_DISABLE=1 tar --no-xattrs -C "$stage" -czf "$out/nativepipe-macos-universal.tar.gz" .
