#ifndef NP_APPLICATION_ICONS_H
#define NP_APPLICATION_ICONS_H
#include <gio/gio.h>
/* Resolves the current icon theme and returns a bounded PNG thumbnail. */
GBytes *np_application_icon(GIcon *icon);
char *np_application_icon_path(GIcon *icon);
void np_application_icons_init(void);
#endif
