#include "user_text.h"

#include <stdlib.h>
#include <string.h>

static void append(char *out, size_t size, size_t *length, const char *text, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (*length + 1 < size) out[*length] = text[i];
		(*length)++;
	}
}

size_t np_user_text_format(char *out, size_t size, const char *text,
                           const char *first, const char *second)
{
	const char *arguments[2] = { first ? first : "", second ? second : "" };
	size_t length = 0, next = 0;
	for (const char *p = text ? text : ""; *p; p++) {
		if (p[0] == '%' && p[1] == 's') {
			const char *value = next < 2 ? arguments[next] : "";
			next++;
			append(out, size, &length, value, strlen(value));
			p += 1;
		} else if (p[0] == '%' && (p[1] == '1' || p[1] == '2') && p[2] == '$' && p[3] == 's') {
			/* Positional, so a translation may put the arguments in another order. */
			const char *value = arguments[p[1] - '1'];
			append(out, size, &length, value, strlen(value));
			p += 3;
		} else if (p[0] == '%' && p[1] == '%') {
			append(out, size, &length, "%", 1);
			p += 1;
		} else {
			append(out, size, &length, p, 1);
		}
	}
	if (size) out[length < size ? length : size - 1] = '\0';
	return length;
}

void np_user_text(const char *name, const char *english,
                  const char *first, const char *second)
{
	char variable[96];
	snprintf(variable, sizeof(variable), "NATIVEPIPE_TEXT_%s", name);
	const char *text = getenv(variable);
	if (!text || !text[0]) text = english;
	char line[1024];
	np_user_text_format(line, sizeof(line), text, first, second);
	fprintf(stderr, "nativepipe-wayland: %s\n", line);
}
