#define _GNU_SOURCE
#include "np.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

struct sockaddr_vm {
    sa_family_t svm_family;
    unsigned short svm_reserved1;
    unsigned int svm_port;
    unsigned int svm_cid;
    unsigned char svm_zero[sizeof(struct sockaddr) - sizeof(sa_family_t) -
                           sizeof(unsigned short) - sizeof(unsigned int) -
                           sizeof(unsigned int)];
};

static int socket_stream_cloexec(int domain) {
    int type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif
    int fd = socket(domain, type, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int np_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) {
            errno = EIO;
            return -1;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int np_write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

int np_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int np_venus_icd_available(void) {
    static const char *const directories[] = {
        "/usr/share/vulkan/icd.d",
        "/etc/vulkan/icd.d",
    };
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        DIR *dir = opendir(directories[i]);
        if (!dir)
            continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            size_t length = strlen(name);
            if (strncmp(name, "virtio_icd", 10) == 0 && length >= 5 &&
                strcmp(name + length - 5, ".json") == 0) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    return 0;
}

int np_mkdir_p(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int np_write_file(const char *path, const void *data, size_t n, int mode) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (np_mkdir_p(dir) < 0)
            return -1;
    }
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (fd < 0)
        return -1;
    int rc = np_write_full(fd, data, n);
    close(fd);
    if (rc == 0)
        chmod(path, (mode_t)mode);
    return rc;
}

int np_write_version(const char *ver) {
    if (!ver || !ver[0])
        return 0;
    char buf[NP_MAX_VERSION + 2];
    snprintf(buf, sizeof(buf), "%s\n", ver);
    if (np_mkdir_p("/usr/libexec/nativepipe") < 0)
        return -1;
    return np_write_file(NP_INSTALLED_VERSION, buf, strlen(buf), 0644);
}

int np_read_version(char *out, size_t cap) {
    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    FILE *f = fopen(NP_INSTALLED_VERSION, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return 0;
}

int np_copy_file(const char *src, const char *dst, int mode) {
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", dst);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        np_mkdir_p(dir);
    }
    int out = open(dst, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    uint8_t buf[65536];
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(in);
            close(out);
            return -1;
        }
        if (r == 0)
            break;
        if (np_write_full(out, buf, (size_t)r) < 0) {
            close(in);
            close(out);
            return -1;
        }
    }
    close(in);
    close(out);
    chmod(dst, (mode_t)mode);
    return 0;
}

int np_run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

int np_vsock_connect_host(uint32_t port, int retries) {
    if (retries < 1)
        retries = 1;
    int fd = -1;
    for (int attempt = 0; attempt < retries; attempt++) {
        if (fd >= 0)
            close(fd);
        fd = socket_stream_cloexec(AF_VSOCK);
        if (fd < 0)
            return -1;
        struct sockaddr_vm addr;
        memset(&addr, 0, sizeof(addr));
        addr.svm_family = AF_VSOCK;
        addr.svm_cid = NP_CID_HOST;
        addr.svm_port = port;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        if (attempt + 1 < retries)
            sleep(1);
    }
    if (fd >= 0)
        close(fd);
    return -1;
}

int np_vsock_listen(uint32_t port, int backlog) {
    int fd = socket_stream_cloexec(AF_VSOCK);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = NP_CID_ANY;
    addr.svm_port = port;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog > 0 ? backlog : 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int np_npip_send(int fd, const void *payload, size_t payload_len) {
    if (payload_len > NP_MAX_NPIP_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint32_t n = (uint32_t)payload_len;
    /* payload length is u32 LE at offset 8 in the 12-byte header:
       "NPIP" | version(1) | 3 reserved | length(u32 LE) */
    uint8_t frame[12];
    memcpy(frame, NP_NPIP_MAGIC, 4);
    frame[4] = NP_WIRE_VERSION;
    frame[5] = 0;
    frame[6] = 0;
    frame[7] = 0;
    frame[8] = (uint8_t)(n & 0xff);
    frame[9] = (uint8_t)((n >> 8) & 0xff);
    frame[10] = (uint8_t)((n >> 16) & 0xff);
    frame[11] = (uint8_t)((n >> 24) & 0xff);
    if (np_write_full(fd, frame, sizeof(frame)) < 0)
        return -1;
    return np_write_full(fd, payload, payload_len);
}

ssize_t np_npip_recv(int fd, uint8_t **out) {
    uint8_t hdr[12];
    if (np_read_full(fd, hdr, sizeof(hdr)) < 0)
        return -1;
    if (memcmp(hdr, NP_NPIP_MAGIC, 4) != 0 || hdr[4] != NP_WIRE_VERSION) {
        errno = EPROTO;
        return -1;
    }
    uint32_t n = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                 ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (n > NP_MAX_NPIP_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint8_t *buf = malloc(n + 1);
    if (!buf)
        return -1;
    if (n && np_read_full(fd, buf, n) < 0) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    *out = buf;
    return (ssize_t)n;
}
