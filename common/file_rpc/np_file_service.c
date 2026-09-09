#define _GNU_SOURCE
#include "np_file_rpc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NP_FILE_CLIENTS 16
struct np_file_service {
    int listener, wake[2], clients[NP_FILE_CLIENTS];
    char *root;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t drained;
    unsigned active;
};
struct file_client { struct np_file_service *service; unsigned slot; };
static void *serve_client(void *data) {
    struct file_client *client = data;
    struct np_file_service *service = client->service;
    unsigned slot = client->slot;
    free(client);
    int fd = service->clients[slot];
    int root = open(service->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root >= 0) { np_file_serve(fd, root, 0); close(root); }
    else np_file_send(fd, NP_FILE_END, 0, (uint32_t)errno, NULL, 0);
    /* Let the peer consume END before close: VZ may discard uncredited bytes
     * on an immediate close. Cancellation/service stop wakes this read. */
    shutdown(fd, SHUT_WR);
    char byte;
    for (;;) {
        ssize_t n = read(fd, &byte, 1);
        if (n > 0 || (n < 0 && errno == EINTR)) continue;
        break;
    }
    pthread_mutex_lock(&service->lock);
    close(fd);
    service->clients[slot] = -1;
    service->active--;
    pthread_cond_broadcast(&service->drained);
    pthread_mutex_unlock(&service->lock);
    return NULL;
}
static void *listen_clients(void *data) {
    struct np_file_service *service = data;
    struct pollfd pollers[2] = {{.fd = service->listener, .events = POLLIN},
                                {.fd = service->wake[0], .events = POLLIN}};
    for (;;) {
        int ready = poll(pollers, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || pollers[1].revents || (pollers[0].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (!(pollers[0].revents & POLLIN)) continue;
#ifdef __linux__
        int fd = accept4(service->listener, NULL, NULL, SOCK_CLOEXEC);
#else
        int fd = accept(service->listener, NULL, NULL);
#endif
        if (fd < 0) continue;
        if (!np_file_peer_is_host(fd)) { close(fd); continue; }
        pthread_mutex_lock(&service->lock);
        unsigned slot = 0;
        while (slot < NP_FILE_CLIENTS && service->clients[slot] >= 0) slot++;
        struct file_client *client = slot < NP_FILE_CLIENTS ? malloc(sizeof(*client)) : NULL;
        pthread_t thread;
        if (client) {
            client->service = service; client->slot = slot;
            service->clients[slot] = fd;
            service->active++;
        }
        if (!client || pthread_create(&thread, NULL, serve_client, client) != 0) {
            if (client) { service->clients[slot] = -1; service->active--; free(client); }
            shutdown(fd, SHUT_RDWR); close(fd);
        } else pthread_detach(thread);
        pthread_mutex_unlock(&service->lock);
    }
    return NULL;
}
struct np_file_service *np_file_service_start(uint32_t port, const char *root) {
    struct np_file_service *service = calloc(1, sizeof(*service));
    if (!service) return NULL;
    service->listener = -1;
    service->root = strdup(root);
    if (!service->root) goto fail;
    service->listener = np_file_listen_vsock(port);
    if (service->listener < 0) goto fail;
#ifdef __linux__
    if (pipe2(service->wake, O_CLOEXEC) < 0) goto fail;
#else
    if (pipe(service->wake) < 0) goto fail;
#endif
    for (unsigned i = 0; i < NP_FILE_CLIENTS; i++) service->clients[i] = -1;
    int error = pthread_mutex_init(&service->lock, NULL);
    if (error) goto fail_pipe;
    error = pthread_cond_init(&service->drained, NULL);
    if (error) goto fail_mutex;
    error = pthread_create(&service->thread, NULL, listen_clients, service);
    if (!error) return service;
    pthread_cond_destroy(&service->drained);
fail_mutex:
    pthread_mutex_destroy(&service->lock);
fail_pipe:
    close(service->wake[0]); close(service->wake[1]);
    errno = error;
fail:;
    int saved = errno;
    if (service->listener >= 0) close(service->listener);
    free(service->root); free(service); errno = saved; return NULL;
}
void np_file_service_stop(struct np_file_service *service) {
    if (!service) return;
    char byte = 1; (void)write(service->wake[1], &byte, 1);
    pthread_join(service->thread, NULL);
    pthread_mutex_lock(&service->lock);
    for (unsigned i = 0; i < NP_FILE_CLIENTS; i++)
        if (service->clients[i] >= 0) shutdown(service->clients[i], SHUT_RDWR);
    while (service->active) pthread_cond_wait(&service->drained, &service->lock);
    pthread_mutex_unlock(&service->lock);
    close(service->listener); close(service->wake[0]); close(service->wake[1]);
    pthread_cond_destroy(&service->drained); pthread_mutex_destroy(&service->lock);
    free(service->root); free(service);
}
