/* common.h - theme reading and drawing shared by wmsliver and wmsliver-logout */
#ifndef COMMON_H
#define COMMON_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <Imlib2.h>

/* a theme value: a string, or the top level of a list like (tpixmap, file, color) */
typedef struct {
	char *v[8];
	int n;
} Val;

extern Display *dpy;
extern int scr, depth;
extern Visual *vis;
extern Colormap cmap;
extern GC gc;                   /* set by the program once it has a window */
extern const char *prog;        /* program name for messages, defined by each program */

void die(const char *msg);

/* config helpers */
char *expand(const char *s);
char *trim(char *s);
int yes(const char *v);

/* Window Maker theme */
void theme_init(void);
Val *tv(const char *key);
const char *ts(const char *key, const char *def);

/* textures are rendered into the Imlib2 context image; blit() puts it on a drawable and frees it */
void texture(Val *t, int w, int h, const char *fallback);
void imbevel(int w, int h);
void imresizebar(int w, int h);
void blit(Drawable d, int x, int y);

void color(XftColor *c, const char *name, const char *def);
XftFont *openfont(const char *key, const char *def);
void rect(Drawable d, XftColor *c, int x, int y, int w, int h);
void text(XftDraw *d, XftFont *f, XftColor *c, int x, int w, int y, int just, const char *s);

int grab(Window w);

#endif
