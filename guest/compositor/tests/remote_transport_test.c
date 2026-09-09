#include "hostlink.h"
#include "host_transport.h"
#include "medialink.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

static int count;
static bool command(const unsigned char *payload, size_t size, void *data)
{
    (void)data;
    assert(size == 4 && payload[0] == 0x44);
    count++;
    return true;
}
static void read_all(int fd, void *bytes, size_t size)
{
    unsigned char *p = bytes;
    while (size) {
        struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
        assert(poll(&poll_fd, 1, 2000) > 0);
        ssize_t n = read(fd, p, size);
        assert(n > 0); p += n; size -= (size_t)n;
    }
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int input[2], output[2];
    assert(pipe(input) == 0 && pipe(output) == 0);
    struct np_host host = { .listen_fd = -1, .conn_fd = input[0] };
    np_host_set_nonblocking(input[0]);
    static const unsigned char frame[] = {
        'N','P','I','P',1,0,0,0,4,0,0,0,0x44,0,0,0
    };
    /* Real pipes, arbitrary fragmentation, no nonce/lane pairing required. */
    for (size_t i = 0; i < sizeof(frame); i++) {
        assert(write(input[1], frame + i, 1) == 1);
        np_host_pump(&host, command, NULL);
    }
    assert(count == 1);
    struct np_media media;
    assert(np_media_open(&media, output[1]));
    assert(np_media_send_binary(&media, frame + 12, 4));
    const unsigned char pixel[] = {1,2,3};
    assert(np_media_send(&media, NP_MEDIA_CODEC_H264, 0, 1, 2, 4, 4, 0, 0, pixel, 3));
    unsigned char result[16 + NP_MEDIA_HEADER_SIZE + 3];
    read_all(output[0], result, sizeof(result));
    assert(!memcmp(result, frame, 16));
    assert(!memcmp(result + 16, "NPEN", 4));
    assert(!memcmp(result + 16 + NP_MEDIA_HEADER_SIZE, pixel, 3));
    /* Failure wakes the event loop; no SIGPIPE abort, unbounded retry or spin. */
    close(output[0]);
    assert(np_media_send_binary(&media, frame + 12, 4));
    struct pollfd error = { .fd = media.error_fd, .events = POLLIN };
    assert(poll(&error, 1, 2000) > 0);
    assert(!np_media_connected(&media));
    np_media_finish(&media);
    close(input[1]);
    np_host_pump(&host, command, NULL);
    assert(!np_host_connected(&host));
    np_host_finish(&host);
    puts("SSH stdio framing, independent input and writer failure: PASS");
}
