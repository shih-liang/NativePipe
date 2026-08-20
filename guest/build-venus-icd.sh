#!/bin/sh
# Build the NativePipe-compatible Mesa Venus ICD without replacing distro Mesa.
# This is a one-time development/setup helper; production images should consume
# a prebuilt, profile-matched artifact through guestd's resource channel.
set -eu

MESA_VERSION=26.1.6
MESA_SHA256=5296b88a0f1e012e2cb9ada150a2bbadf728ca81e5a4fb2ab43c83a4d2158606
PREFIX="/opt/nativepipe/mesa-${MESA_VERSION}"
STABLE_DIR=/opt/nativepipe/venus
STABLE_ICD="${STABLE_DIR}/virtio_icd.json"
ARCH=$(apk --print-arch)
VERSIONED_ICD="${PREFIX}/share/vulkan/icd.d/virtio_icd.${ARCH}.json"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PATCH="${SCRIPT_DIR}/../../../scripts/patches/mesa-venus-blob-alignment.patch"

if [ -s "${PREFIX}/lib/libvulkan_virtio.so" ] && [ -s "${VERSIONED_ICD}" ]; then
	mkdir -p "$STABLE_DIR"
	ln -sfn "$VERSIONED_ICD" "$STABLE_ICD"
	echo "$STABLE_ICD"
	exit 0
fi

if [ ! -r "$PATCH" ]; then
	echo "NativePipe Mesa patch is missing: $PATCH" >&2
	exit 1
fi

WORK=$(mktemp -d /tmp/nativepipe-mesa.XXXXXX)
BUILD_DEPS=.nativepipe-mesa-build
cleanup()
{
	case "$WORK" in
		/tmp/nativepipe-mesa.*) rm -rf -- "$WORK" ;;
	esac
	apk del --no-progress "$BUILD_DEPS" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

# Some development images inherited qt6-qtwayland as an unpinned dependency.
# Pin an already-present runtime before deleting the temporary build group so
# an unrelated graphics setup can never disappear during Venus setup.
if apk info -e qt6-qtwayland >/dev/null 2>&1; then
	apk add --no-progress qt6-qtwayland >/dev/null
fi

apk add --no-progress --virtual "$BUILD_DEPS" \
	build-base meson pkgconf python3 py3-mako py3-yaml py3-packaging \
	py3-ply bison flex libdrm-dev wayland-dev wayland-protocols \
	vulkan-loader-dev zlib-dev

ARCHIVE="${WORK}/mesa-${MESA_VERSION}.tar.xz"
wget -q -O "$ARCHIVE" \
	"https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz"
echo "${MESA_SHA256}  ${ARCHIVE}" | sha256sum -c -

SOURCE="${WORK}/source"
mkdir "$SOURCE"
tar -xf "$ARCHIVE" -C "$SOURCE" --strip-components=1
patch -d "$SOURCE" -p1 < "$PATCH"

meson setup "${SOURCE}/build" "$SOURCE" \
	--prefix="$PREFIX" --libdir=lib \
	-Dplatforms=wayland \
	-Dgallium-drivers= \
	-Dvulkan-drivers=virtio \
	-Dvulkan-layers= \
	-Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dopengl=false -Dgles1=disabled -Dgles2=disabled \
	-Dllvm=disabled -Dbuild-tests=false -Dshader-cache=disabled \
	-Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled
meson compile -C "${SOURCE}/build"
meson install -C "${SOURCE}/build"

test -s "${PREFIX}/lib/libvulkan_virtio.so"
test -s "$VERSIONED_ICD"
mkdir -p "$STABLE_DIR"
ln -sfn "$VERSIONED_ICD" "$STABLE_ICD"
echo "$STABLE_ICD"
