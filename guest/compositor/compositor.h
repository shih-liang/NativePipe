#ifndef NP_COMPOSITOR_H
#define NP_COMPOSITOR_H

/// Shared compositor entry used by both vmpipe-wayland and remotepipe-wayland.
/// Transport differences are selected at compile time (`-DNP_REMOTE`) and by
/// which objects are linked (blob/dmabuf vs medialink/encoder).
int np_compositor_run(int argc, char **argv);

#endif
