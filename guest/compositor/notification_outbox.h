#ifndef NP_NOTIFICATION_OUTBOX_H
#define NP_NOTIFICATION_OUTBOX_H

#include "windowwire.h"
#include <stdlib.h>
#include <string.h>

#define NP_NOTIFICATION_OUTBOX_IDS 64u
#define NP_NOTIFICATION_OUTBOX_BYTES (512u * 1024u)
#define NP_NOTIFICATION_OUTBOX_PAYLOAD 8192u

/* Only optional desktop notifications use this queue. A selected NPIP record
 * is pinned until its last byte is written; replacing a partial record would
 * corrupt the same stream that carries authoritative window state. */
struct np_notification_frame {
    struct np_notification_frame *next;
    uint32_t id;
    uint64_t revision;
    size_t size, offset;
    unsigned char bytes[];
};
struct np_notification_outbox {
    struct np_notification_frame *posted, *closed, *active;
    unsigned posted_count, closed_count;
    size_t bytes, reset_offset;
    bool reset, active_reset;
};

static inline bool np_notification_payload(const void *payload, size_t length)
{
    const unsigned char *p = payload;
    return p && length >= 8 && !memcmp(p, "NPW2\1", 5) &&
        (p[5] == NP_GUEST_NOTIFICATION_POSTED || p[5] == NP_GUEST_NOTIFICATION_CLOSED ||
         p[5] == NP_GUEST_NOTIFICATION_BACKLOG_RESET);
}
static inline void np_notification_free_list(struct np_notification_outbox *q,
    struct np_notification_frame **head, unsigned *count)
{
    while (*head) {
        struct np_notification_frame *next = (*head)->next;
        q->bytes -= (*head)->size;
        free(*head); *head = next;
    }
    *count = 0;
}
static inline void np_notification_outbox_clear(struct np_notification_outbox *q)
{
    np_notification_free_list(q, &q->posted, &q->posted_count);
    np_notification_free_list(q, &q->closed, &q->closed_count);
    free(q->active);
    memset(q, 0, sizeof(*q));
}
static inline struct np_notification_frame **np_notification_find(
    struct np_notification_frame **head, uint32_t id)
{
    while (*head && (*head)->id != id) head = &(*head)->next;
    return head;
}
static inline void np_notification_remove(struct np_notification_outbox *q,
    struct np_notification_frame **item, unsigned *count)
{
    struct np_notification_frame *old = *item;
    *item = old->next;
    q->bytes -= old->size; --*count;
    free(old);
}
static inline bool np_notification_outbox_send(struct np_notification_outbox *q,
    const void *payload, size_t length)
{
    const unsigned char *p = payload;
    if (!np_notification_payload(payload, length) || p[6] || p[7]) return false;
    if (p[5] == NP_GUEST_NOTIFICATION_BACKLOG_RESET) {
        if (length != 8) return false;
        q->reset = true; /* A fixed slot, including when malloc fails. */
        return true;
    }
    if (length < 20 || length > NP_NOTIFICATION_OUTBOX_PAYLOAD) return false;
    uint32_t id; uint64_t revision;
    memcpy(&id, p + 8, sizeof(id)); memcpy(&revision, p + 12, sizeof(revision));
    if (!id || !revision || (p[5] == NP_GUEST_NOTIFICATION_CLOSED && length != 20)) return false;
    bool closing = p[5] == NP_GUEST_NOTIFICATION_CLOSED;
    struct np_notification_frame **post = np_notification_find(&q->posted, id);
    struct np_notification_frame **close = np_notification_find(&q->closed, id);
    if ((q->active && q->active->id == id && q->active->revision > revision) ||
        (*post && (*post)->revision > revision) || (*close && (*close)->revision > revision)) return true;
    if (*post) np_notification_remove(q, post, &q->posted_count);
    if (*close) np_notification_remove(q, close, &q->closed_count);
    if (closing && q->closed_count == NP_NOTIFICATION_OUTBOX_IDS) {
        /* Historical Close churn has no live guest ID to acknowledge. One
         * reset retires lost old banners without discarding any active post. */
        np_notification_free_list(q, &q->closed, &q->closed_count);
        q->reset = true;
    }
    size_t size = 12 + length;
    unsigned *count = closing ? &q->closed_count : &q->posted_count;
    if (*count == NP_NOTIFICATION_OUTBOX_IDS || size > NP_NOTIFICATION_OUTBOX_BYTES - q->bytes) {
        q->reset = true;
        return closing; /* Posts are refused, then retired by notifications.c. */
    }
    struct np_notification_frame *f = malloc(sizeof(*f) + size);
    if (!f) { q->reset = true; return closing; }
    *f = (struct np_notification_frame){ .id = id, .revision = revision, .size = size };
    memcpy(f->bytes, "NPIP\1\0\0\0", 8);
    uint32_t payload_size = (uint32_t)length;
    memcpy(f->bytes + 8, &payload_size, 4); memcpy(f->bytes + 12, payload, length);
    struct np_notification_frame **head = closing ? &q->closed : &q->posted;
    while (*head) head = &(*head)->next;
    *head = f; ++*count; q->bytes += size;
    return true;
}
static inline bool np_notification_outbox_pending(const struct np_notification_outbox *q)
{
    return q->posted || q->closed || q->active || q->reset || q->active_reset;
}
static inline bool np_notification_outbox_started(const struct np_notification_outbox *q)
{
    return (q->active && q->active->offset) || (q->active_reset && q->reset_offset);
}
static inline const unsigned char *np_notification_outbox_next(
    struct np_notification_outbox *q, size_t *length)
{
    static const unsigned char reset[] = {
        'N','P','I','P',1,0,0,0,8,0,0,0,'N','P','W','2',1,NP_GUEST_NOTIFICATION_BACKLOG_RESET,0,0
    };
    if (!q->active && !q->active_reset) {
        if (q->reset) { q->active_reset = true; q->reset = false; q->reset_offset = 0; }
        else {
            struct np_notification_frame **head = q->closed ? &q->closed : &q->posted;
            unsigned *count = q->closed ? &q->closed_count : &q->posted_count;
            if (!*head) { *length = 0; return NULL; }
            q->active = *head; *head = (*head)->next; --*count;
        }
    }
    if (q->active_reset) { *length = sizeof(reset) - q->reset_offset; return reset + q->reset_offset; }
    *length = q->active->size - q->active->offset;
    return q->active->bytes + q->active->offset;
}
static inline void np_notification_outbox_consume(struct np_notification_outbox *q, size_t count)
{
    if (q->active_reset) {
        q->reset_offset += count;
        if (q->reset_offset == 20) { q->active_reset = false; q->reset_offset = 0; }
    } else {
        q->active->offset += count;
        if (q->active->offset == q->active->size) {
            q->bytes -= q->active->size;
            free(q->active); q->active = NULL;
        }
    }
}
#endif
