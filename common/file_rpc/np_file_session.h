#ifndef NP_FILE_SESSION_H
#define NP_FILE_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* NPFR v2: one long-lived, full-duplex connection carrying many concurrent
 * streams. Either side opens streams and sends notifications; the connection's
 * direction only decides stream ID parity. See README.md "NPFR v2". */
#define NP_FILE2_VERSION 2
#define NP_FILE2_HEADER 16u
#define NP_FILE2_FRAME_MAX 65536u
#define NP_FILE2_STREAM_WINDOW (256u * 1024u)
#define NP_FILE2_CONNECTION_WINDOW (4u * 1024u * 1024u)
#define NP_FILE2_MAX_INBOUND_STREAMS 256u
/* Frames the reader originates (credit, refusals) wait here while the peer is
 * not reading; a peer that lets this many accumulate is dropped (ENOBUFS). */
#define NP_FILE2_MAX_PENDING_CONTROL 65536u
#define NP_FILE2_PORT 1031u

enum np_file2_type {
    NP_FILE2_HELLO = 1, NP_FILE2_REQUEST, NP_FILE2_RESPONSE, NP_FILE2_DATA,
    NP_FILE2_NOTIFY, NP_FILE2_CANCEL, NP_FILE2_CREDIT
};
#define NP_FILE2_END_STREAM 1u
/* Host-opened streams are even, guest-opened streams odd; 0 is the connection. */
enum np_file2_role { NP_FILE2_ROLE_HOST = 0, NP_FILE2_ROLE_GUEST = 1 };

struct np_file2_session;
struct np_file2_stream;

/* Callbacks run on the session's reader thread and must not block or call
 * np_file2_session_close: hand work to another thread. `request` receives an
 * owned stream reference that the callee releases with np_file2_stream_release.
 * Bodies are only valid during the call. `closed` runs once, after which no callback is made. */
struct np_file2_handlers {
    void *context;
    void (*request)(void *context, struct np_file2_stream *stream, uint16_t opcode,
                    const void *body, size_t length);
    void (*notify)(void *context, uint16_t opcode, const void *body, size_t length);
    void (*closed)(void *context, int error);
};

struct np_file2_message {
    uint8_t type;   /* NP_FILE2_RESPONSE or NP_FILE2_DATA */
    uint8_t end;    /* the peer finished its half with this message */
    uint32_t status;
    uint32_t length;
    unsigned char data[NP_FILE2_FRAME_MAX];
};
/* Large C arrays are not imported as Swift tuples. */
static inline unsigned char *np_file2_message_data(struct np_file2_message *message) { return message->data; }

/* Takes ownership of the connected socket, including on failure. */
struct np_file2_session *np_file2_session_start(int socket, enum np_file2_role role,
                                                const struct np_file2_handlers *handlers);
/* Disconnects, fails every stream with ECONNRESET and joins the reader. Streams
 * still referenced by the caller stay valid until released. */
void np_file2_session_close(struct np_file2_session *session);

int np_file2_notify(struct np_file2_session *session, uint16_t opcode,
                    const void *body, size_t length);

/* Opens a stream. The body travels in the REQUEST frame (at most
 * NP_FILE2_FRAME_MAX - 4 bytes); larger payloads follow as DATA. */
struct np_file2_stream *np_file2_request(struct np_file2_session *session, uint16_t opcode,
                                         const void *body, size_t length, int end_stream);
/* Responder only, exactly once, before any DATA. */
int np_file2_respond(struct np_file2_stream *stream, uint32_t status,
                     const void *body, size_t length, int end_stream);
/* Blocks for flow-control credit; splits into frames. Empty + end finishes. */
int np_file2_send(struct np_file2_stream *stream, const void *data, size_t length, int end_stream);
/* Returns 1 with a message, 0 once the peer's half has ended, -1 with errno
 * (ECANCELED after either side cancelled, ECONNRESET after disconnect). */
int np_file2_receive(struct np_file2_stream *stream, struct np_file2_message *message);
/* Abandons both directions. Idempotent and non-blocking; wakes blocked
 * senders and receivers. */
void np_file2_cancel(struct np_file2_stream *stream);
/* Releasing a stream that has not finished both directions cancels it.
 * Never blocks, so it is safe inside callbacks. */
void np_file2_stream_release(struct np_file2_stream *stream);
uint32_t np_file2_stream_id(const struct np_file2_stream *stream);

#endif
