#include "compositor.h"

/// LightHouse VM compositor: vsock + virtio-gpu blobs / Venus.
int main(int argc, char **argv) {
	return np_compositor_run(argc, argv);
}
