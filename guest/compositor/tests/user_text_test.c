/* np_user_text takes its text from the environment, so the property that
 * matters most is that the text is never treated as a format: a stray
 * conversion in a translation must come out as characters, not as a read of
 * arguments that were never passed. */
#include "../user_text.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

bool np_trace_enabled(void) { return false; }

static const char *format(const char *text, const char *a, const char *b)
{
	static char out[256];
	np_user_text_format(out, sizeof(out), text, a, b);
	return out;
}

int main(void)
{
	assert(strcmp(format("Couldn’t start %s: %s.", "gtk4-demo", "No such file or directory"),
	              "Couldn’t start gtk4-demo: No such file or directory.") == 0);
	/* A translation may reorder its arguments. */
	assert(strcmp(format("%2$s: %1$s", "first", "second"), "second: first") == 0);
	/* Only %s, %1$s, %2$s and %% mean anything; everything else is literal. */
	assert(strcmp(format("100%% %d %n %x %p %s", "a", NULL), "100% %d %n %x %p a") == 0);
	/* Missing and surplus arguments are empty, never undefined. */
	assert(strcmp(format("[%s][%s][%s]", "a", NULL), "[a][][]") == 0);
	assert(strcmp(format(NULL, "a", "b"), "") == 0);
	assert(strcmp(format("%", "a", "b"), "%") == 0);
	assert(strcmp(format("%1$", "a", "b"), "%1$") == 0);

	/* Truncates safely and reports the length the whole result needs. */
	char small[8];
	size_t needed = np_user_text_format(small, sizeof(small), "abc%sdef", "XYZ", NULL);
	assert(needed == 9 && strcmp(small, "abcXYZd") == 0);
	assert(np_user_text_format(small, 0, "abc", NULL, NULL) == 3);

	/* The exported translation wins; an empty one falls back to English. */
	setenv("NATIVEPIPE_TEXT_TEST", "übersetzt %s", 1);
	np_user_text("TEST", "english %s", "x", NULL);
	setenv("NATIVEPIPE_TEXT_TEST", "", 1);
	np_user_text("TEST", "english %s", "x", NULL);

	puts("user text: ok");
	return 0;
}
