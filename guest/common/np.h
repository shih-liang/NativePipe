/*
 * Shared NativePipe guest-runtime helpers: filesystem access, vsock I/O and
 * NPIP session messages. Guest installation and file transfer live in LinuxKit.
 */
#ifndef NATIVEPIPE_NP_H
#define NATIVEPIPE_NP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NP_CID_HOST 2u
#define NP_CID_ANY 0xFFFFFFFFu
#define NP_PORT_CONTROL 1024u
#define NP_PORT_AGENT 1029u
#define NP_PORT_HOST_OPEN 1030u
#define NP_PORT_SESSION_FIRST 2048u
#define NP_PORT_SESSION_LAST 2303u

#define NP_NPIP_MAGIC "NPIP"
#define NP_WIRE_VERSION 1

#define NP_GUESTD_NAME "nativepipe-guestd"
#define NP_INSTALLED_BIN "/usr/libexec/nativepipe/nativepipe-guestd"
#define NP_INSTALLED_VERSION "/usr/libexec/nativepipe/VERSION"
#define NP_OPEN_NAME "nativepipe-open"
#define NP_INSTALLED_OPEN "/usr/local/bin/np-open"
#define NP_SESSION_NAME "nativepipe-session"
#define NP_INSTALLED_SESSION "/usr/libexec/nativepipe/nativepipe-session"
#define NP_COMPOSITOR_MUSL_NAME "vmpipe-wayland-musl"
#define NP_COMPOSITOR_GNU_NAME "vmpipe-wayland-gnu"
#define NP_INSTALLED_COMPOSITOR "/usr/libexec/nativepipe/vmpipe-wayland"
#define NP_ALIGN_BLOB_MUSL_NAME "nativepipe-align-blob-musl.so"
#define NP_ALIGN_BLOB_GNU_NAME "nativepipe-align-blob-gnu.so"
#define NP_INSTALLED_ALIGN_BLOB "/usr/libexec/nativepipe/nativepipe-align-host-blob.so"
#define NP_VULKAN_LAYER_MUSL_NAME "nativepipe-vulkan-layer-musl.so"
#define NP_VULKAN_LAYER_GNU_NAME "nativepipe-vulkan-layer-gnu.so"
#define NP_INSTALLED_VULKAN_LAYER "/usr/libexec/nativepipe/nativepipe-vulkan-blob-alignment.so"
#define NP_VULKAN_LAYER_MANIFEST_NAME "implicit_layer/VkLayer_NATIVEPIPE_blob_alignment.json"
#define NP_INSTALLED_VULKAN_LAYER_MANIFEST "/etc/vulkan/implicit_layer.d/VkLayer_NATIVEPIPE_blob_alignment.json"
#define NP_SESSION_PROFILE_NAME "profile.d/nativepipe.sh"
#define NP_INSTALLED_SESSION_PROFILE "/etc/profile.d/nativepipe.sh"
#define NP_SESSION_USER_FILE "/var/lib/nativepipe/session-user"
#define NP_DESKTOP_PREFERENCES_FILE "/var/lib/nativepipe/desktop-preferences"
#define NP_MAX_VERSION 256
#define NP_MAX_NPIP_PAYLOAD (8u * 1024u * 1024u)

int np_read_full(int fd, void *buf, size_t n);
int np_write_full(int fd, const void *buf, size_t n);
int np_path_exists(const char *path);
/* True when the distribution installed a Mesa Venus ICD. LinPortal never
 * supplies or selects a second Mesa implementation inside the guest. */
int np_venus_icd_available(void);
int np_mkdir_p(const char *path);
int np_write_file(const char *path, const void *data, size_t n, int mode);
int np_write_version(const char *ver);
int np_read_version(char *out, size_t cap);
int np_copy_file(const char *src, const char *dst, int mode);
int np_run(char *const argv[]);

/* Connect to host CID 2. retries is attempts with 1s sleep; 0 means once. */
int np_vsock_connect_host(uint32_t port, int retries);
int np_vsock_listen(uint32_t port, int backlog);

int np_npip_send(int fd, const void *payload, size_t payload_len);
/* Returns payload length, or -1. Caller frees *out. */
ssize_t np_npip_recv(int fd, uint8_t **out);

#endif
