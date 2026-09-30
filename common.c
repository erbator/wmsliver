/* common.c - theme reading and drawing shared by wmsliver and wmsliver-logout */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <Imlib2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "common.h"

Display *dpy;
int scr, depth;
Visual *vis;
Colormap cmap;
GC gc;

void
die(const char *msg)
{
	fprintf(stderr, "%s: %s\n", prog, msg);
	exit(1);
}

/* ~/foo -> $HOME/foo, newly allocated */
char *
expand(const char *s)
{
	const char *home = getenv("HOME");
	char *r;

	if (s[0] != '~' || (s[1] && s[1] != '/'))
		return strdup(s);
	if (!(r = malloc(strlen(home ? home : "") + strlen(s))))
		die("out of memory");
	sprintf(r, "%s%s", home ? home : "", s + 1);
	return r;
}

char *
trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); e--)
		;
	*e = '\0';
	return s;
}

int
yes(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") ||
	       !strcmp(v, "1");
}

/* ---------- theme ---------- */

/* the theme keys we use, and their values */
static const char *keys[] = {
	"WorkspaceBack", "FTitleBack", "FTitleColor", "ResizebarBack",
	"MenuTextBack", "MenuTextColor", "MenuTextFont", "MenuTitleFont",
	"WindowTitleFont", "TitleJustify", "FrameBorderWidth",
	"FrameBorderColor", "FrameFocusedBorderColor", "FrameSelectedBorderColor",
	"IconBack", "IconTitleBack", "HighlightTextColor", "PixmapPath",
	"MenuTitleBack", "MenuTitleColor", "HighlightColor", "MenuStyle",
};
#define NTHEME  (int)(sizeof keys / sizeof keys[0])
static Val vals[NTHEME];

Val *
tv(const char *key)
{
	for (int i = 0; i < NTHEME; i++)
		if (strcmp(keys[i], key) == 0)
			return vals[i].n ? &vals[i] : NULL;
	return NULL;
}

const char *
ts(const char *key, const char *def)
{
	Val *v = tv(key);
	return v ? v->v[0] : def;
}

/* property list tokenizer: returns 's' for a string/word, a punctuation char, or 0 at EOF */
static int
lex(const char **p, char *out, size_t max)
{
	const char *s = *p;
	size_t n = 0;

	for (;;) {
		while (isspace((unsigned char)*s))
			s++;
		if (s[0] == '/' && s[1] == '/') {
			while (*s && *s != '\n')
				s++;
		} else if (s[0] == '/' && s[1] == '*') {
			for (s += 2; *s && !(s[0] == '*' && s[1] == '/'); s++)
				;
			if (*s)
				s += 2;
		} else {
			break;
		}
	}
	if (!*s) {
		*p = s;
		return 0;
	}
	if (strchr("{}()=;,", *s)) {
		*p = s + 1;
		return *s;
	}
	if (*s == '"') {
		for (s++; *s && *s != '"'; s++) {
			if (*s == '\\' && s[1])
				s++;
			if (n + 1 < max)
				out[n++] = *s;
		}
		if (*s)
			s++;
	} else {
		for (; *s && !isspace((unsigned char)*s) && !strchr("{}()=;,\"", *s); s++)
			if (n + 1 < max)
				out[n++] = *s;
	}
	out[n] = '\0';
	*p = s;
	return 's';
}

/* read a WindowMaker defaults file; keys found override earlier ones */
static void
theme_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char key[256], v[1024], *text;
	const char *p;
	long len;
	int t, d;

	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	if (len <= 0 || !(text = calloc(1, len + 1)) || fread(text, 1, len, f) != (size_t)len) {
		fclose(f);
		return;
	}
	fclose(f);

	p = text;
	if (lex(&p, key, sizeof key) != '{')
		goto out;
	while (lex(&p, key, sizeof key) == 's') {
		Val val = { 0 };

		if (lex(&p, v, sizeof v) != '=')
			break;
		t = lex(&p, v, sizeof v);
		if (t == 's') {
			val.v[val.n++] = strdup(v);
		} else if (t == '(' || t == '{') {
			/* flatten the top level of a list, skip anything nested */
			for (d = 1; d && (t = lex(&p, v, sizeof v)); ) {
				if (t == '(' || t == '{')
					d++;
				else if (t == ')' || t == '}')
					d--;
				else if (t == 's' && d == 1 && val.n < 8)
					val.v[val.n++] = strdup(v);
			}
		}
		lex(&p, v, sizeof v);   /* ';' */

		for (int i = 0; i < NTHEME; i++) {
			if (strcmp(keys[i], key) != 0)
				continue;
			for (int j = 0; j < vals[i].n; j++)
				free(vals[i].v[j]);
			vals[i] = val;
			val.n = 0;
		}
		for (int j = 0; j < val.n; j++)
			free(val.v[j]);
	}
out:
	free(text);
}

/* expand ~ and search PixmapPath */
static int
findpix(const char *name, char *out, size_t n)
{
	const char *home = getenv("HOME");
	Val *path = tv("PixmapPath");
	char dir[2048];

	if (name[0] == '~' && name[1] == '/') {
		snprintf(out, n, "%s%s", home ? home : "", name + 1);
		return access(out, R_OK) == 0;
	}
	if (name[0] == '/') {
		snprintf(out, n, "%s", name);
		return access(out, R_OK) == 0;
	}
	for (int i = 0; path && i < path->n; i++) {
		if (path->v[i][0] == '~')
			snprintf(dir, sizeof dir, "%s%s", home ? home : "", path->v[i] + 1);
		else
			snprintf(dir, sizeof dir, "%s", path->v[i]);
		snprintf(out, n, "%s/%s", dir, name);
		if (access(out, R_OK) == 0)
			return 1;
	}
	return 0;
}

static DATA32
rgb(const char *name)
{
	XColor c;

	if (!name || !XParseColor(dpy, cmap, name, &c))
		return 0xff000000;
	return 0xff000000 | (c.red >> 8) << 16 | (c.green >> 8) << 8 | c.blue >> 8;
}

static DATA32
mix(DATA32 a, DATA32 b, double t)
{
	int r = ((a >> 16) & 0xff) + (((int)((b >> 16) & 0xff) - (int)((a >> 16) & 0xff)) * t);
	int g = ((a >> 8) & 0xff) + (((int)((b >> 8) & 0xff) - (int)((a >> 8) & 0xff)) * t);
	int l = (a & 0xff) + (((int)(b & 0xff) - (int)(a & 0xff)) * t);
	return 0xff000000 | r << 16 | g << 8 | l;
}

static DATA32
shade(DATA32 c, int amount)
{
	int r = ((c >> 16) & 0xff) + amount, g = ((c >> 8) & 0xff) + amount, b = (c & 0xff) + amount;
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return 0xff000000 | r << 16 | g << 8 | b;
}

/* fill the context image with a gradient through `nc` colors; dir is 'h', 'v' or 'd' */
static void
gradient(int w, int h, char dir, DATA32 *cols, int nc)
{
	DATA32 *d = imlib_image_get_data();
	double mw = w > 1 ? w - 1 : 1, mh = h > 1 ? h - 1 : 1;

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double t = dir == 'h' ? x / mw : dir == 'v' ? y / mh : (x / mw + y / mh) / 2;
			double s = t * (nc - 1);
			int i = s >= nc - 1 ? nc - 2 : (int)s;
			d[y * w + x] = nc < 2 ? cols[0] : mix(cols[i], cols[i + 1], s - i);
		}
	imlib_image_put_back_data(d);
}

/* render a Window Maker texture spec, e.g. (tpixmap, file, color), as a w x h
 * image, which is left as the Imlib context image */
void
texture(Val *t, int w, int h, const char *fallback)
{
	char type[32] = "solid", path[4096];
	Imlib_Image im = imlib_create_image(w, h), src;
	DATA32 cols[8];
	int nc = 0, iw, ih, solid;

	if (t)
		for (int i = 0; i < 31 && t->v[0][i]; i++)
			type[i] = tolower((unsigned char)t->v[0][i]), type[i + 1] = '\0';
	solid = !strcmp(type, "solid");

	imlib_context_set_image(im);
	imlib_image_set_has_alpha(0);

	if (t && strstr(type, "gradient")) {
		/* [m][hvd]gradient: colors from index 1; t[hvd]gradient: (type, file, opacity, from, to) */
		for (int i = type[0] == 't' ? 3 : 1; i < t->n && nc < 8; i++)
			cols[nc++] = rgb(t->v[i]);
		if (nc) {
			gradient(w, h, (type[0] == 't' || type[0] == 'm') ? type[1] : type[0], cols, nc);
			return;
		}
	}

	/* solid color, or the color behind a pixmap */
	cols[0] = rgb(t && t->n > (solid ? 1 : 2) ? t->v[solid ? 1 : 2] : fallback);
	gradient(w, h, 'h', cols, 1);

	if (!t || t->n < 2 || !strstr(type, "pixmap") ||
	    !findpix(t->v[1], path, sizeof path) || !(src = imlib_load_image(path)))
		return;

	imlib_context_set_image(src);
	iw = imlib_image_get_width();
	ih = imlib_image_get_height();
	imlib_context_set_image(im);
	imlib_context_set_blend(1);

	switch (type[0]) {
	case 't':       /* tiled */
		for (int y = 0; y < h; y += ih)
			for (int x = 0; x < w; x += iw)
				imlib_blend_image_onto_image(src, 0, 0, 0, iw, ih, x, y, iw, ih);
		break;
	case 's':       /* scaled */
		imlib_blend_image_onto_image(src, 0, 0, 0, iw, ih, 0, 0, w, h);
		break;
	case 'c':       /* centered */
		imlib_blend_image_onto_image(src, 0, 0, 0, iw, ih, (w - iw) / 2, (h - ih) / 2, iw, ih);
		break;
	case 'f': {     /* scaled to fill, keeping aspect: crop the source */
		double s = (double)w / iw > (double)h / ih ? (double)w / iw : (double)h / ih;
		int sw = w / s, sh = h / s;
		imlib_blend_image_onto_image(src, 0, (iw - sw) / 2, (ih - sh) / 2, sw, sh, 0, 0, w, h);
		break;
	}
	case 'm': {     /* maximized, keeping aspect: letterbox */
		double s = (double)w / iw < (double)h / ih ? (double)w / iw : (double)h / ih;
		int dw = iw * s, dh = ih * s;
		imlib_blend_image_onto_image(src, 0, 0, 0, iw, ih, (w - dw) / 2, (h - dh) / 2, dw, dh);
		break;
	}
	}
	imlib_context_set_image(src);
	imlib_free_image();
	imlib_context_set_image(im);
}

/* wmaker's raised bevel on the context image: light top/left, dark then black
 * bottom/right */
void
imbevel(int w, int h)
{
	DATA32 *d = imlib_image_get_data();

	for (int x = 0; x < w; x++)
		d[x] = shade(d[x], 80);
	for (int y = 1; y < h; y++)
		d[y * w] = shade(d[y * w], 80);
	for (int x = 0; x < w; x++) {
		d[(h - 2) * w + x] = shade(d[(h - 2) * w + x], -40);
		d[(h - 1) * w + x] = 0xff000000;
	}
	for (int y = 0; y < h; y++) {
		d[y * w + w - 2] = shade(d[y * w + w - 2], -40);
		d[y * w + w - 1] = 0xff000000;
	}
	imlib_image_put_back_data(d);
}

/* wmaker's resizebar on the context image: black line on top, then a light
 * edge, and grips 28px in from each end */
void
imresizebar(int w, int h)
{
	DATA32 *d = imlib_image_get_data();

	for (int x = 0; x < w; x++) {
		d[x] = 0xff000000;
		d[w + x] = shade(d[w + x], 80);
	}
	for (int y = 1; y < h; y++) {
		d[y * w + 28] = d[y * w + w - 30] = 0xff000000;
		d[y * w + 29] = shade(d[y * w + 29], 80);
		d[y * w + w - 29] = shade(d[y * w + w - 29], 80);
	}
	imlib_image_put_back_data(d);
}

/* put the context image on a drawable and free it */
void
blit(Drawable d, int x, int y)
{
	imlib_context_set_drawable(d);
	imlib_render_image_on_drawable(x, y);
	imlib_free_image();
}

void
color(XftColor *c, const char *name, const char *def)
{
	if (!name || !XftColorAllocName(dpy, vis, cmap, name, c))
		if (!XftColorAllocName(dpy, vis, cmap, def, c))
			die("cannot allocate color");
}

XftFont *
openfont(const char *key, const char *def)
{
	XftFont *f = XftFontOpenName(dpy, scr, ts(key, def));
	if (!f && !(f = XftFontOpenName(dpy, scr, def)))
		die("cannot open font");
	return f;
}

/* default visual, theme files, Imlib2 context; call after opening the display */
void
theme_init(void)
{
	const char *home = getenv("HOME");
	char rc[4096];

	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	depth = DefaultDepth(dpy, scr);

	theme_load("/etc/WindowMaker/WindowMaker");
	if (getenv("GNUSTEP_USER_ROOT"))
		snprintf(rc, sizeof rc, "%s/Defaults/WindowMaker", getenv("GNUSTEP_USER_ROOT"));
	else
		snprintf(rc, sizeof rc, "%s/GNUstep/Defaults/WindowMaker", home ? home : "");
	theme_load(rc);

	imlib_context_set_display(dpy);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cmap);
}

void
rect(Drawable d, XftColor *c, int x, int y, int w, int h)
{
	XSetForeground(dpy, gc, c->pixel);
	XFillRectangle(dpy, d, gc, x, y, w, h);
}

/* text in the box x..x+w, baseline y; just: 0 left, 1 center, 2 right */
void
text(XftDraw *d, XftFont *f, XftColor *c, int x, int w, int y, int just, const char *s)
{
	XGlyphInfo gi;

	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &gi);
	x += just == 2 ? w - gi.xOff : just == 1 ? (w - gi.xOff) / 2 : 0;
	XftDrawStringUtf8(d, c, f, x, y, (const FcChar8 *)s, strlen(s));
}

/* grab keyboard and pointer for w; returns 0 if someone else keeps them */
int
grab(Window w)
{
	int kb = 0, ptr = 0;

	/* another client may hold a grab briefly (menus, key bindings); retry ~1s */
	for (int i = 0; i < 100 && !(kb && ptr); i++) {
		if (!ptr)
			ptr = XGrabPointer(dpy, w, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
			                   GrabModeAsync, GrabModeAsync, None, None,
			                   CurrentTime) == GrabSuccess;
		if (!kb)
			kb = XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync,
			                   CurrentTime) == GrabSuccess;
		if (!(kb && ptr))
			usleep(10000);
	}
	if (!(kb && ptr)) {
		XUngrabKeyboard(dpy, CurrentTime);
		XUngrabPointer(dpy, CurrentTime);
		return 0;
	}
	return 1;
}
