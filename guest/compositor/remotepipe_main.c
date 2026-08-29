#include "compositor.h"

/// Bare-metal remote compositor: TCP NPIP/NPEN + H.264 encode.
/// No VM or virtio-gpu objects are linked.
int main(int argc, char **argv) {
	return np_compositor_run(argc, argv);
}
