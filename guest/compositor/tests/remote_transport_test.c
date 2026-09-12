#include "hostlink.h"
#include "host_transport.h"
#include "medialink.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    struct np_remote_flow flow = {0};
    double now = 1;
    for (unsigned phase = 0; phase < 3; phase++) {
        for (unsigned i = 0; i < 16; i++) {
            size_t size;
            while ((size = np_flow_allow(&flow, 1000000))) np_flow_sent(&flow, size, now);
            now += phase == 1 ? 0.5 : phase == 2 ? 0.13 : 0.1;
            assert(np_flow_ack(&flow, flow.bytes, now));
        }
        if (phase == 1) assert(flow.window < 65536);
        else assert(flow.window > 4096);
    }
    assert(!np_flow_ack(&flow, 1, now));
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
    unsigned char result[16 + 28 + NP_MEDIA_HEADER_SIZE + 3];
    read_all(output[0], result, sizeof(result));
    assert(!memcmp(result, frame, 16));
    assert(!memcmp(result + 16 + 12, "NPRF", 4));
    assert(!memcmp(result + 16 + 28, "NPEN", 4));
    assert(!memcmp(result + 16 + 28 + NP_MEDIA_HEADER_SIZE, pixel, 3));
    assert(np_media_acknowledge(&media, NP_MEDIA_HEADER_SIZE + 3));
    /* A slow receiver stops bulk at the initial 64 KiB credit budget.
     * Keys and control replies still pass without granting more bulk credit. */
    unsigned char *large = calloc(1, 512 * 1024);
    assert(large);
    assert(np_media_send(&media, NP_MEDIA_CODEC_H264, 0, 1, 3, 4, 4, 0, 0, large, 512 * 1024));
    free(large);
    unsigned char chunk[28 + 16384];
    for (int i = 0; i < 4; i++) {
        read_all(output[0], chunk, sizeof(chunk));
        assert(!memcmp(chunk + 12, "NPRF", 4));
    }
    struct pollfd paused = { .fd = output[0], .events = POLLIN };
    assert(poll(&paused, 1, 50) == 0);
    assert(!np_media_can_encode(&media));
    assert(write(input[1], frame, sizeof(frame)) == sizeof(frame));
    np_host_pump(&host, command, NULL);
    assert(count == 2);
    assert(np_media_send_binary(&media, frame + 12, 4));
    read_all(output[0], result, 16);
    assert(!memcmp(result, frame, 16));
    assert(np_media_acknowledge(&media, 65536));
    read_all(output[0], chunk, 28); /* Adaptive next fragment size may change. */
    /* Failure wakes the event loop; no SIGPIPE abort, unbounded retry or spin. */
    close(output[0]);
    /* The in-progress bulk write can observe EPIPE before this enqueue. Both
     * rejecting it immediately and accepting it before failure are correct. */
    (void)np_media_send_binary(&media, frame + 12, 4);
    while (np_media_connected(&media)) {
        struct pollfd error = { .fd = media.error_fd, .events = POLLIN };
        assert(poll(&error, 1, 2000) > 0);
        uint64_t wake;
        (void)read(media.error_fd, &wake, sizeof(wake));
    }
    assert(!np_media_connected(&media));
    assert(!np_media_send_binary(&media, frame + 12, 4));
    np_media_finish(&media);
    /* A large application/icon reply must not prevent the next scene from
     * being encoded. Its chunks share bandwidth, not the display work budget. */
    int background[2];
    assert(pipe(background) == 0 && np_media_open(&media, background[1]));
    large = calloc(1, 512 * 1024);
    assert(large);
    memcpy(large, "NPAP\1\1", 6);
    assert(np_media_send_binary(&media, large, 512 * 1024));
    free(large);
    const unsigned char end[] = {'N','P','A','P',1,4,0,0,7,0,0,0,0,0,0,0};
    assert(np_media_send_binary(&media, end, sizeof(end)));
    for (int i = 0; i < 4; i++) {
        read_all(background[0], chunk, sizeof(chunk));
        assert(!memcmp(chunk + 12, "NPRF", 4));
    }
    paused.fd = background[0];
    assert(poll(&paused, 1, 50) == 0); /* End cannot overtake incomplete batches. */
    assert(np_media_send_binary(&media, frame + 12, 4));
    read_all(background[0], result, 16);
    assert(!memcmp(result, frame, 16)); /* An independent control reply can. */
    assert(np_media_can_encode(&media));
    np_media_finish(&media);
    close(background[0]);
    close(input[1]);
    np_host_pump(&host, command, NULL);
    assert(!np_host_connected(&host));
    np_host_finish(&host);
    puts("SSH fragments, bounded in-flight bytes, control under stalled video, writer failure: PASS");
}
