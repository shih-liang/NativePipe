#!/bin/sh
# Published with each release and bundled by LinPortal. stdout returns the
# installed executable; the offline upload handshake uses inherited fd 3.
set -eu

# Keep installation inside one complete function when consumed by curl | sh.
install_compositor() {
    mode=${1:?Expected --release URL or --upload}
    case "$mode" in
        --release) base=${2:?Expected a pinned release URL}; base=${base%/} ;;
        --upload) ;;
        *) echo "Unknown installation mode: $mode" >&2; exit 2 ;;
    esac
    arch=$(uname -m)
    case "$arch" in aarch64|x86_64) ;; *) echo "Unsupported architecture: $arch" >&2; exit 1 ;; esac
    libc=gnu
    if (ldd --version 2>&1 || :) | grep -i musl >/dev/null; then libc=musl; fi
    command -v sha256sum >/dev/null || { echo 'Install sha256sum to verify NativePipe.' >&2; exit 1; }
    root="$HOME/.local/share/nativepipe/compositor"
    mkdir -p "$root/releases"
    tmp=$(mktemp -d "$root/.install-XXXXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 1' HUP INT TERM
    asset="nativepipe-compositor-$arch-$libc.tar.gz"

    if [ "$mode" = --upload ]; then
        printf 'NATIVEPIPE TARGET %s %s\n' "$arch" "$libc" >&3
        IFS=' ' read -r digest size
        case "$size" in ''|*[!0-9]*) echo 'Invalid NativePipe archive size.' >&2; exit 1 ;; esac
        [ "${#size}" -le 9 ] && [ "$size" -gt 0 ] && [ "$size" -le 536870912 ] || {
            echo 'NativePipe archive exceeds the size limit.' >&2; exit 1;
        }
    else
        command -v curl >/dev/null || { echo 'Install curl to download NativePipe.' >&2; exit 1; }
        echo "Checking NativePipe ${base##*/}..." >&2
        curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
            --connect-timeout 10 --max-time 20 "$base/SHA256SUMS" -o "$tmp/SHA256SUMS"
        digest=$(awk -v asset="$asset" '$2 == asset { print $1 }' "$tmp/SHA256SUMS")
    fi
    case "$digest" in ''|*[!0-9a-fA-F]*) echo "Release has no valid checksum for $asset." >&2; exit 1 ;; esac
    [ "${#digest}" = 64 ] || { echo "Release has no unique checksum for $asset." >&2; exit 1; }
    installed="$root/releases/$digest"

    if [ ! -x "$installed/nativepipe-wayland" ] || [ "$(cat "$installed/.sha256" 2>/dev/null || :)" != "$digest" ]; then
        echo "Installing NativePipe for $arch ($libc)..." >&2
        if [ "$mode" = --upload ]; then
            printf 'NATIVEPIPE UPLOAD\n' >&3
            head -c "$size" > "$tmp/$asset"
            [ "$(wc -c < "$tmp/$asset")" -eq "$size" ] || { echo 'NativePipe upload was interrupted.' >&2; exit 1; }
        else
            curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
                --connect-timeout 10 --max-time 120 "$base/$asset" -o "$tmp/$asset"
        fi
        printf '%s  %s\n' "$digest" "$asset" > "$tmp/checksum"
        (cd "$tmp" && sha256sum -c checksum >&2)
        mkdir "$tmp/unpacked"
        tar -xzf "$tmp/$asset" -C "$tmp/unpacked"
        [ -x "$tmp/unpacked/nativepipe-wayland" ] || { echo 'Release is missing nativepipe-wayland.' >&2; exit 1; }
        printf '%s\n' "$digest" > "$tmp/unpacked/.sha256"
        # Never overwrite a directory still used by another connection.
        if ! mv -T "$tmp/unpacked" "$installed" 2>/dev/null; then
            [ -x "$installed/nativepipe-wayland" ] && [ "$(cat "$installed/.sha256" 2>/dev/null || :)" = "$digest" ] || {
                echo 'Could not publish the NativePipe installation.' >&2; exit 1;
            }
        fi
    else
        echo 'NativePipe is up to date.' >&2
        if [ "$mode" = --upload ]; then printf 'NATIVEPIPE CACHED\n' >&3; fi
    fi

    "$installed/nativepipe-wayland" --check-runtime >&2
    ln -s "releases/$digest" "$tmp/current"
    mv -Tf "$tmp/current" "$root/current"
    rm -rf "$tmp"
    trap - EXIT HUP INT TERM
    printf '%s\n' "$installed/nativepipe-wayland"
}

install_compositor "$@"
