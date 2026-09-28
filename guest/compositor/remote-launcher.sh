#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Private ELF RUNPATHs locate the bundled libraries without changing the
# environment inherited by Linux apps, Xwayland or the private D-Bus daemon.
if ! LD_BIND_NOW=1 "$root/libexec/nativepipe-wayland" --check-runtime; then
    echo 'NativePipe runtime check failed (see the specific error above).' >&2
    echo 'Required: GLib/GIO and graphics libraries, dbus-run-session, and XKB keyboard data. Install missing packages with your distribution package manager. AV1 software encoding is bundled.' >&2
    exit 127
fi
exec "$root/libexec/nativepipe-wayland" "$@"
