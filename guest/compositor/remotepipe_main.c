#include "compositor.h"

/// Bare-metal remote compositor: TCP NPIP/NPEN + H.264 encode.
/// Built with -DNP_REMOTE; does not link virtio blob/dmabuf objects.
int main(int argc, char **argv) {
	return np_compositor_run(argc, argv);
}
