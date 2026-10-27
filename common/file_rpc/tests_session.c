#define _GNU_SOURCE
#undef NDEBUG
#include "np_file_session.h"
#include "np_file_rpc.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum { ECHO = 1, SINK, INFO };

struct side {
    struct np_file2_session *session;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int closed, closed_error;
    int notifications;
    uint16_t notified_opcode;
    char notified[64];
    int sink_started, sink_release, sink_done, sink_result, sink_errno;
    struct np_file2_stream *held[512];
    unsigned held_count;
};

struct work { struct side *side; struct np_file2_stream *stream; uint16_t opcode; char body[64]; size_t length; };

static void wait_until(struct side *side, int *flag) {
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 10;
    pthread_mutex_lock(&side->lock);
    while (!*flag) assert(pthread_cond_timedwait(&side->changed, &side->lock, &deadline) == 0);
    pthread_mutex_unlock(&side->lock);
}
static void set_flag(struct side *side, int *flag) {
    pthread_mutex_lock(&side->lock);
    *flag = 1;
    pthread_cond_broadcast(&side->changed);
    pthread_mutex_unlock(&side->lock);
}

static void *serve(void *argument) {
    struct work *work = argument;
    struct np_file2_stream *stream = work->stream;
    struct np_file2_message *message = malloc(sizeof(*message));
    assert(message);
    if (work->opcode == ECHO) {
        /* A requester may abandon the stream at any point. */
        int result = np_file2_respond(stream, 0, "echo", 4, 0) == 0 ? 1 : -1;
        while (result == 1 && (result = np_file2_receive(stream, message)) == 1) {
            assert(message->type == NP_FILE2_DATA);
            if (np_file2_send(stream, message->data, message->length, 0) < 0) result = -1;
        }
        if (!result) result = np_file2_send(stream, NULL, 0, 1);
        assert(!result || errno == ECANCELED || errno == ECONNRESET);
    } else if (work->opcode == SINK) {
        assert(np_file2_respond(stream, 0, NULL, 0, 0) == 0);
        pthread_mutex_lock(&work->side->lock);
        work->side->sink_started = 1;
        pthread_cond_broadcast(&work->side->changed);
        pthread_mutex_unlock(&work->side->lock);
        /* Consumes nothing until released: its data waits in the session. */
        wait_until(work->side, &work->side->sink_release);
        /* The requester cancels before releasing; let the CANCEL arrive. */
        for (int i = 0; i < 1000 && !np_file2_stream_cancelled(stream); i++) usleep(10000);
        int result = np_file2_receive(stream, message), error = errno;
        pthread_mutex_lock(&work->side->lock);
        work->side->sink_result = result;
        work->side->sink_errno = error;
        work->side->sink_done = 1;
        pthread_cond_broadcast(&work->side->changed);
        pthread_mutex_unlock(&work->side->lock);
    } else if (work->opcode == INFO) {
        char reversed[64];
        for (size_t i = 0; i < work->length; i++) reversed[i] = work->body[work->length - 1 - i];
        assert(np_file2_respond(stream, 42, reversed, work->length, 1) == 0);
        assert(np_file2_receive(stream, message) == 0);
    }
    free(message);
    np_file2_stream_release(stream);
    free(work);
    return NULL;
}

static void on_request(void *context, struct np_file2_stream *stream, uint16_t opcode,
                       const void *body, size_t length) {
    struct work *work = calloc(1, sizeof(*work));
    assert(work && length <= sizeof(work->body));
    work->side = context; work->stream = stream; work->opcode = opcode;
    memcpy(work->body, body, length); work->length = length;
    pthread_attr_t attributes; pthread_t thread;
    assert(pthread_attr_init(&attributes) == 0);
    /* musl's small default stack; message buffers live on the heap. */
    assert(pthread_attr_setstacksize(&attributes, 128 * 1024) == 0);
    assert(pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0);
    assert(pthread_create(&thread, &attributes, serve, work) == 0);
    pthread_attr_destroy(&attributes);
}
static void on_hold(void *context, struct np_file2_stream *stream, uint16_t opcode,
                    const void *body, size_t length) {
    (void)opcode; (void)body; (void)length;
    struct side *side = context;
    pthread_mutex_lock(&side->lock);
    assert(side->held_count < sizeof(side->held) / sizeof(side->held[0]));
    side->held[side->held_count++] = stream;
    pthread_mutex_unlock(&side->lock);
}
static void on_notify(void *context, uint16_t opcode, const void *body, size_t length) {
    struct side *side = context;
    pthread_mutex_lock(&side->lock);
    side->notifications++;
    side->notified_opcode = opcode;
    memset(side->notified, 0, sizeof(side->notified));
    memcpy(side->notified, body, length < sizeof(side->notified) - 1 ? length : sizeof(side->notified) - 1);
    pthread_cond_broadcast(&side->changed);
    pthread_mutex_unlock(&side->lock);
}
static void on_closed(void *context, int error) {
    struct side *side = context;
    pthread_mutex_lock(&side->lock);
    side->closed++; side->closed_error = error;
    pthread_cond_broadcast(&side->changed);
    pthread_mutex_unlock(&side->lock);
}

static void init_side(struct side *side) {
    memset(side, 0, sizeof(*side));
    pthread_mutex_init(&side->lock, NULL);
    pthread_cond_init(&side->changed, NULL);
}
enum serving { NONE, SERVE, HOLD };
static struct np_file2_session *start_side(struct side *side, int socket, enum np_file2_role role,
                                           enum serving serve) {
    struct np_file2_handlers handlers = {side, serve == SERVE ? on_request : serve == HOLD ? on_hold : NULL,
                                         on_notify, on_closed};
    side->session = np_file2_session_start(socket, role, &handlers);
    assert(side->session);
    return side->session;
}
static void pair(struct side *host, struct side *guest, int host_serves, int guest_serves) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    init_side(host); init_side(guest);
    start_side(host, sockets[0], NP_FILE2_ROLE_HOST, host_serves ? SERVE : NONE);
    start_side(guest, sockets[1], NP_FILE2_ROLE_GUEST, guest_serves ? SERVE : NONE);
}
static void close_pair(struct side *host, struct side *guest) {
    np_file2_session_close(host->session);
    np_file2_session_close(guest->session);
    assert(host->closed == 1 && guest->closed == 1);
}
static unsigned char pattern(size_t index, unsigned seed) { return (unsigned char)(index * 131u + seed); }

struct transfer { struct np_file2_stream *stream; size_t size; unsigned seed; int result, error; };
static void *send_pattern(void *argument) {
    struct transfer *transfer = argument;
    unsigned char *buffer = malloc(100000);
    assert(buffer);
    size_t sent = 0;
    transfer->result = 0;
    while (sent < transfer->size) {
        size_t n = transfer->size - sent < 100000 ? transfer->size - sent : 100000;
        for (size_t i = 0; i < n; i++) buffer[i] = pattern(sent + i, transfer->seed);
        if (np_file2_send(transfer->stream, buffer, n, sent + n == transfer->size) < 0) {
            transfer->result = -1; transfer->error = errno; break;
        }
        sent += n;
    }
    if (!transfer->size && np_file2_send(transfer->stream, NULL, 0, 1) < 0) {
        transfer->result = -1; transfer->error = errno;
    }
    free(buffer);
    return NULL;
}
/* Sends `size` patterned bytes through an ECHO stream while reading them back. */
static void echo(struct np_file2_session *session, size_t size, unsigned seed) {
    struct np_file2_stream *stream = np_file2_request(session, ECHO, "x", 1, 0);
    assert(stream);
    struct np_file2_message *message = malloc(sizeof(*message));
    assert(message);
    assert(np_file2_receive(stream, message) == 1);
    assert(message->type == NP_FILE2_RESPONSE && message->status == 0 &&
           message->length == 4 && !memcmp(message->data, "echo", 4) && !message->end);
    struct transfer transfer = {stream, size, seed, 0, 0};
    pthread_t sender; assert(pthread_create(&sender, NULL, send_pattern, &transfer) == 0);
    size_t received = 0;
    int result;
    while ((result = np_file2_receive(stream, message)) == 1) {
        assert(message->type == NP_FILE2_DATA && message->length <= NP_FILE2_FRAME_MAX);
        for (uint32_t i = 0; i < message->length; i++) assert(message->data[i] == pattern(received + i, seed));
        received += message->length;
    }
    assert(result == 0 && received == size);
    assert(pthread_join(sender, NULL) == 0 && transfer.result == 0);
    np_file2_stream_release(stream);
    free(message);
}

static void test_echo_up_to_the_stream_limit(void) {
    struct side host, guest; pair(&host, &guest, 0, 1);
    echo(host.session, NP_FILE2_STREAM_DATA_MAX, 7);
    echo(host.session, 0, 1);
    close_pair(&host, &guest);
}

static void test_send_beyond_the_stream_limit_is_refused(void) {
    struct side host, guest; pair(&host, &guest, 0, 1);
    struct np_file2_stream *stream = np_file2_request(host.session, ECHO, NULL, 0, 0);
    struct np_file2_message message;
    assert(stream && np_file2_receive(stream, &message) == 1);
    static unsigned char bytes[NP_FILE2_STREAM_DATA_MAX + 1];
    /* Nothing is sent: the whole send is refused, not cut short. */
    assert(np_file2_send(stream, bytes, sizeof(bytes), 1) == -1 && errno == EMSGSIZE);
    assert(np_file2_send(stream, bytes, NP_FILE2_STREAM_DATA_MAX, 0) == 0);
    assert(np_file2_send(stream, bytes, 1, 1) == -1 && errno == EMSGSIZE);
    np_file2_stream_release(stream);
    close_pair(&host, &guest);
}

struct concurrent { struct np_file2_session *session; unsigned seed; };
static void *concurrent_echo(void *argument) {
    struct concurrent *c = argument;
    echo(c->session, NP_FILE2_STREAM_DATA_MAX - c->seed * 977, c->seed);
    return NULL;
}
static void test_multiplexed_streams_both_directions(void) {
    struct side host, guest; pair(&host, &guest, 1, 1);
    pthread_t threads[12]; struct concurrent work[12];
    for (unsigned i = 0; i < 12; i++) {
        /* Half the streams open from each side, sharing one connection. */
        work[i].session = i % 2 ? guest.session : host.session; work[i].seed = i + 1;
        assert(pthread_create(&threads[i], NULL, concurrent_echo, &work[i]) == 0);
    }
    for (unsigned i = 0; i < 12; i++) assert(pthread_join(threads[i], NULL) == 0);
    close_pair(&host, &guest);
}

static void test_request_response_and_notifications(void) {
    struct side host, guest; pair(&host, &guest, 1, 0);
    struct np_file2_stream *stream = np_file2_request(guest.session, INFO, "abc", 3, 1);
    assert(stream && np_file2_stream_id(stream) % 2 == 1);
    struct np_file2_message message;
    assert(np_file2_receive(stream, &message) == 1);
    assert(message.type == NP_FILE2_RESPONSE && message.status == 42 && message.end &&
           message.length == 3 && !memcmp(message.data, "cba", 3));
    assert(np_file2_receive(stream, &message) == 0);
    np_file2_stream_release(stream);

    assert(np_file2_notify(host.session, 9, "invalidate", 10) == 0);
    wait_until(&guest, &guest.notifications);
    assert(guest.notified_opcode == 9 && !strcmp(guest.notified, "invalidate"));
    assert(np_file2_notify(guest.session, 3, "", 0) == 0);
    wait_until(&host, &host.notifications);
    assert(host.notified_opcode == 3);
    close_pair(&host, &guest);
}

static void test_unserved_request_is_cancelled(void) {
    struct side host, guest; pair(&host, &guest, 0, 0);
    struct np_file2_stream *stream = np_file2_request(host.session, INFO, NULL, 0, 1);
    assert(stream);
    struct np_file2_message message;
    assert(np_file2_receive(stream, &message) == -1 && errno == ECANCELED);
    np_file2_stream_release(stream);
    close_pair(&host, &guest);
}

static void test_stalled_stream_and_cancel(void) {
    struct side host, guest; pair(&host, &guest, 0, 1);
    struct np_file2_stream *stream = np_file2_request(host.session, SINK, NULL, 0, 0);
    assert(stream);
    struct np_file2_message message;
    assert(np_file2_receive(stream, &message) == 1 && message.type == NP_FILE2_RESPONSE);
    wait_until(&guest, &guest.sink_started);
    /* The sink reads nothing yet, so a full stream's data waits in its session. */
    struct transfer transfer = {stream, NP_FILE2_STREAM_DATA_MAX, 3, 0, 0};
    pthread_t sender; assert(pthread_create(&sender, NULL, send_pattern, &transfer) == 0);
    assert(pthread_join(sender, NULL) == 0 && transfer.result == 0);
    /* Another stream on the same connection still makes progress. */
    echo(host.session, 100000, 5);
    np_file2_cancel(stream);
    np_file2_stream_release(stream);
    /* The responder sees the cancellation, and buffered data is dropped. */
    set_flag(&guest, &guest.sink_release);
    wait_until(&guest, &guest.sink_done);
    assert(guest.sink_result == -1 && guest.sink_errno == ECANCELED);
    close_pair(&host, &guest);
}

static void test_release_cancels_and_close_wakes(void) {
    struct side host, guest; pair(&host, &guest, 0, 1);
    struct np_file2_stream *abandoned = np_file2_request(host.session, ECHO, NULL, 0, 0);
    assert(abandoned);
    np_file2_stream_release(abandoned);
    /* The connection stays usable after the peer sees the cancellation. */
    echo(host.session, 70000, 9);
    struct np_file2_stream *pending = np_file2_request(host.session, ECHO, NULL, 0, 0);
    struct np_file2_message message;
    assert(np_file2_receive(pending, &message) == 1);
    np_file2_session_close(guest.session);
    wait_until(&host, &host.closed);
    assert(np_file2_receive(pending, &message) == -1 && errno == ECONNRESET);
    assert(np_file2_send(pending, "x", 1, 0) == -1 && errno == ECONNRESET);
    assert(np_file2_request(host.session, ECHO, NULL, 0, 0) == NULL && errno == ECONNRESET);
    np_file2_session_close(host.session);
    /* A stream outliving its session's owner stays valid until released. */
    assert(np_file2_receive(pending, &message) == -1 && errno == ECONNRESET);
    np_file2_stream_release(pending);
}

static void raw_frame(int socket, uint8_t type, uint8_t flags, uint32_t id, const void *body, uint32_t length) {
    unsigned char header[NP_FILE2_HEADER] = {'N','P','2','F', type, flags, 0, 0};
    np_file_put32(header + 8, id); np_file_put32(header + 12, length);
    /* Writes after the violation may fail once the session has closed. */
    if (send(socket, header, sizeof(header), MSG_NOSIGNAL) == (ssize_t)sizeof(header) && length)
        (void)send(socket, body, length, MSG_NOSIGNAL);
}
static void raw_hello(int socket, enum np_file2_role role) {
    unsigned char hello[8] = {NP_FILE2_VERSION, (unsigned char)role};
    raw_frame(socket, NP_FILE2_HELLO, 0, 0, hello, sizeof(hello));
}
struct injection { void (*inject)(int socket); int socket; };
static void *run_injection(void *argument) {
    struct injection *injection = argument;
#ifdef SO_NOSIGPIPE
    int yes = 1; setsockopt(injection->socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    injection->inject(injection->socket);
    return NULL;
}
static void expect_failure(void (*inject)(int socket), int expected) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct side side; init_side(&side);
    start_side(&side, sockets[0], NP_FILE2_ROLE_HOST, HOLD);
    /* A writer the session stops reading only wakes once the session closes. */
    struct injection injection = {inject, sockets[1]};
    pthread_t thread; assert(pthread_create(&thread, NULL, run_injection, &injection) == 0);
    wait_until(&side, &side.closed);
    if (side.closed_error != expected) fprintf(stderr, "closed with %d, expected %d\n", side.closed_error, expected);
    assert(side.closed_error == expected);
    np_file2_session_close(side.session);
    for (unsigned i = 0; i < side.held_count; i++) np_file2_stream_release(side.held[i]);
    assert(pthread_join(thread, NULL) == 0);
    close(sockets[1]);
}
static void expect_protocol_error(void (*inject)(int socket)) { expect_failure(inject, EPROTO); }
static void bad_magic(int socket) { (void)send(socket, "NPFR\1\1\0\0\0\0\0\0\0\0\0\0", 16, MSG_NOSIGNAL); }
static void same_role(int socket) { raw_hello(socket, NP_FILE2_ROLE_HOST); }
static void wrong_parity(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    raw_frame(socket, NP_FILE2_REQUEST, 0, 2, "\1\0\0\0", 4);
}
static void decreasing_id(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    raw_frame(socket, NP_FILE2_REQUEST, 0, 5, "\1\0\0\0", 4);
    raw_frame(socket, NP_FILE2_REQUEST, 0, 3, "\1\0\0\0", 4);
}
static void unsolicited_response(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    raw_frame(socket, NP_FILE2_RESPONSE, 0, 2, "\0\0\0\0", 4);
}
static void stream_data_limit_exceeded(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    static unsigned char chunk[NP_FILE2_FRAME_MAX];
    raw_frame(socket, NP_FILE2_REQUEST, 0, 1, "\1\0\0\0", 4);
    for (unsigned i = 0; i <= NP_FILE2_STREAM_DATA_MAX / NP_FILE2_FRAME_MAX; i++)
        raw_frame(socket, NP_FILE2_DATA, 0, 1, chunk, sizeof(chunk));
}
static void data_after_end(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    raw_frame(socket, NP_FILE2_REQUEST, NP_FILE2_END_STREAM, 1, "\1\0\0\0", 4);
    raw_frame(socket, NP_FILE2_DATA, 0, 1, "x", 1);
}
/* Type 7 was CREDIT before flow control was removed. */
static void unknown_type(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    raw_frame(socket, 7, 0, 0, "\1\0\0\0", 4);
}

/* Past the inbound stream limit every request is refused with a CANCEL; a
 * peer that never reads those must not grow the queue without bound. */
static void refusal_flood(int socket) {
    raw_hello(socket, NP_FILE2_ROLE_GUEST);
    unsigned char frame[NP_FILE2_HEADER + 4] = {'N','P','2','F', NP_FILE2_REQUEST, 0, 0, 0};
    np_file_put32(frame + 12, 4); np_file_put32(frame + NP_FILE2_HEADER, 1);
    uint32_t limit = 2 * (NP_FILE2_MAX_PENDING_CONTROL + NP_FILE2_MAX_INBOUND_STREAMS + 4096);
    for (uint32_t id = 1; id < limit; id += 2) {
        np_file_put32(frame + 8, id);
        /* Stops once the session drops this peer. */
        if (send(socket, frame, sizeof(frame), MSG_NOSIGNAL) != (ssize_t)sizeof(frame)) break;
    }
}

int main(void) {
    test_echo_up_to_the_stream_limit();
    test_send_beyond_the_stream_limit_is_refused();
    test_multiplexed_streams_both_directions();
    test_request_response_and_notifications();
    test_unserved_request_is_cancelled();
    test_stalled_stream_and_cancel();
    test_release_cancels_and_close_wakes();
    expect_protocol_error(bad_magic);
    expect_protocol_error(same_role);
    expect_protocol_error(wrong_parity);
    expect_protocol_error(decreasing_id);
    expect_protocol_error(unsolicited_response);
    expect_protocol_error(stream_data_limit_exceeded);
    expect_protocol_error(data_after_end);
    expect_protocol_error(unknown_type);
    expect_failure(refusal_flood, ENOBUFS);
    puts("file session tests passed");
    return 0;
}
