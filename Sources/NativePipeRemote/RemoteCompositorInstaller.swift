/// Runs inside the authenticated SSH session, before the binary protocol starts.
/// Release files are immutable per digest: replacing an installation must never
/// overwrite libraries that another compositor still has mapped or may dlopen.
enum RemoteCompositorInstaller {
    private static let setup = #"""
    arch=$(uname -m)
    case "$arch" in aarch64|x86_64) ;; *) echo "Unsupported architecture: $arch" >&2; exit 1;; esac
    libc=gnu
    if ldd --version 2>&1 | head -1 | grep -qi musl; then libc=musl; fi
    command -v sha256sum >/dev/null || { echo 'Install sha256sum to verify NativePipe.' >&2; exit 1; }
    root="$HOME/.local/share/nativepipe/compositor"
    mkdir -p "$root/releases"
    tmp=$(mktemp -d "$root/.install-XXXXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 1' HUP INT TERM
    asset="nativepipe-compositor-$arch-$libc.tar.gz"
    """#

    private static let validateDigest = #"""
    case "$digest" in ''|*[!0-9a-fA-F]*) echo "Release has no valid checksum for $asset." >&2; exit 1;; esac
    [ "${#digest}" = 64 ] || { echo "Release has no unique checksum for $asset." >&2; exit 1; }
    installed="$root/releases/$digest"
    """#

    private static let publish = #"""
    printf '%s  %s\n' "$digest" "$asset" > "$tmp/checksum"
    (cd "$tmp" && sha256sum -c checksum >&2)
    mkdir "$tmp/unpacked"
    tar -xzf "$tmp/$asset" -C "$tmp/unpacked"
    [ -x "$tmp/unpacked/nativepipe-wayland" ] || { echo 'Release is missing nativepipe-wayland.' >&2; exit 1; }
    printf '%s\n' "$digest" > "$tmp/unpacked/.sha256"
    # A simultaneous connection may publish the same verified release first.
    # Rename on one filesystem; never merge directories or replace live files.
    if ! mv -T "$tmp/unpacked" "$installed" 2>/dev/null; then
      [ -x "$installed/nativepipe-wayland" ] && [ "$(cat "$installed/.sha256" 2>/dev/null || :)" = "$digest" ] || {
        echo 'Could not publish the NativePipe installation.' >&2; exit 1;
      }
    fi
    """#

    private static let finish = #"""
    # Verify the distribution's runtime ABI before changing the active version.
    "$installed/nativepipe-wayland" --check-runtime
    ln -s "releases/$digest" "$tmp/current"
    mv -Tf "$tmp/current" "$root/current"
    compositor="$installed/nativepipe-wayland"
    rm -rf "$tmp"
    trap - EXIT HUP INT TERM
    """#

    static let script = [setup, #"""
    command -v curl >/dev/null || { echo 'Install curl to download NativePipe.' >&2; exit 1; }
    # The Mac selected a compositor release and pinned its tag before SSH.
    # Runtime-only releases do not change this URL between downloads.
    base=${release:?NativePipe release URL was not resolved.}
    base=${base%/}
    tag=${base##*/}
    echo "Checking NativePipe ${tag}..." >&2
    curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
      --connect-timeout 10 --max-time 20 "$base/SHA256SUMS" -o "$tmp/SHA256SUMS"
    digest=$(awk -v asset="$asset" '$2 == asset { print $1 }' "$tmp/SHA256SUMS")
    """#, validateDigest, #"""
    if [ ! -x "$installed/nativepipe-wayland" ] || [ "$(cat "$installed/.sha256" 2>/dev/null || :)" != "$digest" ]; then
      echo "Installing NativePipe $tag for $arch ($libc)…" >&2
      curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --connect-timeout 10 --max-time 120 "$base/$asset" -o "$tmp/$asset"
    """#, publish, #"""
    else
      echo "NativePipe $tag is up to date." >&2
    fi
    """#, finish].joined(separator: "\n")

    /// This prelude uses the same SSH process as the display protocol. Only
    /// the matching archive is sent, and only when its digest is not installed.
    static let uploadScript = [setup, #"""
    printf 'NATIVEPIPE TARGET %s %s\n' "$arch" "$libc"
    IFS=' ' read -r digest size
    """#, validateDigest, #"""
    case "$size" in ''|*[!0-9]*) echo 'Invalid NativePipe archive size.' >&2; exit 1;; esac
    [ "${#size}" -le 9 ] && [ "$size" -gt 0 ] && [ "$size" -le 536870912 ] || {
      echo 'NativePipe archive exceeds the size limit.' >&2; exit 1;
    }
    if [ ! -x "$installed/nativepipe-wayland" ] || [ "$(cat "$installed/.sha256" 2>/dev/null || :)" != "$digest" ]; then
      echo "Installing bundled NativePipe for $arch ($libc)..." >&2
      printf 'NATIVEPIPE UPLOAD\n'
      head -c "$size" > "$tmp/$asset"
      [ "$(wc -c < "$tmp/$asset")" -eq "$size" ] || { echo 'NativePipe upload was interrupted.' >&2; exit 1; }
    """#, publish, #"""
    else
      echo 'Bundled NativePipe is up to date.' >&2
      printf 'NATIVEPIPE CACHED\n'
    fi
    """#, finish].joined(separator: "\n")
}
