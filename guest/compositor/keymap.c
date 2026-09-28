#include "keymap.h"
#include <stdio.h>
#include <xkbcommon/xkbcommon.h>

char *np_keymap_text(const char *layout)
{
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .model = "pc105", .layout = layout };
    struct xkb_keymap *keymap = context
        ? xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    if (!text)
        fprintf(stderr, "[wayland] cannot compile XKB keymap '%s'. Install xkb-data "
            "(Debian/Ubuntu) or xkeyboard-config (Arch/Alpine), and check XKB_CONFIG_ROOT.\n", layout);
    return text;
}
