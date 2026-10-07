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
#include <sys/socket.h>

bool np_trace_enabled(void) { return false; }

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
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static size_t note(unsigned char *p, uint32_t id, uint64_t revision, bool closing)
{
    memcpy(p, "NPW2\1\0\0\0", 8);
    p[5] = closing ? NP_GUEST_NOTIFICATION_CLOSED : NP_GUEST_NOTIFICATION_POSTED;
    put32(p + 8, id); memcpy(p + 12, &revision, 8);
    if (closing) return 20;
    size_t offset = 20;
    p[offset++] = 1; put32(p + offset, UINT32_MAX); offset += 4;
    const unsigned lengths[] = {128, 128, 512, 4096};
    for (unsigned i = 0; i < 4; i++) {
        put32(p + offset, lengths[i]); offset += 4;
        memset(p + offset, 'x', lengths[i]); offset += lengths[i];
    }
    put32(p + offset, 8); offset += 4;
    for (unsigned i = 0; i < 16; i++) {
        put32(p + offset, 128); offset += 4;
        memset(p + offset, 'a' + i / 2, 128); offset += 128;
    }
    return offset;
}
static struct np_host socket_host(int peer[2])
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, peer) == 0);
    int small = 1024;
    assert(setsockopt(peer[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    np_host_set_nonblocking(peer[0]); np_host_set_nonblocking(peer[1]);
    return (struct np_host){ .listen_fd = -1, .conn_fd = peer[0],
        .output_enabled = true, .input_enabled = true };
}
static size_t drain_host(struct np_host *host, int peer, unsigned char *stream, size_t capacity)
{
    size_t size = 0;
    for (unsigned attempt = 0; attempt < 10000; attempt++) {
        np_host_flush(host);
        assert(np_host_connected(host));
        ssize_t n = read(peer, stream + size, capacity - size);
        if (n > 0) { size += (size_t)n; assert(size < capacity); continue; }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        if (!np_host_has_backlog(host)) return size;
    }
    assert(!"notification output did not drain"); return size;
}
static void test_vm_notification_pressure(void)
{
    int peer[2]; struct np_host host = socket_host(peer);
    unsigned char payload[8192], scene[4096] = {'S'}, *stream = malloc(1024 * 1024);
    assert(stream);
    /* Block a real socket behind complete/partial structural NPIP records. */
    for (unsigned i = 0; i < 100 && host.out_len == host.out_head; i++)
        assert(np_host_send_binary(&host, scene, sizeof(scene)));
    assert(host.out_len > host.out_head);
    uint64_t newest[64] = {0};
    for (unsigned i = 0; i < 10000; i++) {
        unsigned id = i % 64; newest[id] = i / 64 + 1;
        assert(np_host_send_binary(&host, payload, note(payload, id + 1, newest[id], false)));
        assert(host.notifications.posted_count <= 64 && host.notifications.bytes <= NP_NOTIFICATION_OUTBOX_BYTES);
        assert(np_host_connected(&host));
    }
    assert(!np_host_send_binary(&host, payload, note(payload, 65, 1, false)));
    assert(np_host_connected(&host)); /* Optional refusal is never fatal. */
    assert(np_host_send_binary(&host, payload, note(payload, 65, 1, true)));
    assert(np_host_send_binary(&host, payload, note(payload, 1, newest[0] - 1, true)));
    for (unsigned id = 1000; id < 11000; id++)
        assert(np_host_send_binary(&host, payload, note(payload, id, 1, true)));
    assert(np_host_send_binary(&host, "LAST", 4));
    size_t size = drain_host(&host, peer[1], stream, 1024 * 1024), offset = 0;
    bool structural = false, seen[64] = {0}; unsigned posts = 0, resets = 0;
    while (offset < size) {
        assert(size - offset >= 12 && !memcmp(stream + offset, "NPIP\1\0\0\0", 8));
        size_t length = get32(stream + offset + 8);
        assert(length <= size - offset - 12);
        const unsigned char *p = stream + offset + 12;
        if (length == 4 && !memcmp(p, "LAST", 4)) structural = true;
        if (np_notification_payload(p, length)) {
            assert(structural); /* A fresh structural record passed the flood. */
            if (p[5] == NP_GUEST_NOTIFICATION_BACKLOG_RESET) ++resets;
            if (p[5] == NP_GUEST_NOTIFICATION_POSTED) {
                unsigned id = get32(p + 8); assert(id && id <= 64 && !seen[id - 1]);
                seen[id - 1] = true; ++posts;
                assert(get64(p + 12) == newest[id - 1]);
            }
        }
        offset += 12 + length;
    }
    assert(structural && posts == 64 && resets == 1);
    assert(!np_host_has_backlog(&host) && host.notifications.bytes == 0);
    np_host_finish(&host); close(peer[1]);

    /* A partially sent notification must finish before a new scene's header. */
    host = socket_host(peer);
    size_t length = note(payload, 1, 1, false);
    assert(np_host_send_binary(&host, payload, length));
    assert(np_notification_outbox_started(&host.notifications));
    assert(np_host_send_binary(&host, "LAST", 4));
    size = drain_host(&host, peer[1], stream, 1024 * 1024);
    assert(get32(stream + 8) == length && !memcmp(stream + 12, payload, length));
    assert(size == 12 + length + 16 && !memcmp(stream + 12 + length + 12, "LAST", 4));
    np_host_finish(&host); close(peer[1]);

    /* Optional pressure cannot turn structural overflow into silent dropping. */
    host = socket_host(peer);
    for (unsigned i = 0; i < 100 && host.out_len == host.out_head; i++)
        assert(np_host_send_binary(&host, scene, sizeof(scene)));
    unsigned char *maximum = calloc(1, 8u * 1024u * 1024u); assert(maximum);
    assert(!np_host_send_binary(&host, maximum, 8u * 1024u * 1024u));
    assert(!np_host_connected(&host));
    free(maximum); free(stream); np_host_finish(&host); close(peer[1]);
}
static void test_remote_notification_pressure(void)
{
    int output[2]; assert(pipe(output) == 0);
    np_host_set_nonblocking(output[1]);
    static const unsigned char filler[] = {'N','P','I','P',1,0,0,0,4,0,0,0,'F','I','L','L'};
    size_t filled = 0;
    while (write(output[1], filler, sizeof(filler)) == sizeof(filler)) filled += sizeof(filler);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    struct np_media media; assert(np_media_open(&media, output[1]));
    unsigned char payload[8192];
    assert(np_media_send_notification(&media, payload, note(payload, 1, 1, false)));
    bool selected = false;
    for (unsigned i = 0; i < 2000 && !selected; i++) {
        pthread_mutex_lock(&media.lock); selected = media.notifications.active != NULL; pthread_mutex_unlock(&media.lock);
        if (!selected) usleep(1000);
    }
    assert(selected); /* Writer is blocked on an optional frame, before byte 0. */
    assert(np_media_send_binary(&media, "CTRL", 4));
    uint64_t newest[64] = {0};
    for (unsigned i = 0; i < 10000; i++) {
        unsigned id = i % 64; newest[id] = i / 64 + 2;
        assert(np_media_send_notification(&media, payload, note(payload, id + 1, newest[id], false)));
    }
    pthread_mutex_lock(&media.lock);
    assert(media.notifications.posted_count <= 64 && media.notifications.bytes <= NP_NOTIFICATION_OUTBOX_BYTES);
    assert(media.queued_bytes == 16 && media.display_bytes == 0);
    pthread_mutex_unlock(&media.lock);
    assert(np_media_can_encode(&media) && np_media_connected(&media));
    size_t pixel_size = 512 * 1024;
    unsigned char *pixels = malloc(pixel_size), *received = malloc(pixel_size + NP_MEDIA_HEADER_SIZE);
    assert(pixels && received); memset(pixels, 0x5a, pixel_size);
    assert(np_media_send(&media, NP_MEDIA_CODEC_H264, 0, 3, 9, 4, 4, 0, 0, pixels, (uint32_t)pixel_size));
    /* Leave enough time for the blocked optional writer to yield to CTRL. */
    usleep(60000);
    unsigned char chunk[28 + 16384];
    while (filled) { size_t n = filled < sizeof(chunk) ? filled : sizeof(chunk); read_all(output[0], chunk, n); filled -= n; }
    read_all(output[0], chunk, 16);
    assert(!memcmp(chunk, "NPIP\1\0\0\0", 8) && !memcmp(chunk + 12, "CTRL", 4));
    unsigned fragments = 0, first_note_fragment = UINT32_MAX, posts = 0;
    size_t pixel_offset = 0; bool seen[64] = {0};
    while (pixel_offset < pixel_size + NP_MEDIA_HEADER_SIZE || posts < 65) {
        read_all(output[0], chunk, 12);
        assert(!memcmp(chunk, "NPIP\1\0\0\0", 8));
        size_t length = get32(chunk + 8); assert(length <= sizeof(chunk) - 12);
        read_all(output[0], chunk + 12, length);
        unsigned char *p = chunk + 12;
        if (!memcmp(p, "NPRF", 4)) {
            assert(get32(p + 4) == 1 && get32(p + 12) == pixel_offset);
            assert(get32(p + 8) == pixel_size + NP_MEDIA_HEADER_SIZE);
            memcpy(received + pixel_offset, p + 16, length - 16);
            pixel_offset += length - 16; ++fragments;
            assert(np_media_acknowledge(&media, (uint32_t)(length - 16)));
        } else {
            assert(np_notification_payload(p, length) && p[5] == NP_GUEST_NOTIFICATION_POSTED);
            if (first_note_fragment == UINT32_MAX) first_note_fragment = fragments;
            unsigned id = get32(p + 8); assert(id && id <= 64);
            uint64_t revision = get64(p + 12);
            if (revision == newest[id - 1]) { assert(!seen[id - 1]); seen[id - 1] = true; }
            else assert(id == 1 && revision == 1); /* Already selected record is pinned. */
            ++posts;
        }
    }
    assert(first_note_fragment <= 8 && first_note_fragment < fragments);
    assert(!memcmp(received, "NPEN", 4) && !memcmp(received + NP_MEDIA_HEADER_SIZE, pixels, pixel_size));
    for (unsigned i = 0; i < 64; i++) assert(seen[i]);
    assert(np_media_connected(&media));
    np_media_finish(&media); close(output[0]); free(pixels); free(received);
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
    test_vm_notification_pressure();
    test_remote_notification_pressure();
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
    puts("Bounded optional notifications, intact prioritized scenes, SSH fragments, control under stalled video, writer failure: PASS");
}
