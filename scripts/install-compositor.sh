#!/bin/sh
# Published with each release and bundled by LinPortal. stdout returns the
# installed executable; the offline upload handshake uses inherited fd 3.
set -eu

# Keep installation inside one complete function when consumed by curl | sh.
install_compositor() {
    # Messages for the person at the Mac. The Mac exports each one, already in
    # their language, as NATIVEPIPE_TEXT_<NAME>; run any other way, this prints
    # the English. Each %s takes the next argument. The text comes from outside
    # this script, so it is substituted here and never used as a printf format.
    say() {
        say_name=$1; say_text=$2; shift 2
        eval "say_translated=\${NATIVEPIPE_TEXT_$say_name:-}"
        [ -n "$say_translated" ] && say_text=$say_translated
        say_out=
        while :; do
            case "$say_text" in *%s*) ;; *) break ;; esac
            say_out=$say_out${say_text%%"%s"*}${1-}
            say_text=${say_text#*"%s"}
            [ $# -gt 0 ] && shift
        done
        printf '%s\n' "$say_out$say_text" >&2
    }

    mode=${1:?Expected --release URL or --upload}
    case "$mode" in
        --release) base=${2:?Expected a pinned release URL}; base=${base%/} ;;
        --upload) ;;
        *) echo "Unknown installation mode: $mode" >&2; exit 2 ;;
    esac
    arch=$(uname -m)
    case "$arch" in aarch64|x86_64) ;; *) say INSTALL_UNSUPPORTED_ARCH "NativePipe doesn’t support this Linux computer’s architecture (%s). Supported architectures: aarch64 and x86_64." "$arch"; exit 1 ;; esac
    libc=gnu
    if (ldd --version 2>&1 || :) | grep -i musl >/dev/null; then libc=musl; fi
    command -v sha256sum >/dev/null || { say INSTALL_NEED_SHA256SUM "Install sha256sum so NativePipe can verify its download."; exit 1; }
    root="$HOME/.local/share/nativepipe/compositor"
    mkdir -p "$root/releases"
    tmp=$(mktemp -d "$root/.install-XXXXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 1' HUP INT TERM
    asset="nativepipe-compositor-$arch-$libc.tar.gz"

    if [ "$mode" = --upload ]; then
        printf 'NATIVEPIPE TARGET %s %s\n' "$arch" "$libc" >&3
        IFS=' ' read -r digest size
        case "$size" in ''|*[!0-9]*) say INSTALL_INVALID_UPLOAD "The NativePipe files received from this Mac are invalid. Try again."; exit 1 ;; esac
        [ "${#size}" -le 9 ] && [ "$size" -gt 0 ] && [ "$size" -le 536870912 ] || {
            say INSTALL_INVALID_UPLOAD "The NativePipe files received from this Mac are invalid. Try again."; exit 1;
        }
    else
        command -v curl >/dev/null || { say INSTALL_NEED_CURL "Install curl so NativePipe can download its compositor."; exit 1; }
        say INSTALL_CHECKING "Checking NativePipe %s…" "${base##*/}"
        curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
            --connect-timeout 10 --max-time 20 "$base/SHA256SUMS" -o "$tmp/SHA256SUMS"
        digest=$(awk -v asset="$asset" '$2 == asset { print $1 }' "$tmp/SHA256SUMS")
    fi
    case "$digest" in ''|*[!0-9a-fA-F]*) say INSTALL_NO_CHECKSUM "The NativePipe release has no valid checksum for %s." "$asset"; exit 1 ;; esac
    [ "${#digest}" = 64 ] || { say INSTALL_NO_CHECKSUM "The NativePipe release has no valid checksum for %s." "$asset"; exit 1; }
    installed="$root/releases/$digest"

    if [ ! -x "$installed/nativepipe-wayland" ] || [ "$(cat "$installed/.sha256" 2>/dev/null || :)" != "$digest" ]; then
        say INSTALL_INSTALLING "Installing the NativePipe compositor for %s (%s)…" "$arch" "$libc"
        if [ "$mode" = --upload ]; then
            printf 'NATIVEPIPE UPLOAD\n' >&3
            head -c "$size" > "$tmp/$asset"
            [ "$(wc -c < "$tmp/$asset")" -eq "$size" ] || { say INSTALL_INTERRUPTED "The NativePipe upload was interrupted. Try again."; exit 1; }
        else
            curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
                --connect-timeout 10 --max-time 120 "$base/$asset" -o "$tmp/$asset"
        fi
        printf '%s  %s\n' "$digest" "$asset" > "$tmp/checksum"
        (cd "$tmp" && sha256sum -c checksum >&2)
        mkdir "$tmp/unpacked"
        tar -xzf "$tmp/$asset" -C "$tmp/unpacked"
        [ -x "$tmp/unpacked/nativepipe-wayland" ] || { say INSTALL_INCOMPLETE_RELEASE "The NativePipe release doesn’t contain nativepipe-wayland."; exit 1; }
        printf '%s\n' "$digest" > "$tmp/unpacked/.sha256"
        # Never overwrite a directory still used by another connection.
        if ! mv -T "$tmp/unpacked" "$installed" 2>/dev/null; then
            [ -x "$installed/nativepipe-wayland" ] && [ "$(cat "$installed/.sha256" 2>/dev/null || :)" = "$digest" ] || {
                say INSTALL_PUBLISH_FAILED "Couldn’t finish installing the NativePipe compositor. Check the permissions and free space in ~/.local/share/nativepipe."; exit 1;
            }
        fi
    else
        say INSTALL_UP_TO_DATE "The NativePipe compositor is up to date."
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
