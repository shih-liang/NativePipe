#!/usr/bin/env bash
# Cross-compile a static musl vmpipe-wayland with zig (runs on Alpine and glibc).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIG="${ZIG:-zig}"
TARGET="${1:-aarch64-linux-musl}"
case "${TARGET}" in
  aarch64-linux-musl) OUT_NAME=vmpipe-wayland-aarch64 ;;
  x86_64-linux-musl)  OUT_NAME=vmpipe-wayland-x86_64 ;;
  *) echo "usage: $0 aarch64-linux-musl|x86_64-linux-musl" >&2; exit 1 ;;
esac

DEPS="${ROOT}/.deps/${TARGET}"
SRC="${DEPS}/src"
PREFIX="${DEPS}/prefix"
DIST="${ROOT}/dist"
WRAP="${DEPS}/bin"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

CJSON_VER=1.7.19
FFI_VER=3.4.7
WAYLAND_VER=1.23.1
XKB_VER=1.7.0

mkdir -p "${SRC}" "${PREFIX}" "${WRAP}" "${DIST}"

cat > "${WRAP}/cc" <<EOF
#!/bin/sh
exec ${ZIG} cc -target ${TARGET} -fno-sanitize=undefined "\$@"
EOF
cat > "${WRAP}/ar" <<EOF
#!/bin/sh
exec ${ZIG} ar "\$@"
EOF
cat > "${WRAP}/ranlib" <<EOF
#!/bin/sh
exec ${ZIG} ranlib "\$@"
EOF
cat > "${WRAP}/pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR
exec /opt/homebrew/bin/pkg-config --static "\$@"
EOF
chmod +x "${WRAP}"/cc "${WRAP}"/ar "${WRAP}"/ranlib "${WRAP}"/pkg-config

export CC="${WRAP}/cc"
export AR="${WRAP}/ar"
export RANLIB="${WRAP}/ranlib"
# Do not put WRAP on PATH — the pkg-config wrapper used to recurse on itself.

fetch() {
  local url="$1" dest="$2"
  if [[ -f "${dest}" ]]; then
    return 0
  fi
  echo "==> fetching $(basename "${dest}")"
  curl -fL --progress-bar -o "${dest}.tmp" "${url}"
  mv "${dest}.tmp" "${dest}"
}

extract() {
  local archive="$1" dir="$2"
  if [[ -d "${dir}" ]]; then
    return 0
  fi
  mkdir -p "$(dirname "${dir}")"
  local tmp
  tmp="$(mktemp -d "${SRC}/extract.XXXXXX")"
  case "${archive}" in
    *.tar.xz) tar -xJf "${archive}" -C "${tmp}" ;;
    *.tar.gz) tar -xzf "${archive}" -C "${tmp}" ;;
    *) echo "unknown archive ${archive}" >&2; exit 1 ;;
  esac
  local inner
  inner="$(find "${tmp}" -mindepth 1 -maxdepth 1 -type d | head -1)"
  mv "${inner}" "${dir}"
  rmdir "${tmp}"
}

# --- cJSON (single file) ---
fetch "https://github.com/DaveGamble/cJSON/archive/refs/tags/v${CJSON_VER}.tar.gz" \
  "${SRC}/cJSON-${CJSON_VER}.tar.gz"
extract "${SRC}/cJSON-${CJSON_VER}.tar.gz" "${SRC}/cJSON-${CJSON_VER}"
if [[ ! -f "${PREFIX}/lib/libcjson.a" ]]; then
  echo "==> cJSON ${CJSON_VER}"
  "${CC}" -O2 -c -o "${SRC}/cJSON-${CJSON_VER}/cJSON.o" \
    "${SRC}/cJSON-${CJSON_VER}/cJSON.c" -DcJSON_API=
  mkdir -p "${PREFIX}/lib/pkgconfig" "${PREFIX}/include/cjson"
  "${AR}" rcs "${PREFIX}/lib/libcjson.a" "${SRC}/cJSON-${CJSON_VER}/cJSON.o"
  cp "${SRC}/cJSON-${CJSON_VER}/cJSON.h" "${PREFIX}/include/cjson/cJSON.h"
  cat > "${PREFIX}/lib/pkgconfig/libcjson.pc" <<EOF
prefix=${PREFIX}
includedir=\${prefix}/include
libdir=\${prefix}/lib
Name: libcjson
Version: ${CJSON_VER}
Cflags: -I\${includedir}
Libs: -L\${libdir} -lcjson
EOF
fi

# --- libffi ---
fetch "https://github.com/libffi/libffi/releases/download/v${FFI_VER}/libffi-${FFI_VER}.tar.gz" \
  "${SRC}/libffi-${FFI_VER}.tar.gz"
extract "${SRC}/libffi-${FFI_VER}.tar.gz" "${SRC}/libffi-${FFI_VER}"
if [[ ! -f "${PREFIX}/lib/libffi.a" ]]; then
  echo "==> libffi ${FFI_VER}"
  host="${TARGET%-musl}"
  # zig's aarch64-linux-musl ≈ aarch64-unknown-linux-musl for autoconf
  case "${TARGET}" in
    aarch64-linux-musl) host=aarch64-unknown-linux-musl ;;
    x86_64-linux-musl)  host=x86_64-unknown-linux-musl ;;
  esac
  pushd "${SRC}/libffi-${FFI_VER}" >/dev/null
  ./configure --host="${host}" --prefix="${PREFIX}" \
    --enable-static --disable-shared --disable-docs \
    CC="${CC}" AR="${AR}" RANLIB="${RANLIB}"
  make -j"${JOBS}"
  make install
  popd >/dev/null
fi

# --- wayland-server (no meson: host scanner + zig for linux objects) ---
fetch "https://gitlab.freedesktop.org/wayland/wayland/-/releases/${WAYLAND_VER}/downloads/wayland-${WAYLAND_VER}.tar.xz" \
  "${SRC}/wayland-${WAYLAND_VER}.tar.xz"
extract "${SRC}/wayland-${WAYLAND_VER}.tar.xz" "${SRC}/wayland-${WAYLAND_VER}"
WL="${SRC}/wayland-${WAYLAND_VER}"
WLB="${WL}/np-build"
if [[ ! -f "${PREFIX}/lib/libwayland-server.a" ]]; then
  echo "==> wayland ${WAYLAND_VER}"
  mkdir -p "${WLB}"
  sed -e "s/@WAYLAND_VERSION_MAJOR@/1/" -e "s/@WAYLAND_VERSION_MINOR@/23/" \
      -e "s/@WAYLAND_VERSION_MICRO@/1/" -e "s/@WAYLAND_VERSION@/${WAYLAND_VER}/" \
      "${WL}/src/wayland-version.h.in" > "${WLB}/wayland-version.h"
  cat > "${WLB}/config.h" <<'EOF'
#define PACKAGE "wayland"
#define PACKAGE_VERSION "1.23.1"
#define HAVE_SYS_PRCTL_H 1
#define HAVE_ACCEPT4 1
#define HAVE_MKOSTEMP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_PRCTL 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_MREMAP 1
#define HAVE_STRNDUP 1
#define HAVE_BROKEN_MSG_CMSG_CLOEXEC 0
#define HAVE_XUCRED_CR_PID 0
EOF
  # wayland-os.c includes "../config.h" relative to src/.
  cp "${WLB}/config.h" "${WL}/config.h"
  # Native scanner (macOS). DTD validation off — no libxml.
  /usr/bin/cc -O2 -o "${WLB}/wayland-scanner" \
    -I"${WLB}" -I"${WL}/src" \
    "${WL}/src/scanner.c" "${WL}/src/wayland-util.c" \
    -lexpat

  "${WLB}/wayland-scanner" server-header \
    "${WL}/protocol/wayland.xml" "${WLB}/wayland-server-protocol.h"
  "${WLB}/wayland-scanner" server-header -c \
    "${WL}/protocol/wayland.xml" "${WLB}/wayland-server-protocol-core.h"
  "${WLB}/wayland-scanner" public-code \
    "${WL}/protocol/wayland.xml" "${WLB}/wayland-protocol.c"

  WL_CFLAGS="-O2 -std=c99 -D_POSIX_C_SOURCE=200809L -DHAVE_CONFIG_H -fvisibility=hidden"
  WL_INC="-I${WLB} -I${WL}/src -I${PREFIX}/include"
  for f in wayland-util.c connection.c wayland-os.c wayland-server.c wayland-shm.c event-loop.c; do
    "${CC}" ${WL_CFLAGS} ${WL_INC} -c -o "${WLB}/${f%.c}.o" "${WL}/src/${f}"
  done
  "${CC}" ${WL_CFLAGS} ${WL_INC} -c -o "${WLB}/wayland-protocol.o" "${WLB}/wayland-protocol.c"
  "${AR}" rcs "${PREFIX}/lib/libwayland-server.a" \
    "${WLB}"/wayland-util.o "${WLB}"/connection.o "${WLB}"/wayland-os.o \
    "${WLB}"/wayland-server.o "${WLB}"/wayland-shm.o "${WLB}"/event-loop.o \
    "${WLB}"/wayland-protocol.o
  mkdir -p "${PREFIX}/include"
  cp "${WL}/src/wayland-server.h" "${WL}/src/wayland-server-core.h" \
     "${WL}/src/wayland-util.h" "${WLB}/wayland-version.h" \
     "${WLB}/wayland-server-protocol.h" "${WLB}/wayland-server-protocol-core.h" \
     "${PREFIX}/include/"
  cat > "${PREFIX}/lib/pkgconfig/wayland-server.pc" <<EOF
prefix=${PREFIX}
includedir=\${prefix}/include
libdir=\${prefix}/lib
Name: Wayland Server
Version: ${WAYLAND_VER}
Cflags: -I\${includedir}
Libs: -L\${libdir} -lwayland-server -lffi -lpthread -lrt
EOF
fi

# --- libxkbcommon (keyboard only; xkb data lives on the guest) ---
fetch "https://xkbcommon.org/download/libxkbcommon-${XKB_VER}.tar.xz" \
  "${SRC}/libxkbcommon-${XKB_VER}.tar.xz"
extract "${SRC}/libxkbcommon-${XKB_VER}.tar.xz" "${SRC}/libxkbcommon-${XKB_VER}"
XKB="${SRC}/libxkbcommon-${XKB_VER}"
XKB_B="${XKB}/np-build"
if [[ ! -f "${PREFIX}/lib/libxkbcommon.a" ]]; then
  echo "==> libxkbcommon ${XKB_VER}"
  mkdir -p "${XKB_B}"
  cat > "${XKB_B}/config.h" <<'EOF'
#pragma once
#define _GNU_SOURCE 1
#define DEFAULT_XKB_RULES "evdev"
#define DEFAULT_XKB_MODEL "pc105"
#define DEFAULT_XKB_LAYOUT "us"
#define DEFAULT_XKB_VARIANT NULL
#define DEFAULT_XKB_OPTIONS NULL
#define DFLT_XKB_CONFIG_ROOT "/usr/share/X11/xkb"
#define DFLT_XKB_CONFIG_EXTRA_PATH "/etc/xkb"
#define XLOCALEDIR "/usr/share/X11/locale"
#define EXIT_INVALID_USAGE 2
#define LIBXKBCOMMON_VERSION "1.7.0"
#define HAVE_UNISTD_H 1
#define HAVE___BUILTIN_EXPECT 1
#define HAVE_EACCESS 1
#define HAVE_MMAP 1
#define HAVE_MKOSTEMP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_STRNDUP 1
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
#define HAVE_SECURE_GETENV 1
EOF
  BISON="${BISON:-}"
  if [[ -z "${BISON}" ]]; then
    if [[ -x /opt/homebrew/opt/bison/bin/bison ]]; then
      BISON=/opt/homebrew/opt/bison/bin/bison
    else
      BISON="$(command -v bison)"
    fi
  fi
  # meson always regenerates this; the release tarball has parser.y only.
  "${BISON}" -d -p _xkbcommon_ -o "${XKB_B}/parser.c" "${XKB}/src/xkbcomp/parser.y"
  XKB_CFLAGS="-O2 -std=c99 -DHAVE_CONFIG_H -D_GNU_SOURCE"
  XKB_INC="-I${XKB_B} -I${XKB}/src -I${XKB}/src/xkbcomp -I${XKB}/src/compose -I${XKB}/include"
  objs=()
  for f in \
    src/compose/parser.c src/compose/paths.c src/compose/state.c src/compose/table.c \
    src/xkbcomp/action.c src/xkbcomp/ast-build.c src/xkbcomp/compat.c src/xkbcomp/expr.c \
    src/xkbcomp/include.c src/xkbcomp/keycodes.c src/xkbcomp/keymap.c src/xkbcomp/keymap-dump.c \
    src/xkbcomp/keywords.c src/xkbcomp/rules.c src/xkbcomp/scanner.c src/xkbcomp/symbols.c \
    src/xkbcomp/types.c src/xkbcomp/vmod.c src/xkbcomp/xkbcomp.c \
    src/atom.c src/context.c src/context-priv.c src/keysym.c src/keysym-utf.c \
    src/keymap.c src/keymap-priv.c src/state.c src/text.c src/utf8.c src/utils.c
  do
    obj="${XKB_B}/$(echo "${f}" | tr '/-' '__')"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "${obj}")"
    "${CC}" ${XKB_CFLAGS} ${XKB_INC} -c -o "${obj}" "${XKB}/${f}"
    objs+=("${obj}")
  done
  "${CC}" ${XKB_CFLAGS} ${XKB_INC} -c -o "${XKB_B}/parser.o" "${XKB_B}/parser.c"
  objs+=("${XKB_B}/parser.o")
  "${AR}" rcs "${PREFIX}/lib/libxkbcommon.a" "${objs[@]}"
  mkdir -p "${PREFIX}/include/xkbcommon"
  cp "${XKB}/include/xkbcommon/"*.h "${PREFIX}/include/xkbcommon/"
  cat > "${PREFIX}/lib/pkgconfig/xkbcommon.pc" <<EOF
prefix=${PREFIX}
includedir=\${prefix}/include
libdir=\${prefix}/lib
Name: xkbcommon
Version: ${XKB_VER}
Cflags: -I\${includedir}
Libs: -L\${libdir} -lxkbcommon
EOF
fi

echo "==> linking ${OUT_NAME}"
# Protocol files are already generated in this directory.
(
  cd "${ROOT}"
  "${CC}" -O2 -static -s -std=gnu11 -Wall \
    -I"${PREFIX}/include" \
    -I"${ROOT}" \
    -o "${DIST}/${OUT_NAME}" \
    compositor.c hostlink.c vmpipe_main.c blob.c dmabuf.c \
    xdg-shell-protocol.c fractional-scale-v1-protocol.c \
    xdg-decoration-protocol.c text-input-v3-protocol.c \
    linux-dmabuf-unstable-v1-protocol.c \
    -L"${PREFIX}/lib" \
    -lwayland-server -lxkbcommon -lcjson -lffi \
    -lpthread -lrt -lm
)
file "${DIST}/${OUT_NAME}"
echo "built ${DIST}/${OUT_NAME}"
