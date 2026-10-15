#define _GNU_SOURCE
#include "np_file_session.h"
#include "np_file_rpc.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Locking: `lock` guards all session and stream state. Socket writes happen
 * outside it, serialized by the `writing` gate, where control frames overtake
 * waiting DATA. The reader thread never writes: frames it must originate
 * (credit for discarded data, refusals) go through the control writer, so two
 * peers with full socket buffers cannot deadlock on each other's readers. */

struct inbound {
    struct inbound *next;
    uint8_t type, end;
    uint32_t status, length;
    unsigned char data[];
};

struct control {
    struct control *next;
    uint8_t type;
    uint32_t id, value;
};

struct np_file2_stream {
    struct np_file2_session *session;
    struct np_file2_stream *next;
    uint32_t id;
    int referenced, responder, responded;
    int local_ended, remote_ended, cancelled;
    uint32_t send_window, receive_window, consumed;
    struct inbound *head, *tail;
};

struct np_file2_session {
    int socket;
    enum np_file2_role role;
    struct np_file2_handlers handlers;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t reader, controller;
    struct np_file2_stream *streams;
    struct control *controls, *controls_tail;
    unsigned pending_controls;
    uint32_t next_id, last_peer_id;
    unsigned inbound_streams;
    uint32_t send_window, receive_window, consumed;
    int writing;
    unsigned control_waiting;
    int error, owner_closed;
    unsigned references;
    unsigned char *buffer;
};

static void put32(unsigned char *p, uint32_t n) { np_file_put32(p, n); }
static uint32_t get32(const unsigned char *p) { return np_file_u32(p); }

static int read_all(int fd, void *bytes, size_t size) {
    unsigned char *p = bytes;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int write_all(int fd, const void *bytes, size_t size) {
    const unsigned char *p = bytes;
    while (size) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
#else
        ssize_t n = write(fd, p, size);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}

static void free_session(struct np_file2_session *session) {
    struct control *control = session->controls;
    while (control) { struct control *next = control->next; free(control); control = next; }
    pthread_cond_destroy(&session->changed);
    pthread_mutex_destroy(&session->lock);
    free(session->buffer);
    free(session);
}

/* Locked. Marks the session failed; the caller shuts down the socket. */
static int fail_locked(struct np_file2_session *session, int error) {
    if (session->error) return 0;
    session->error = error ? error : ECONNRESET;
    pthread_cond_broadcast(&session->changed);
    return 1;
}
static void fail(struct np_file2_session *session, int error) {
    pthread_mutex_lock(&session->lock);
    int first = fail_locked(session, error);
    pthread_mutex_unlock(&session->lock);
    if (first) shutdown(session->socket, SHUT_RDWR);
}

/* Locked. */
static void queue_control(struct np_file2_session *session, uint8_t type, uint32_t id, uint32_t value) {
    if (session->error) return;
    struct control *control = session->pending_controls < NP_FILE2_MAX_PENDING_CONTROL
        ? calloc(1, sizeof(*control)) : NULL;
    if (!control) {
        fail_locked(session, session->pending_controls < NP_FILE2_MAX_PENDING_CONTROL ? ENOMEM : ENOBUFS);
        shutdown(session->socket, SHUT_RDWR);
        return;
    }
    session->pending_controls++;
    control->type = type; control->id = id; control->value = value;
    if (session->controls_tail) session->controls_tail->next = control;
    else session->controls = control;
    session->controls_tail = control;
    pthread_cond_broadcast(&session->changed);
}

/* Locked. Returns credit for DATA that will never be consumed. */
static void discard_inbound(struct np_file2_stream *stream) {
    struct np_file2_session *session = stream->session;
    uint32_t discarded = 0;
    while (stream->head) {
        struct inbound *message = stream->head;
        stream->head = message->next;
        if (message->type == NP_FILE2_DATA) discarded += message->length;
        free(message);
    }
    stream->tail = NULL;
    session->receive_window += discarded;
    if (discarded) queue_control(session, NP_FILE2_CREDIT, 0, discarded);
}

static struct np_file2_stream *find_stream(struct np_file2_session *session, uint32_t id) {
    for (struct np_file2_stream *stream = session->streams; stream; stream = stream->next)
        if (stream->id == id) return stream;
    return NULL;
}

/* Locked. Frees the stream once nothing can still use it. Returns whether the
 * whole session was freed (only possible after the owner closed it). */
static int maybe_destroy(struct np_file2_stream *stream) {
    struct np_file2_session *session = stream->session;
    if (stream->referenced) return 0;
    if (!stream->cancelled && !session->error && !(stream->local_ended && stream->remote_ended)) return 0;
    struct np_file2_stream **link = &session->streams;
    while (*link != stream) link = &(*link)->next;
    *link = stream->next;
    discard_inbound(stream);
    if (stream->responder) session->inbound_streams--;
    free(stream);
    pthread_cond_broadcast(&session->changed);
    return --session->references == 0;
}

static int acquire_writer(struct np_file2_session *session, int priority) {
    if (priority) session->control_waiting++;
    while (!session->error && (session->writing || (!priority && session->control_waiting)))
        pthread_cond_wait(&session->changed, &session->lock);
    if (priority) session->control_waiting--;
    if (session->error) { pthread_cond_broadcast(&session->changed); errno = ECONNRESET; return -1; }
    session->writing = 1;
    return 0;
}
/* Called with the gate held and the lock released; returns with lock held. */
static int write_frame_unlocked(struct np_file2_session *session, uint8_t type, uint8_t flags, uint32_t id,
                                const void *prefix, size_t prefix_length,
                                const void *body, size_t length) {
    unsigned char header[NP_FILE2_HEADER] = {'N','P','2','F', type, flags, 0, 0};
    put32(header + 8, id); put32(header + 12, (uint32_t)(prefix_length + length));
    int result = write_all(session->socket, header, sizeof(header));
    if (!result && prefix_length) result = write_all(session->socket, prefix, prefix_length);
    if (!result && length) result = write_all(session->socket, body, length);
    int error = errno;
    pthread_mutex_lock(&session->lock);
    session->writing = 0;
    pthread_cond_broadcast(&session->changed);
    if (result < 0) {
        if (fail_locked(session, error)) shutdown(session->socket, SHUT_RDWR);
        errno = error;
    }
    return result;
}
static int write_frame(struct np_file2_session *session, int priority, uint8_t type, uint8_t flags,
                       uint32_t id, const void *prefix, size_t prefix_length,
                       const void *body, size_t length) {
    pthread_mutex_lock(&session->lock);
    if (acquire_writer(session, priority) < 0) { pthread_mutex_unlock(&session->lock); return -1; }
    pthread_mutex_unlock(&session->lock);
    int result = write_frame_unlocked(session, type, flags, id, prefix, prefix_length, body, length);
    int error = errno;
    pthread_mutex_unlock(&session->lock);
    errno = error;
    return result;
}

static void *control_writer(void *argument) {
    struct np_file2_session *session = argument;
    pthread_mutex_lock(&session->lock);
    for (;;) {
        while (!session->error && !session->controls) pthread_cond_wait(&session->changed, &session->lock);
        if (session->error) break;
        struct control *control = session->controls;
        session->controls = control->next;
        if (!session->controls) session->controls_tail = NULL;
        session->pending_controls--;
        if (acquire_writer(session, 1) < 0) { free(control); break; }
        pthread_mutex_unlock(&session->lock);
        unsigned char value[4]; put32(value, control->value);
        size_t length = control->type == NP_FILE2_CREDIT ? 4 : 0;
        write_frame_unlocked(session, control->type, 0, control->id, value, length, NULL, 0);
        free(control);
    }
    pthread_mutex_unlock(&session->lock);
    return NULL;
}

static int protocol_error(void) { errno = EPROTO; return -1; }

static int enqueue(struct np_file2_stream *stream, uint8_t type, uint8_t end, uint32_t status,
                   const unsigned char *data, uint32_t length) {
    struct inbound *message = malloc(sizeof(*message) + length);
    if (!message) { errno = ENOMEM; return -1; }
    message->next = NULL; message->type = type; message->end = end;
    message->status = status; message->length = length;
    memcpy(message->data, data, length);
    if (stream->tail) stream->tail->next = message; else stream->head = message;
    stream->tail = message;
    return 0;
}

static int is_peer_id(const struct np_file2_session *session, uint32_t id) {
    return id && (id & 1u) != (session->role == NP_FILE2_ROLE_GUEST ? 1u : 0u);
}

/* Locked. Returns -1 with errno to end the session. */
static int dispatch(struct np_file2_session *session, uint8_t type, uint8_t flags, uint32_t id,
                    const unsigned char *payload, uint32_t length) {
    int end = flags & NP_FILE2_END_STREAM;
    struct np_file2_stream *stream = id ? find_stream(session, id) : NULL;
    switch (type) {
    case NP_FILE2_CREDIT: {
        if (length != 4 || !get32(payload)) return protocol_error();
        uint32_t increment = get32(payload);
        uint32_t *window = id ? (stream && !stream->local_ended ? &stream->send_window : NULL)
                              : &session->send_window;
        if (!window) return 0;
        if (*window > UINT32_MAX / 2 - increment) return protocol_error();
        *window += increment;
        pthread_cond_broadcast(&session->changed);
        return 0;
    }
    case NP_FILE2_CANCEL:
        if (!id || length) return protocol_error();
        if (stream && !stream->cancelled) {
            stream->cancelled = 1;
            discard_inbound(stream);
            pthread_cond_broadcast(&session->changed);
        }
        return 0;
    case NP_FILE2_REQUEST: {
        if (!is_peer_id(session, id) || id <= session->last_peer_id || length < 4) return protocol_error();
        session->last_peer_id = id;
        uint16_t opcode = (uint16_t)(payload[0] | payload[1] << 8);
        if (!session->handlers.request || session->inbound_streams >= NP_FILE2_MAX_INBOUND_STREAMS) {
            queue_control(session, NP_FILE2_CANCEL, id, 0);
            return 0;
        }
        stream = calloc(1, sizeof(*stream));
        if (!stream) { errno = ENOMEM; return -1; }
        stream->session = session; stream->id = id; stream->referenced = 1;
        stream->responder = 1; stream->remote_ended = end;
        stream->send_window = stream->receive_window = NP_FILE2_STREAM_WINDOW;
        stream->next = session->streams; session->streams = stream;
        session->inbound_streams++; session->references++;
        pthread_mutex_unlock(&session->lock);
        session->handlers.request(session->handlers.context, stream, opcode, payload + 4, length - 4);
        pthread_mutex_lock(&session->lock);
        return 0;
    }
    case NP_FILE2_RESPONSE:
        if (!id || is_peer_id(session, id) || length < 4) return protocol_error();
        if (!stream) return id < session->next_id ? 0 : protocol_error();
        if (stream->cancelled) return 0;
        if (stream->responded || stream->remote_ended) return protocol_error();
        stream->responded = 1;
        if (enqueue(stream, type, (uint8_t)end, get32(payload), payload + 4, length - 4) < 0) return -1;
        if (end) stream->remote_ended = 1;
        pthread_cond_broadcast(&session->changed);
        return 0;
    case NP_FILE2_DATA:
        if (!id || (!is_peer_id(session, id) && id >= session->next_id) ||
            (is_peer_id(session, id) && id > session->last_peer_id)) return protocol_error();
        if (length > session->receive_window) return protocol_error();
        session->receive_window -= length;
        if (!stream || stream->cancelled) {
            session->receive_window += length;
            if (length) queue_control(session, NP_FILE2_CREDIT, 0, length);
            return 0;
        }
        if (stream->remote_ended || (!stream->responder && !stream->responded) ||
            length > stream->receive_window) return protocol_error();
        stream->receive_window -= length;
        /* An empty DATA only carries END; it is not delivered as a message. */
        if (length && enqueue(stream, type, (uint8_t)end, 0, payload, length) < 0) return -1;
        if (end) stream->remote_ended = 1;
        pthread_cond_broadcast(&session->changed);
        return 0;
    case NP_FILE2_NOTIFY:
        if (id || length < 4) return protocol_error();
        if (session->handlers.notify) {
            pthread_mutex_unlock(&session->lock);
            session->handlers.notify(session->handlers.context,
                                     (uint16_t)(payload[0] | payload[1] << 8), payload + 4, length - 4);
            pthread_mutex_lock(&session->lock);
        }
        return 0;
    default:
        return protocol_error();
    }
}

static int read_frame(struct np_file2_session *session, uint8_t *type, uint8_t *flags,
                      uint32_t *id, uint32_t *length) {
    unsigned char header[NP_FILE2_HEADER];
    if (read_all(session->socket, header, sizeof(header)) < 0) return -1;
    *type = header[4]; *flags = header[5]; *id = get32(header + 8); *length = get32(header + 12);
    if (memcmp(header, "NP2F", 4) || header[6] || header[7] ||
        *type < NP_FILE2_HELLO || *type > NP_FILE2_CREDIT || *length > NP_FILE2_FRAME_MAX)
        return protocol_error();
    int may_end = *type == NP_FILE2_REQUEST || *type == NP_FILE2_RESPONSE || *type == NP_FILE2_DATA;
    if (*flags & ~(may_end ? NP_FILE2_END_STREAM : 0u)) return protocol_error();
    return read_all(session->socket, session->buffer, *length);
}

static void *reader(void *argument) {
    struct np_file2_session *session = argument;
    uint8_t type, flags; uint32_t id, length;
    int error = 0;
    if (read_frame(session, &type, &flags, &id, &length) < 0) error = errno;
    else if (type != NP_FILE2_HELLO || id || length != 8 || session->buffer[0] != NP_FILE2_VERSION ||
             session->buffer[1] == (unsigned char)session->role || session->buffer[1] > NP_FILE2_ROLE_GUEST ||
             session->buffer[2] || session->buffer[3]) error = EPROTO;
    while (!error) {
        if (read_frame(session, &type, &flags, &id, &length) < 0) { error = errno; break; }
        pthread_mutex_lock(&session->lock);
        int result = session->error ? (errno = session->error, -1)
                                    : dispatch(session, type, flags, id, session->buffer, length);
        if (result < 0) error = errno;
        pthread_mutex_unlock(&session->lock);
    }
    fail(session, error);
    /* Report the first cause, which may have been found outside the reader. */
    pthread_mutex_lock(&session->lock);
    error = session->error;
    pthread_mutex_unlock(&session->lock);
    if (session->handlers.closed) session->handlers.closed(session->handlers.context, error);
    return NULL;
}

struct np_file2_session *np_file2_session_start(int socket, enum np_file2_role role,
                                                const struct np_file2_handlers *handlers) {
#ifdef SO_NOSIGPIPE
    int yes = 1; setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    struct np_file2_session *session = calloc(1, sizeof(*session));
    unsigned char *buffer = malloc(NP_FILE2_FRAME_MAX);
    if (!session || !buffer) { free(session); free(buffer); close(socket); errno = ENOMEM; return NULL; }
    session->socket = socket; session->role = role; session->buffer = buffer;
    if (handlers) session->handlers = *handlers;
    session->next_id = role == NP_FILE2_ROLE_HOST ? 2 : 1;
    session->send_window = session->receive_window = NP_FILE2_CONNECTION_WINDOW;
    session->references = 1;
    pthread_mutex_init(&session->lock, NULL);
    pthread_cond_init(&session->changed, NULL);
    unsigned char hello[8] = {NP_FILE2_VERSION, (unsigned char)role, 0, 0, 0, 0, 0, 0};
    unsigned char header[NP_FILE2_HEADER] = {'N','P','2','F', NP_FILE2_HELLO, 0, 0, 0};
    put32(header + 8, 0); put32(header + 12, sizeof(hello));
    if (write_all(socket, header, sizeof(header)) < 0 || write_all(socket, hello, sizeof(hello)) < 0) {
        int error = errno; close(socket); free_session(session); errno = error; return NULL;
    }
    int error = pthread_create(&session->reader, NULL, reader, session);
    if (!error) {
        error = pthread_create(&session->controller, NULL, control_writer, session);
        if (error) { fail(session, error); pthread_join(session->reader, NULL); }
    }
    if (error) { close(socket); free_session(session); errno = error; return NULL; }
    return session;
}

void np_file2_session_close(struct np_file2_session *session) {
    if (!session) return;
    fail(session, ECONNRESET);
    pthread_join(session->reader, NULL);
    pthread_join(session->controller, NULL);
    close(session->socket);
    pthread_mutex_lock(&session->lock);
    session->owner_closed = 1;
    /* Unreferenced streams can go now; referenced ones on release. */
    struct np_file2_stream *stream = session->streams;
    int freed = 0;
    while (stream && !freed) {
        struct np_file2_stream *next = stream->next;
        freed = maybe_destroy(stream);
        stream = next;
    }
    if (!freed) freed = --session->references == 0;
    pthread_mutex_unlock(&session->lock);
    if (freed) free_session(session);
}

int np_file2_notify(struct np_file2_session *session, uint16_t opcode, const void *body, size_t length) {
    if (length > NP_FILE2_FRAME_MAX - 4) { errno = EMSGSIZE; return -1; }
    unsigned char prefix[4] = {(unsigned char)opcode, (unsigned char)(opcode >> 8), 0, 0};
    return write_frame(session, 1, NP_FILE2_NOTIFY, 0, 0, prefix, sizeof(prefix), body, length);
}

struct np_file2_stream *np_file2_request(struct np_file2_session *session, uint16_t opcode,
                                         const void *body, size_t length, int end_stream) {
    if (length > NP_FILE2_FRAME_MAX - 4) { errno = EMSGSIZE; return NULL; }
    struct np_file2_stream *stream = calloc(1, sizeof(*stream));
    if (!stream) { errno = ENOMEM; return NULL; }
    pthread_mutex_lock(&session->lock);
    /* IDs must reach the wire in increasing order, so allocate under the gate. */
    if (acquire_writer(session, 1) < 0) { pthread_mutex_unlock(&session->lock); free(stream); return NULL; }
    if (session->next_id > UINT32_MAX - 2) {
        session->writing = 0; pthread_cond_broadcast(&session->changed);
        pthread_mutex_unlock(&session->lock); free(stream); errno = EOVERFLOW; return NULL;
    }
    stream->session = session; stream->id = session->next_id; session->next_id += 2;
    stream->referenced = 1; stream->local_ended = !!end_stream;
    stream->send_window = stream->receive_window = NP_FILE2_STREAM_WINDOW;
    stream->next = session->streams; session->streams = stream;
    session->references++;
    pthread_mutex_unlock(&session->lock);
    unsigned char prefix[4] = {(unsigned char)opcode, (unsigned char)(opcode >> 8), 0, 0};
    int result = write_frame_unlocked(session, NP_FILE2_REQUEST, end_stream ? NP_FILE2_END_STREAM : 0,
                                      stream->id, prefix, sizeof(prefix), body, length);
    int error = errno;
    pthread_mutex_unlock(&session->lock);
    if (result < 0) { np_file2_stream_release(stream); errno = error; return NULL; }
    return stream;
}

static int stream_unusable(struct np_file2_stream *stream) {
    if (stream->cancelled) { errno = ECANCELED; return 1; }
    if (stream->session->error) { errno = ECONNRESET; return 1; }
    if (stream->local_ended) { errno = EPIPE; return 1; }
    return 0;
}

int np_file2_respond(struct np_file2_stream *stream, uint32_t status,
                     const void *body, size_t length, int end_stream) {
    struct np_file2_session *session = stream->session;
    if (length > NP_FILE2_FRAME_MAX - 4) { errno = EMSGSIZE; return -1; }
    pthread_mutex_lock(&session->lock);
    if (stream_unusable(stream)) { pthread_mutex_unlock(&session->lock); return -1; }
    if (!stream->responder || stream->responded) { pthread_mutex_unlock(&session->lock); errno = EINVAL; return -1; }
    if (acquire_writer(session, 1) < 0) { pthread_mutex_unlock(&session->lock); return -1; }
    if (stream_unusable(stream)) {
        int error = errno;
        session->writing = 0; pthread_cond_broadcast(&session->changed);
        pthread_mutex_unlock(&session->lock); errno = error; return -1;
    }
    stream->responded = 1;
    if (end_stream) stream->local_ended = 1;
    pthread_mutex_unlock(&session->lock);
    unsigned char prefix[4]; put32(prefix, status);
    int result = write_frame_unlocked(session, NP_FILE2_RESPONSE, end_stream ? NP_FILE2_END_STREAM : 0,
                                      stream->id, prefix, sizeof(prefix), body, length);
    int error = errno;
    pthread_mutex_unlock(&session->lock);
    errno = error;
    return result;
}

int np_file2_send(struct np_file2_stream *stream, const void *data, size_t length, int end_stream) {
    struct np_file2_session *session = stream->session;
    const unsigned char *bytes = data;
    if (!length && !end_stream) return 0;
    pthread_mutex_lock(&session->lock);
    if (stream->responder && !stream->responded) { pthread_mutex_unlock(&session->lock); errno = EINVAL; return -1; }
    do {
        while (length && !stream->cancelled && !session->error && !stream->local_ended &&
               (!stream->send_window || !session->send_window))
            pthread_cond_wait(&session->changed, &session->lock);
        if (stream_unusable(stream)) { pthread_mutex_unlock(&session->lock); return -1; }
        size_t size = length;
        if (size > NP_FILE2_FRAME_MAX) size = NP_FILE2_FRAME_MAX;
        if (size > stream->send_window) size = stream->send_window;
        if (size > session->send_window) size = session->send_window;
        stream->send_window -= (uint32_t)size; session->send_window -= (uint32_t)size;
        int last = size == length && end_stream;
        if (acquire_writer(session, 0) < 0) { pthread_mutex_unlock(&session->lock); return -1; }
        if (stream_unusable(stream)) {
            /* Cancelled while waiting: the peer never sees these bytes, so the
             * credit must be restored or the connection window shrinks for good. */
            int error = errno;
            stream->send_window += (uint32_t)size; session->send_window += (uint32_t)size;
            session->writing = 0; pthread_cond_broadcast(&session->changed);
            pthread_mutex_unlock(&session->lock); errno = error; return -1;
        }
        if (last) stream->local_ended = 1;
        pthread_mutex_unlock(&session->lock);
        if (write_frame_unlocked(session, NP_FILE2_DATA, last ? NP_FILE2_END_STREAM : 0,
                                 stream->id, NULL, 0, bytes, size) < 0) {
            int error = errno; pthread_mutex_unlock(&session->lock); errno = error; return -1;
        }
        bytes += size; length -= size;
    } while (length);
    pthread_mutex_unlock(&session->lock);
    return 0;
}

int np_file2_receive(struct np_file2_stream *stream, struct np_file2_message *message) {
    struct np_file2_session *session = stream->session;
    pthread_mutex_lock(&session->lock);
    while (!stream->head && !stream->remote_ended && !stream->cancelled && !session->error)
        pthread_cond_wait(&session->changed, &session->lock);
    struct inbound *inbound = stream->head;
    if (!inbound) {
        int result = 0;
        if (stream->cancelled) { errno = ECANCELED; result = -1; }
        else if (!stream->remote_ended) { errno = ECONNRESET; result = -1; }
        pthread_mutex_unlock(&session->lock);
        return result;
    }
    stream->head = inbound->next;
    if (!stream->head) stream->tail = NULL;
    uint32_t stream_credit = 0, connection_credit = 0;
    if (inbound->type == NP_FILE2_DATA) {
        stream->consumed += inbound->length; session->consumed += inbound->length;
        if (!stream->remote_ended && stream->consumed >= NP_FILE2_STREAM_WINDOW / 4) {
            stream_credit = stream->consumed; stream->receive_window += stream_credit; stream->consumed = 0;
        }
        if (session->consumed >= NP_FILE2_CONNECTION_WINDOW / 4) {
            connection_credit = session->consumed; session->receive_window += connection_credit; session->consumed = 0;
        }
    }
    pthread_mutex_unlock(&session->lock);
    message->type = inbound->type; message->end = inbound->end;
    message->status = inbound->status; message->length = inbound->length;
    memcpy(message->data, inbound->data, inbound->length);
    free(inbound);
    /* The message is already delivered; a failed credit write ends the session,
     * which the next call reports. */
    unsigned char value[4];
    if (connection_credit) {
        put32(value, connection_credit);
        write_frame(session, 1, NP_FILE2_CREDIT, 0, 0, value, 4, NULL, 0);
    }
    if (stream_credit) {
        put32(value, stream_credit);
        write_frame(session, 1, NP_FILE2_CREDIT, 0, stream->id, value, 4, NULL, 0);
    }
    return 1;
}

void np_file2_cancel(struct np_file2_stream *stream) {
    struct np_file2_session *session = stream->session;
    pthread_mutex_lock(&session->lock);
    if (!stream->cancelled) {
        /* Queued, never written here: cancelling (and so releasing) a stream
         * must not block, including from the reader's request callback. */
        if (!(stream->local_ended && stream->remote_ended)) queue_control(session, NP_FILE2_CANCEL, stream->id, 0);
        stream->cancelled = 1;
        discard_inbound(stream);
        pthread_cond_broadcast(&session->changed);
    }
    pthread_mutex_unlock(&session->lock);
}

void np_file2_stream_release(struct np_file2_stream *stream) {
    if (!stream) return;
    struct np_file2_session *session = stream->session;
    pthread_mutex_lock(&session->lock);
    int unfinished = !stream->cancelled && !session->error && !(stream->local_ended && stream->remote_ended);
    pthread_mutex_unlock(&session->lock);
    if (unfinished) np_file2_cancel(stream);
    pthread_mutex_lock(&session->lock);
    stream->referenced = 0;
    int freed = maybe_destroy(stream) && session->owner_closed;
    pthread_mutex_unlock(&session->lock);
    if (freed) free_session(session);
}

uint32_t np_file2_stream_id(const struct np_file2_stream *stream) { return stream->id; }
