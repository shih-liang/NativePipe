import Foundation
import NativePipeStrings

/// Text printed on the Linux side -- by the installer and by the compositor --
/// in the language of the person at this Mac.
///
/// Neither process can translate for itself: the Linux account's locale need
/// not be this Mac's, and neither ships a message catalog. So the remote script
/// exports every message, already translated, as NATIVEPIPE_TEXT_<NAME>. Each
/// reader keeps its own English and falls back to it when a variable is absent
/// -- an older client, LinPortal, or a manual run.
///
/// The readers substitute "%s" themselves and never use the text as a printf
/// format: it arrives through the environment, not from their own source.
enum RemoteText {
    /// Name as the readers look it up, and the message in the user's language.
    /// The English for each must match the reader's own fallback; a test keeps
    /// the two in step.
    static var messages: [(name: String, text: String)] { [
        // nativepipe-wayland
        ("STARTUP_FAILED", NPText("The NativePipe compositor couldn’t start (%@).")),
        ("X11_FAILED", NPText("X11 applications won’t open: xwayland-satellite couldn’t start (%@).")),
        ("X11_NOT_INSTALLED", NPText("X11 applications won’t open because xwayland-satellite isn’t installed. Install xwayland-satellite and Xwayland on the Linux computer.")),
        ("CANNOT_LAUNCH", NPText("Couldn’t start %@: %@.")),
        ("DBUS_REQUIRED", NPText("dbus-run-session is required to run applications in their own session. Install dbus-daemon (Debian/Ubuntu) or dbus (Arch/Alpine), and check PATH.")),
        ("KEYMAP_MISSING", NPText("Couldn’t load the %@ keyboard layout. Install xkb-data (Debian/Ubuntu) or xkeyboard-config (Arch/Alpine), and check XKB_CONFIG_ROOT.")),
        ("TRANSFER_TOO_LARGE", NPText("The copied or dragged content is larger than %@ bytes, so it wasn’t transferred.")),
        ("FORCE_QUIT_FAILED", NPText("Couldn’t force quit the application: %@.")),
        ("RENDER_NODE_FAILED", NPText("Couldn’t open the render node %@ set in REMOTEPIPE_RENDER_NODE: %@.")),
        // install-compositor.sh and the bootstraps that run it
        ("INSTALL_UNSUPPORTED_ARCH", NPText("NativePipe doesn’t support this Linux computer’s architecture (%@). Supported architectures: aarch64 and x86_64.")),
        ("INSTALL_NEED_SHA256SUM", NPText("Install sha256sum so NativePipe can verify its download.")),
        ("INSTALL_NEED_CURL", NPText("Install curl so NativePipe can download its compositor.")),
        ("INSTALL_CHECKING", NPText("Checking NativePipe %@…")),
        ("INSTALL_NO_CHECKSUM", NPText("The NativePipe release has no valid checksum for %@.")),
        ("INSTALL_INSTALLING", NPText("Installing the NativePipe compositor for %@ (%@)…")),
        ("INSTALL_INTERRUPTED", NPText("The NativePipe upload was interrupted. Try again.")),
        ("INSTALL_INCOMPLETE_RELEASE", NPText("The NativePipe release doesn’t contain nativepipe-wayland.")),
        ("INSTALL_INCOMPLETE_OPEN_RELEASE", NPText("The NativePipe release doesn’t contain np-open.")),
        ("INSTALL_PUBLISH_FAILED", NPText("Couldn’t finish installing the NativePipe compositor. Check the permissions and free space in ~/.local/share/nativepipe.")),
        ("INSTALL_UP_TO_DATE", NPText("The NativePipe compositor is up to date.")),
        ("INSTALL_INVALID_UPLOAD", NPText("The NativePipe files received from this Mac are invalid. Try again.")),
        ("INSTALL_DOWNLOAD_INCOMPLETE", NPText("The NativePipe installer didn’t download completely. Check the Linux computer’s internet connection, then try again.")),
        ("INSTALL_FAILED", NPText("The NativePipe compositor couldn’t be installed.")),
    ] }

    /// The localized text in the readers' notation: "%@" becomes "%s", and a
    /// positional "%2$@" becomes "%2$s". The shell reader has no positional
    /// form, so a translation that reorders its arguments is not exported for
    /// the installer at all -- English in the right order beats a translation
    /// with its arguments swapped.
    static func remoteNotation(name: String, text: String) -> String? {
        let converted = text
            .replacingOccurrences(of: #"%([12])\$@"#, with: "%$1\\$s", options: .regularExpression)
            .replacingOccurrences(of: "%@", with: "%s")
        if name.hasPrefix("INSTALL_") && converted.contains("$s") { return nil }
        return converted
    }

    /// Shell lines exporting every message, for the top of the remote script.
    static var exports: String {
        messages.compactMap { name, text in
            remoteNotation(name: name, text: text).map {
                "export NATIVEPIPE_TEXT_\(name)=\(SSHCommand.quote($0))"
            }
        }.joined(separator: "\n")
    }
}
