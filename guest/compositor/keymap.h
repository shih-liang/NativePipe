#ifndef NP_KEYMAP_H
#define NP_KEYMAP_H

/* Caller frees the serialized keymap. No display, device or session is opened. */
char *np_keymap_text(const char *layout);

#endif
