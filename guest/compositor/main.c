#include "backend.h"
#include "keymap.h"
#include <stdlib.h>
#include <string.h>

/* Lets the updater reject an old executable without accidentally starting it
 * as a compositor: older versions ignored unknown command-line options. */
__attribute__((used, retain)) static const char runtime_probe[] = "NP_RUNTIME_PROBE:1";

int main(int argc, char **argv)
{
	if (!np_backend_check_runtime()) return 1;
	/* Validate essential userspace data as well as the dynamic loader before
	 * replacing a working compositor. No display, GPU, socket or window. */
	if (argc == 2 && !strcmp(argv[1], "--check-runtime")) {
		char *keymap = np_keymap_text("us");
		if (!keymap) return 1;
		free(keymap);
		return 0;
	}
	return np_backend_run(argc, argv);
}
