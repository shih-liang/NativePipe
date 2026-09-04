#ifndef NP_COMPOSITOR_H
#define NP_COMPOSITOR_H

/// Backend-independent Wayland frontend.  Exactly one linked backend owns the
/// opaque state and calls this after completing its process-level setup.
int np_frontend_run(int argc, char **argv, void *backend_state);

#endif
