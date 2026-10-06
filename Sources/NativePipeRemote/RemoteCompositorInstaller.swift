/// Small SSH bootstraps. Installation policy lives in install-compositor.sh,
/// published with the selected release or supplied by LinPortal offline.
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
    [ -s "$download_status" ] || { printf '%s\n' "${NATIVEPIPE_TEXT_INSTALL_DOWNLOAD_INCOMPLETE:-The NativePipe installer didn’t download completely. Check the Linux computer’s internet connection, then try again.}" >&2; exit 1; }
    [ -n "$compositor" ] && [ -x "$compositor" ] || { printf '%s\n' "${NATIVEPIPE_TEXT_INSTALL_FAILED:-The NativePipe compositor couldn’t be installed.}" >&2; exit 1; }
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
    case "$size" in ''|*[!0-9]*) printf '%s\n' "${NATIVEPIPE_TEXT_INSTALL_INVALID_UPLOAD:-The NativePipe files received from this Mac are invalid. Try again.}" >&2; exit 1;; esac
    [ "${#size}" -le 5 ] && [ "$size" -gt 0 ] && [ "$size" -le 65536 ] || {
      printf '%s\n' "${NATIVEPIPE_TEXT_INSTALL_INVALID_UPLOAD:-The NativePipe files received from this Mac are invalid. Try again.}" >&2; exit 1;
    }
    head -c "$size" > "$installer"
    [ "$(wc -c < "$installer")" -eq "$size" ] || { printf '%s\n' "${NATIVEPIPE_TEXT_INSTALL_INTERRUPTED:-The NativePipe upload was interrupted. Try again.}" >&2; exit 1; }
    exec 3>&1
    compositor=$(sh "$installer" --upload)
    exec 3>&-
    [ -n "$compositor" ] && [ -x "$compositor" ] || { printf '%s\n' "${NATIVEPIPE_TEXT_INSTALL_FAILED:-The NativePipe compositor couldn’t be installed.}" >&2; exit 1; }
    rm -f "$installer"
    trap - EXIT HUP INT TERM
    """#
}
