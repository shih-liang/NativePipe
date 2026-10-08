/*
 * Shared NativePipe guest-runtime helpers: filesystem access and vsock I/O.
 * Guest installation and file transfer live in LinuxKit.
 */
#ifndef NATIVEPIPE_NP_H
#define NATIVEPIPE_NP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NP_CID_HOST 2u
#define NP_PORT_HOST_OPEN 1030u

#define NP_INSTALLED_SESSION "/usr/libexec/nativepipe/nativepipe-session"
#define NP_INSTALLED_COMPOSITOR "/usr/libexec/nativepipe/vmpipe-wayland"
#define NP_INSTALLED_ALIGN_BLOB "/usr/libexec/nativepipe/nativepipe-align-host-blob.so"
#define NP_INSTALLED_VULKAN_LAYER "/usr/libexec/nativepipe/nativepipe-vulkan-blob-alignment.so"
#define NP_INSTALLED_VULKAN_LAYER_MANIFEST "/etc/vulkan/implicit_layer.d/VkLayer_NATIVEPIPE_blob_alignment.json"
#define NP_SESSION_USER_FILE "/var/lib/nativepipe/session-user"
#define NP_DESKTOP_PREFERENCES_FILE "/var/lib/nativepipe/desktop-preferences"

int np_read_full(int fd, void *buf, size_t n);
int np_write_full(int fd, const void *buf, size_t n);
int np_path_exists(const char *path);
/* True when the distribution installed a Mesa Venus ICD. LinPortal never
 * supplies or selects a second Mesa implementation inside the guest. */
int np_venus_icd_available(void);
int np_mkdir_p(const char *path);
int np_write_file(const char *path, const void *data, size_t n, int mode);
int np_run(char *const argv[]);

/* Connect to host CID 2. retries is attempts with 1s sleep; 0 means once. */
int np_vsock_connect_host(uint32_t port, int retries);

#endif
