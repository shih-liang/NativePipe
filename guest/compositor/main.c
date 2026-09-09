#include "backend.h"
#include <string.h>

/* Lets the updater reject an old executable without accidentally starting it
 * as a compositor: older versions ignored unknown command-line options. */
__attribute__((used)) static const char runtime_probe[] = "NP_RUNTIME_PROBE:1";

int main(int argc, char **argv)
{
	/* The updater checks the guest's dynamic loader before replacing a working
	 * compositor. This mode creates no display, GPU device, socket or window. */
	if (argc == 2 && !strcmp(argv[1], "--check-runtime")) return 0;
	return np_backend_run(argc, argv);
}
