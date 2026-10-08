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
