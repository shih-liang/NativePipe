/// Small SSH bootstraps. Installation policy lives in install-compositor.sh,
/// published with the selected release or supplied by FluxWindow offline.
enum RemoteCompositorInstaller {
    static let script = #"""
    download_status=$(mktemp)
    trap 'rm -f "$download_status"' EXIT
    trap 'exit 1' HUP INT TERM
    compositor=$(
      { curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
          --connect-timeout 10 --max-time 30 "${release:?NativePipe release URL was not resolved.}/install-compositor.sh" &&
        printf complete > "$download_status"; } | sh -s -- --release "$release"
    )
    [ -s "$download_status" ] || { echo 'NativePipe installer download did not complete.' >&2; exit 1; }
    [ -n "$compositor" ] && [ -x "$compositor" ] || { echo 'NativePipe installer did not return an executable.' >&2; exit 1; }
    rm -f "$download_status"
    trap - EXIT HUP INT TERM
    """#

    /// Receive the bundled script as a bounded file before its archive/protocol
    /// exchange. The script never travels inside the SSH command argument.
    static let uploadScript = #"""
    installer=$(mktemp)
    trap 'rm -f "$installer"' EXIT
    trap 'exit 1' HUP INT TERM
    printf 'NATIVEPIPE INSTALLER\n'
    IFS= read -r size
    case "$size" in ''|*[!0-9]*) echo 'Invalid NativePipe installer size.' >&2; exit 1;; esac
    [ "${#size}" -le 5 ] && [ "$size" -gt 0 ] && [ "$size" -le 65536 ] || {
      echo 'NativePipe installer exceeds the size limit.' >&2; exit 1;
    }
    head -c "$size" > "$installer"
    [ "$(wc -c < "$installer")" -eq "$size" ] || { echo 'NativePipe installer upload was interrupted.' >&2; exit 1; }
    exec 3>&1
    compositor=$(sh "$installer" --upload)
    exec 3>&-
    [ -n "$compositor" ] && [ -x "$compositor" ] || { echo 'NativePipe installer did not return an executable.' >&2; exit 1; }
    rm -f "$installer"
    trap - EXIT HUP INT TERM
    """#
}
