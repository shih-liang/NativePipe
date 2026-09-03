#ifndef NP_COMPOSITOR_H
#define NP_COMPOSITOR_H

/// Shared Wayland compositor entry. The selected backend supplies transport
/// and frame-publication policy at link time.
int np_compositor_run(int argc, char **argv);

#endif
