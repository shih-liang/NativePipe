#!/bin/sh
# Package the native command, native FSKit module, translations and licenses.
set -eu
binary=${1:?nativepipe binary}
out=${2:?output directory}
filesystem_binary=${3:-"$(dirname -- "$binary")/NativePipeFileSystemExtension"}
# FSKit entitlements without issued profiles make macOS terminate the executable
# before main. Reject missing authorization before creating/replacing output.
python3 scripts/verify-fskit-profiles.py >/dev/null
lipo "$binary" -verify_arch arm64 x86_64
lipo "$filesystem_binary" -verify_arch arm64 x86_64
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
python3 scripts/verify-fskit-profiles.py --json > "$stage/signing.json"
mkdir "$stage/bin"
app="$stage/NativePipe.app"
module="$app/Contents/Extensions/NativePipeFileSystemExtension.appex"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$module/Contents/MacOS" "$module/Contents/Resources"
install -m0755 "$binary" "$app/Contents/MacOS/nativepipe"
install -m0755 "$filesystem_binary" "$module/Contents/MacOS/NativePipeFileSystemExtension"
# The command's actual process has a fixed signed application identity. Neither
# application nor extension bundles are generated while the command runs.
cat > "$stage/bin/nativepipe" <<'ENTRY'
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
ENTRY
chmod 0755 "$stage/bin/nativepipe"
resources="$(dirname -- "$binary")/NativePipe_NativePipeStrings.bundle"
[ -d "$resources" ] || { echo "missing NativePipe localization bundle" >&2; exit 1; }
cp -R "$resources" "$app/Contents/Resources/"
cp -R "$resources" "$module/Contents/Resources/"
cp -R Resources/FileSystem/. "$module/Contents/Resources/"
cp "$NATIVEPIPE_PROVISIONING_PROFILE" "$app/Contents/embedded.provisionprofile"
cp "$NATIVEPIPE_FILESYSTEM_PROVISIONING_PROFILE" "$module/Contents/embedded.provisionprofile"
python3 - "$stage" <<'PY'
import json
from pathlib import Path
import plistlib
import sys

stage = Path(sys.argv[1])
app = stage / "NativePipe.app"
module = app / "Contents/Extensions/NativePipeFileSystemExtension.appex"
version = Path("Sources/NativePipeStrings/Resources/VERSION").read_text().strip()
languages = ["en", "zh-Hans", "zh-Hant", "ja", "ko", "fr", "de", "es", "pt-BR", "it", "ru"]
host = {"CFBundleExecutable": "nativepipe", "CFBundleIdentifier": "com.nativepipe.cli",
        "CFBundleName": "NativePipe", "CFBundleDisplayName": "NativePipe",
        "CFBundleDevelopmentRegion": "en", "CFBundleLocalizations": languages,
        "CFBundlePackageType": "APPL", "CFBundleShortVersionString": version, "CFBundleVersion": version,
        "LSMinimumSystemVersion": "14.0", "NSHighResolutionCapable": True}
(app / "Contents/Info.plist").write_bytes(plistlib.dumps(host))
info = plistlib.loads(Path("Resources/NativePipeFileSystem-Info.plist").read_bytes())
info.update(CFBundleShortVersionString=version, CFBundleVersion=version, CFBundleLocalizations=languages)
(module / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
authorization = json.loads((stage / "signing.json").read_text())
for filename, field in (("NativePipe", "NATIVEPIPE_PROVISIONING_PROFILE"),
                        ("NativePipeFileSystem", "NATIVEPIPE_FILESYSTEM_PROVISIONING_PROFILE")):
    grant = authorization[field]
    entitlements = plistlib.loads(Path("Resources/" + filename + ".entitlements").read_bytes())
    entitlements["com.apple.application-identifier"] = grant["applicationIdentifier"]
    entitlements["com.apple.developer.team-identifier"] = grant["team"]
    if grant["debuggable"]:
        entitlements["com.apple.security.get-task-allow"] = True
    (stage / (filename + ".entitlements")).write_bytes(plistlib.dumps(entitlements))
PY
# Sign inside out. Keep resource bundles immutable once their enclosing code has
# been signed, and never replace an FSKit signature with an ad-hoc signature.
sign_id=${SIGN_ID:-Developer ID Application}
codesign --force --options runtime --sign "$sign_id" --entitlements "$stage/NativePipeFileSystem.entitlements" "$module"
codesign --force --options runtime --sign "$sign_id" --entitlements "$stage/NativePipe.entitlements" "$app"
python3 scripts/verify-macos-package.py "$app"
version=$(cat Sources/NativePipeStrings/Resources/VERSION)
[ "$("$stage/bin/nativepipe" --version)" = "nativepipe $version" ] || {
    echo "Command version differs from release source" >&2; exit 1
}
for directory in Sources/NativePipeStrings/Resources/*.lproj; do
    language=$(basename "$directory" .lproj)
    "$stage/bin/nativepipe" --help -AppleLanguages "($language)" >/dev/null
done
cp Sources/NativePipeStrings/Resources/VERSION "$stage/VERSION"
cp LICENSE "$stage/LICENSE"
cp -R LICENSES "$stage/LICENSES"
cp -R .build/codecs/macos/LICENSES/. "$stage/LICENSES/"
if otool -L "$binary" | grep -E '(libdav1d|libyuv|libaom)'; then
    echo "Codec dependencies must be statically linked" >&2; exit 1
fi
cat > "$stage/README.txt" <<'README'
NativePipe for macOS 14 or later (Apple Silicon and Intel)

Run: bin/nativepipe --install-compositor user@linux-host application
Run: bin/nativepipe --help

NativePipe.app supplies the command's signed macOS identity. Its bundled native
FSKit module shares selected Linux files on demand on macOS 27 or later. Enable
NativePipe Shared Files in System Settings > General > Login Items & Extensions
> File System Extensions before sharing files.
Linux compositor packages are separate assets in the same release.
README
# Build the archive entirely in the owned stage, then replace only the final
# asset. Missing profiles, failed signing or failed validation leave output intact.
archive=$(mktemp "$stage/nativepipe-archive.XXXXXX")
COPYFILE_DISABLE=1 tar --no-xattrs -C "$stage" -czf "$archive" bin NativePipe.app VERSION LICENSE LICENSES README.txt
mkdir -p "$out"
mv -f "$archive" "$out/nativepipe-macos-universal.tar.gz"
