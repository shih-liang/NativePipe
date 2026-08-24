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

if [ -r /opt/nativepipe/venus/virtio_icd.json ]; then
	ICD=/opt/nativepipe/venus/virtio_icd.json
else
	ICD=$(ls /usr/share/vulkan/icd.d/virtio_icd*.json 2>/dev/null | head -1 || true)
fi
if [ -n "$ICD" ]; then
	export VK_DRIVER_FILES="$ICD"
	export VK_ICD_FILENAMES="$ICD"
fi

# A 4 KiB guest needs blob sizes rounded to the 16 KiB host page. A native
# 16 KiB LightHouse kernel already provides that invariant and needs no shim.
# Copy off the virtiofs share: some guests refuse PROT_EXEC mmap there.
ALIGN_SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/align-host-blob.so
ALIGN_INSTALLED=/usr/libexec/nativepipe/nativepipe-align-host-blob.so
GUEST_PAGE_SIZE=$(getconf PAGESIZE 2>/dev/null || getconf PAGE_SIZE 2>/dev/null || true)
ALIGN_SO=
if [ -z "$GUEST_PAGE_SIZE" ] || [ "$GUEST_PAGE_SIZE" -lt 16384 ]; then
    if [ -r "$ALIGN_INSTALLED" ]; then
        ALIGN_SO=$ALIGN_INSTALLED
    elif [ -f "$ALIGN_SRC" ]; then
        ALIGN_SO=$XDG_RUNTIME_DIR/nativepipe-align-host-blob.so
        cp "$ALIGN_SRC" "$ALIGN_SO"
    else
        ALIGN_SO=$ALIGN_SRC
    fi
    if [ -r "$ALIGN_SO" ]; then
        export LD_PRELOAD="$ALIGN_SO${LD_PRELOAD:+:$LD_PRELOAD}"
    fi
fi

{
	echo "ICD=$ICD"
	echo "LD_PRELOAD=${LD_PRELOAD:-}"
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
	VKINFO_COPY=$XDG_RUNTIME_DIR/nativepipe-vulkaninfo
	cp "$VKINFO" "$VKINFO_COPY"
	chmod 755 "$VKINFO_COPY"
} > "$LOG_DIR/vulkaninfo-setup.log"

"$VKINFO_COPY" > "$LOG_DIR/vulkaninfo.log" 2>&1 || {
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
