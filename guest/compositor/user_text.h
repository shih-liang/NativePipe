#ifndef NP_USER_TEXT_H
#define NP_USER_TEXT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* What the compositor writes to stderr reaches the person at the Mac terminal,
 * so it splits in two.
 *
 * np_user_text is for that person, in their language. The compositor never
 * picks a language itself -- the Linux account's locale need not be the Mac
 * user's, and it ships no message catalog -- so the Mac exports each message,
 * already translated, as NATIVEPIPE_TEXT_<name> before starting it. Without
 * that variable (an older client, LinPortal, a manual run) the English text is
 * used. "%s" takes the next argument and "%1$s"/"%2$s" a specific one; nothing
 * else is interpreted. The text comes from the environment, not from this
 * program, so it must never become a printf format.
 *
 * np_debug_log is for whoever is debugging, with NP_TRACE set. Internals such
 * as socket names, frame counters and EGL errors are noise in a user's terminal
 * and mean nothing translated, so they stay English and off by default. */
void np_user_text(const char *name, const char *english,
                  const char *first, const char *second);

/* The substitution behind np_user_text, into a buffer. Returns the length the
 * full result needs, like snprintf. */
size_t np_user_text_format(char *out, size_t size, const char *text,
                           const char *first, const char *second);

bool np_trace_enabled(void);

static inline void np_debug_log(const char *format, ...)
	__attribute__((format(printf, 1, 2)));
static inline void np_debug_log(const char *format, ...)
{
	if (!np_trace_enabled()) return;
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

#endif
