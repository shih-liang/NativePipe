#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Private ELF RUNPATHs locate the bundled libraries without changing the
# environment inherited by Linux apps, Xwayland or the private D-Bus daemon.
if ! LD_BIND_NOW=1 "$root/libexec/nativepipe-wayland" --check-runtime; then
    echo 'NativePipe cannot load its Linux runtime libraries (see the loader error above).' >&2
    echo 'Install the matching system FFmpeg, GLib/GIO and graphics libraries with your distribution package manager.' >&2
    echo 'FFmpeg library major versions must match this build; if unavailable, build NativePipe against this system. Do not symlink different library major versions.' >&2
    exit 127
fi
exec "$root/libexec/nativepipe-wayland" "$@"
