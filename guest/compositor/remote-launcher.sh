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
# Child applications share this session's private np-open endpoint, and find
# the matching helper from the same release rather than another installation.
test -x "$root/libexec/np-open" || { echo 'The NativePipe release is missing np-open.' >&2; exit 127; }
PATH="$root/libexec:${PATH:-/usr/bin:/bin}"
export PATH
exec "$root/libexec/nativepipe-wayland" "$@"
