#!/bin/sh
# Guest-side Venus client. Not cairo. Not GTK.
#
# Installs Mesa's virtio Venus ICD if needed, dumps vulkaninfo, then starts
# vkcube on Wayland so present is a linux-dmabuf attach.
set -eu

LOG_DIR=${1:-/tmp}
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/np-runtime}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"

if [ ! -f /usr/share/vulkan/icd.d/virtio_icd.json ] && \
   [ ! -f /usr/share/vulkan/icd.d/virtio_icd.aarch64.json ]; then
	if [ "${NATIVEPIPE_SETUP_VENUS:-0}" != 1 ]; then
		echo "Venus packages are missing; run 'make setup-3d-test' once" \
			> "$LOG_DIR/vulkaninfo.log"
		exit 1
	fi
	if ! grep -q '/community' /etc/apk/repositories; then
		sed -i -e 's|^#\(.*/community\)|\1|' /etc/apk/repositories || true
		if ! grep -q '/community' /etc/apk/repositories; then
			branch=$(. /etc/os-release; echo "${VERSION_ID%.*}")
			echo "https://dl-cdn.alpinelinux.org/alpine/v${branch}/community" \
				>> /etc/apk/repositories
		fi
	fi
	apk add --no-progress mesa-vulkan-virtio vulkan-loader vulkan-tools \
		> "$LOG_DIR/apk-venus.log" 2>&1 || {
		echo "apk add mesa-vulkan-virtio vulkan-tools failed" > "$LOG_DIR/vulkaninfo.log"
		cat "$LOG_DIR/apk-venus.log" >> "$LOG_DIR/vulkaninfo.log"
		exit 1
	}
fi

ICD=$(ls /usr/share/vulkan/icd.d/virtio_icd*.json 2>/dev/null | head -1 || true)
if [ -n "$ICD" ]; then
	export VK_DRIVER_FILES="$ICD"
	export VK_ICD_FILENAMES="$ICD"
fi

# Round Mesa Venus blob sizes to the host page. See align-host-blob.c.
# Copy off the virtiofs share: some guests refuse PROT_EXEC mmap there.
ALIGN_SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/align-host-blob.so
ALIGN_SO=/tmp/align-host-blob.so
if [ -f "$ALIGN_SRC" ]; then
	cp "$ALIGN_SRC" "$ALIGN_SO"
	export LD_PRELOAD="$ALIGN_SO${LD_PRELOAD:+:$LD_PRELOAD}"
fi

{
	echo "ICD=$ICD"
	echo "LD_PRELOAD=$LD_PRELOAD"
	echo "align so: $ALIGN_SO"
	ls -l "$ALIGN_SO" 2>&1 || true
	echo "--- preload smoke ---"
	/bin/true 2>&1 || true
	echo "--- vulkaninfo binary ---"
	command -v vulkaninfo
	ls -l "$(command -v vulkaninfo)" 2>&1 || true
	ldd "$(command -v vulkaninfo)" 2>&1 || true
	echo "render nodes:"; ls -l /dev/dri 2>&1 || true
	echo "--- vulkaninfo ---"
	# A setgid vulkaninfo would run AT_SECURE and ignore LD_PRELOAD.
	VKINFO=$(command -v vulkaninfo)
	cp "$VKINFO" /tmp/vulkaninfo
	chmod 755 /tmp/vulkaninfo
} > "$LOG_DIR/vulkaninfo-setup.log"

/tmp/vulkaninfo > "$LOG_DIR/vulkaninfo.log" 2>&1 || {
	echo "vulkaninfo failed: $?" >> "$LOG_DIR/vulkaninfo.log"
}

# Default client is stock vulkan-tools vkcube. Set NATIVEPIPE_VENUS_CLIENT=vkpresent
# if musl libwayland still SIGSEGVs inside Mesa's xdg configure + roundtrip path.
export MESA_DEBUG="${MESA_DEBUG:-1}"
export MESA_VK_WSI_DEBUG="${MESA_VK_WSI_DEBUG:-binds}"
unset WAYLAND_DEBUG
CLIENT="${NATIVEPIPE_VENUS_CLIENT:-vkcube}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/compositor

if [ "$CLIENT" = vkpresent ]; then
	if ! pkg-config --exists vulkan 2>/dev/null; then
		if [ "${NATIVEPIPE_SETUP_VENUS:-0}" = 1 ]; then
			apk add --no-progress vulkan-headers vulkan-loader-dev \
				>> "$LOG_DIR/apk-vkpresent.log" 2>&1 || true
		else
			echo "Vulkan headers are missing; run 'make setup-3d-test' once" \
				>> "$LOG_DIR/vkcube.log"
			exit 1
		fi
	fi
	if ! make -C "$HERE" vkpresent >> "$LOG_DIR/vkpresent-build.log" 2>&1; then
		echo "vkpresent build failed" >> "$LOG_DIR/vkcube.log"
		cat "$LOG_DIR/vkpresent-build.log" >> "$LOG_DIR/vkcube.log"
		exit 1
	fi
	nohup "$HERE/vkpresent" > "$LOG_DIR/vkcube.log" 2>&1 &
	echo "vkpresent pid $!" >> "$LOG_DIR/vulkaninfo.log"
else
	if ! command -v vkcube >/dev/null 2>&1; then
		if [ "${NATIVEPIPE_SETUP_VENUS:-0}" = 1 ]; then
			apk add --no-progress vulkan-tools >> "$LOG_DIR/apk-vkcube.log" 2>&1 || true
		fi
	fi
	if ! command -v vkcube >/dev/null 2>&1; then
		echo "vkcube not found" >> "$LOG_DIR/vkcube.log"
		exit 1
	fi
	nohup vkcube --wsi wayland > "$LOG_DIR/vkcube.log" 2>&1 &
	echo "vkcube pid $!" >> "$LOG_DIR/vulkaninfo.log"
fi
