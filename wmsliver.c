/* wmsliver - a Window Maker screen locker that wears your theme.
 *
 * See README.md for usage and the config file format.
 *
 * Look: follows the active Window Maker theme. Theme keys are read from
 * /etc/WindowMaker/WindowMaker, then $GNUSTEP_USER_ROOT/Defaults/WindowMaker
 * (default ~/GNUstep) on top. The lock panel is drawn as a focused window
 * (FTitleBack, ResizebarBack, frame border colors, title font and
 * justification) with a MenuTextBack body, dock tiles use IconBack, and the
 * background is the current wallpaper (_XROOTPMAP_ID) or WorkspaceBack.
 *
 * Dockapps: we start a private copy of each configured dockapp and swallow its
 * icon window into the lock window. Copies already in your Window Maker dock
 * are never touched (moving those out and back confuses wmaker). The copies
 * get no input because we hold the keyboard/pointer grab, and are killed on
 * unlock.
 *
 * Power: while locked we hold power-profiles-daemon's "power-saver" profile and
 * set the previous profile back on unlock. If we die, the daemon drops the
 * hold with our bus connection, so the machine can't get stuck in power-saver.
 */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <Imlib2.h>
#include <security/pam_appl.h>
#ifdef WITH_POWER
#include <systemd/sd-bus.h>
#endif
#include <ctype.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/select.h>

#define AV      128             /* avatar size */
#define PW      300             /* panel inner width */
#define BH      242             /* panel body height */
#define RH      8               /* resizebar height, including the line above it */
#define TILE    64              /* dock tile size */
#define MAXDOCK 8               /* max dockapps */
#define MAXARG  16              /* max argv entries per dockapp, including NULL */

#ifndef VERSION
#define VERSION "dev"
#endif

/* settings; defaults here, overridden by the config file, then the command line */
static char *avatar;                            /* NULL: ~/.face, then ~/.face.icon */
static char *title = "Screen Locked";
static char *errcolor = "#e05050";
static char dockpos[16] = "bottom-left";        /* bottom-left/-right, top-left/-right */
static int usepower = 1;
static char *dockargv[MAXDOCK][MAXARG];
static int ndock;

static Display *dpy;
static int scr;
static Window win;
static Visual *vis;
static Colormap cmap;
static int depth;
static Pixmap frame, buf, tilepm;
static GC gc;
static XftDraw *xd;
static XftFont *tfont, *font, *bfont;
static XftColor cborder, ctitle, ctext, cfield, cfieldbd, cfieldtx, cerr;
static int fw, fh, bw;          /* whole panel size, frame border width */
static int th;                  /* titlebar height */
static int px, py;              /* panel position */
static int dx, dy;              /* dock position */
static const char *user;

static struct {
	pid_t pid;
	Window icon;            /* swallowed icon window, 0 until found */
	Window seen[16];        /* windows of this class that existed before we spawned */
	int nseen;
} dock[MAXDOCK];
static long dockstart;          /* ms timestamp of spawn */
#ifdef WITH_POWER
static sd_bus *bus;             /* system bus, holds the power-saver profile */
static char *prevprofile;       /* profile to restore on unlock */
#endif
static volatile sig_atomic_t quit;

static char pass[256];
static int plen;
static const char *status = "";

static void
die(const char *msg)
{
	fprintf(stderr, "wmsliver: %s\n", msg);
	exit(1);
}

/* ---------- config ---------- */

/* ~/foo -> $HOME/foo, newly allocated */
static char *
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

static char *
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

static int
yes(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") ||
	       !strcmp(v, "1");
}

/* key = value lines; '#' starts a comment only at the beginning of a line,
 * so colors like #e05050 work. Mistakes are reported but never stop the lock. */
static int
config_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024], *k, *v, *eq;
	int ln = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		ln++;
		k = trim(line);
		if (!*k || *k == '#')
			continue;
		if (!(eq = strchr(k, '='))) {
			fprintf(stderr, "wmsliver: %s:%d: expected key = value\n", path, ln);
			continue;
		}
		*eq = '\0';
		k = trim(k);
		v = trim(eq + 1);

		if (!strcmp(k, "avatar")) {
			avatar = expand(v);
		} else if (!strcmp(k, "title")) {
			title = strdup(v);
		} else if (!strcmp(k, "error_color")) {
			errcolor = strdup(v);
		} else if (!strcmp(k, "powersave")) {
			usepower = yes(v);
		} else if (!strcmp(k, "dock")) {
			if (strcmp(v, "bottom-left") && strcmp(v, "bottom-right") &&
			    strcmp(v, "top-left") && strcmp(v, "top-right"))
				fprintf(stderr, "wmsliver: %s:%d: dock must be bottom-left, "
				        "bottom-right, top-left or top-right\n", path, ln);
			else
				snprintf(dockpos, sizeof dockpos, "%s", v);
		} else if (!strcmp(k, "dockapp")) {
			if (ndock == MAXDOCK) {
				fprintf(stderr, "wmsliver: %s:%d: at most %d dockapps\n", path, ln, MAXDOCK);
				continue;
			}
			int n = 0;
			for (char *t = strtok(v, " \t"); t && n < MAXARG - 1; t = strtok(NULL, " \t"))
				dockargv[ndock][n++] = expand(t);
			if (n)
				ndock++;
		} else {
			fprintf(stderr, "wmsliver: %s:%d: unknown key '%s'\n", path, ln, k);
		}
	}
	fclose(f);
	return 1;
}

static void
usage(void)
{
	fputs("usage: wmsliver [-c config] [-a avatar] [-v]\n", stderr);
	exit(1);
}

/* ---------- theme ---------- */

typedef struct {
	char *v[8];
	int n;
} Val;

/* the theme keys we use, and their values */
static const char *keys[] = {
	"WorkspaceBack", "FTitleBack", "FTitleColor", "ResizebarBack",
	"MenuTextBack", "MenuTextColor", "MenuTextFont", "MenuTitleFont",
	"WindowTitleFont", "TitleJustify", "FrameBorderWidth",
	"FrameBorderColor", "FrameFocusedBorderColor", "FrameSelectedBorderColor",
	"IconBack", "IconTitleBack", "HighlightTextColor", "PixmapPath",
};
#define NTHEME  (int)(sizeof keys / sizeof keys[0])
static Val vals[NTHEME];

static Val *
tv(const char *key)
{
	for (int i = 0; i < NTHEME; i++)
		if (strcmp(keys[i], key) == 0)
			return vals[i].n ? &vals[i] : NULL;
	return NULL;
}

static const char *
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
static void
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
static void
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
static void
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
static void
blit(Drawable d, int x, int y)
{
	imlib_context_set_drawable(d);
	imlib_render_image_on_drawable(x, y);
	imlib_free_image();
}

static void
color(XftColor *c, const char *name, const char *def)
{
	if (!name || !XftColorAllocName(dpy, vis, cmap, name, c))
		if (!XftColorAllocName(dpy, vis, cmap, def, c))
			die("cannot allocate color");
}

static XftFont *
openfont(const char *key, const char *def)
{
	XftFont *f = XftFontOpenName(dpy, scr, ts(key, def));
	if (!f && !(f = XftFontOpenName(dpy, scr, def)))
		die("cannot open font");
	return f;
}

/* ---------- PAM ---------- */

static int
conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
	struct pam_response *r = calloc(n, sizeof *r);
	(void)data;
	if (!r)
		return PAM_BUF_ERR;
	for (int i = 0; i < n; i++)
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
		    msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
			r[i].resp = strdup(pass);
	*resp = r;
	return PAM_SUCCESS;
}

static int
authenticate(void)
{
	/* use our own service if installed, else fall back to login's auth stack */
	const char *svc = access("/etc/pam.d/wmsliver", R_OK) == 0 ? "wmsliver" : "login";
	struct pam_conv pc = { conv, NULL };
	pam_handle_t *ph;
	int ret;

	if (pam_start(svc, user, &pc, &ph) != PAM_SUCCESS)
		return 0;
	ret = pam_authenticate(ph, 0);
	pam_end(ph, ret);
	return ret == PAM_SUCCESS;
}

/* ---------- drawing ---------- */

static void
rect(Drawable d, XftColor *c, int x, int y, int w, int h)
{
	XSetForeground(dpy, gc, c->pixel);
	XFillRectangle(dpy, d, gc, x, y, w, h);
}

/* text in the box x..x+w, baseline y; just: 0 left, 1 center, 2 right */
static void
text(XftDraw *d, XftFont *f, XftColor *c, int x, int w, int y, int just, const char *s)
{
	XGlyphInfo gi;

	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &gi);
	x += just == 2 ? w - gi.xOff : just == 1 ? (w - gi.xOff) / 2 : 0;
	XftDrawStringUtf8(d, c, f, x, y, (const FcChar8 *)s, strlen(s));
}

/* body y of the password field */
#define FY      (16 + AV + 40)

static void
draw(void)
{
	char stars[sizeof pass];
	int x = bw + 30, y = bw + th + FY, w = PW - 60, h = 24;

	XCopyArea(dpy, frame, buf, gc, 0, 0, fw, fh, 0, 0);

	rect(buf, &cfieldbd, x - 1, y - 1, w + 2, h + 2);
	rect(buf, &cfield, x, y, w, h);
	memset(stars, '*', plen);
	stars[plen > 40 ? 40 : plen] = '\0';
	text(xd, font, &cfieldtx, x, w, y + (h + font->ascent - font->descent) / 2, 1, stars);
	text(xd, font, &cerr, bw, PW, y + h + 24, 1, status);

	XCopyArea(dpy, buf, win, gc, 0, 0, fw, fh, px, py);
	XFlush(dpy);
}

/* dock tiles; the swallowed dockapps paint themselves on top */
static void
drawdock(void)
{
	for (int i = 0; i < ndock; i++)
		XCopyArea(dpy, tilepm, win, gc, 0, 0, TILE, TILE, dx + i * TILE, dy);
}

/* the static parts of the panel, drawn once: frame, titlebar, body, avatar,
 * user name, resizebar */
static void
build_frame(const char *avpath)
{
	const char *j = ts("TitleJustify", "center");
	int just = !strcasecmp(j, "left") ? 0 : !strcasecmp(j, "right") ? 2 : 1;
	int by = bw + th, ax = bw + (PW - AV) / 2, ay = by + 16;
	Imlib_Image img, sq;
	XftDraw *fd;

	frame = XCreatePixmap(dpy, win, fw, fh, depth);
	fd = XftDrawCreate(dpy, frame, vis, cmap);
	rect(frame, &cborder, 0, 0, fw, fh);

	texture(tv("FTitleBack"), PW, th, "black");
	imbevel(PW, th);
	blit(frame, bw, bw);
	text(fd, tfont, &ctitle, bw + 4, PW - 8, bw + (th - 1 + tfont->ascent - tfont->descent) / 2,
	     just, title);

	texture(tv("MenuTextBack"), PW, BH, "gray67");
	blit(frame, bw, by);

	/* avatar with a 1px frame in the border color */
	rect(frame, &cborder, ax - 1, ay - 1, AV + 2, AV + 2);
	if (avpath && (img = imlib_load_image(avpath))) {
		imlib_context_set_image(img);
		int w = imlib_image_get_width(), h = imlib_image_get_height(), s = w < h ? w : h;
		sq = imlib_create_cropped_scaled_image((w - s) / 2, (h - s) / 2, s, s, AV, AV);
		imlib_free_image();
		imlib_context_set_image(sq);
		blit(frame, ax, ay);
	} else {
		/* no picture: generic head-and-shoulders */
		rect(frame, &cfield, ax, ay, AV, AV);
		XSetForeground(dpy, gc, ctext.pixel);
		XFillArc(dpy, frame, gc, ax + AV / 2 - 24, ay + 22, 48, 48, 0, 360 * 64);
		XFillArc(dpy, frame, gc, ax + AV / 2 - 48, ay + 78, 96, 100, 0, 180 * 64);
	}

	text(fd, bfont, &ctext, bw, PW, ay + AV + 26, 1, user);

	texture(tv("ResizebarBack"), PW, RH, "gray67");
	imresizebar(PW, RH);
	blit(frame, bw, by + BH);

	XftDrawDestroy(fd);
}

/* the wallpaper as a background pixmap: wmaker/wmsetbg publish it on the root
 * window; copy it so it survives a wallpaper change while we're locked */
static Pixmap
wallpaper(int sw, int sh, XineramaScreenInfo *xi, int nxi)
{
	Atom a = XInternAtom(dpy, "_XROOTPMAP_ID", True), type;
	unsigned long n, after;
	unsigned char *prop = NULL;
	unsigned int w, h, b, d;
	int fmt, x, y, ok = 0;
	Window r;
	Pixmap pm = XCreatePixmap(dpy, win, sw, sh, depth), root;

	if (a && XGetWindowProperty(dpy, RootWindow(dpy, scr), a, 0, 1, False, XA_PIXMAP,
	                            &type, &fmt, &n, &after, &prop) == Success && prop) {
		if (n) {
			root = *(Pixmap *)prop;
			ok = XGetGeometry(dpy, root, &r, &x, &y, &w, &h, &b, &d) &&
			     d == (unsigned)depth && (int)w >= sw && (int)h >= sh;
			if (ok)
				XCopyArea(dpy, root, pm, gc, 0, 0, sw, sh, 0, 0);
		}
		XFree(prop);
	}
	if (ok)
		return pm;

	/* no published wallpaper: render WorkspaceBack on each head */
	for (int i = 0; i < nxi; i++) {
		texture(tv("WorkspaceBack"), xi[i].width, xi[i].height, "black");
		blit(pm, xi[i].x_org, xi[i].y_org);
	}
	return pm;
}

/* ---------- dockapps ---------- */

static long
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* top-level windows whose WM_CLASS name is `name` */
static int
toplevels(const char *name, Window *out, int max)
{
	Window r, p, *c;
	unsigned int nc;
	XClassHint ch;
	int n = 0;

	if (!XQueryTree(dpy, RootWindow(dpy, scr), &r, &p, &c, &nc))
		return 0;
	for (unsigned int i = 0; i < nc && n < max; i++) {
		if (!XGetClassHint(dpy, c[i], &ch))
			continue;
		if (ch.res_name && strcmp(ch.res_name, name) == 0)
			out[n++] = c[i];
		XFree(ch.res_name);
		XFree(ch.res_class);
	}
	if (c)
		XFree(c);
	return n;
}

/* a dockapp's WM_CLASS name is normally its program name without the directory */
static const char *
progname(int i)
{
	const char *slash = strrchr(dockargv[i][0], '/');
	return slash ? slash + 1 : dockargv[i][0];
}

static void
dock_spawn(void)
{
	for (int i = 0; i < ndock; i++) {
		dock[i].nseen = toplevels(progname(i), dock[i].seen, 16);
		if ((dock[i].pid = fork()) == 0) {
			close(ConnectionNumber(dpy));
			setsid();
			execvp(dockargv[i][0], dockargv[i]);
			_exit(127);
		}
	}
	dockstart = now_ms();
}

/* look for our dockapps' icon windows and swallow them; returns how many are still pending */
static int
dock_poll(void)
{
	Window w[16], r, p, *c;
	unsigned int nc;
	XWMHints *h;
	XWindowAttributes a;
	long age = now_ms() - dockstart;
	int pending = 0;

	for (int i = 0; i < ndock; i++) {
		if (dock[i].pid <= 0 || dock[i].icon)
			continue;
		int n = toplevels(progname(i), w, 16);
		for (int j = 0; j < n && !dock[i].icon; j++) {
			int old = 0;
			for (int k = 0; k < dock[i].nseen; k++)
				old |= dock[i].seen[k] == w[j];
			if (old || !(h = XGetWMHints(dpy, w[j])))
				continue;
			if ((h->flags & IconWindowHint) && XGetWindowAttributes(dpy, h->icon_window, &a) &&
			    XQueryTree(dpy, h->icon_window, &r, &p, &c, &nc)) {
				if (c)
					XFree(c);
				/* wait until the WM has put it in an appicon, or it would take it
				 * back from us; without a WM it stays on the root, so give up
				 * waiting after half a second */
				if (p != r || age > 500) {
					XReparentWindow(dpy, h->icon_window, win,
					                dx + i * TILE + (TILE - a.width) / 2,
					                dy + (TILE - a.height) / 2);
					XMapWindow(dpy, h->icon_window);
					dock[i].icon = h->icon_window;
				}
			}
			XFree(h);
		}
		if (!dock[i].icon && age < 3000)
			pending++;
	}
	return pending;
}

static void
dock_kill(void)
{
	for (int i = 0; i < ndock; i++)
		if (dock[i].pid > 0)
			kill(dock[i].pid, SIGTERM);
}

/* ---------- power profile ---------- */

#ifdef WITH_POWER

#define PPD     "org.freedesktop.UPower.PowerProfiles"
#define PPDPATH "/org/freedesktop/UPower/PowerProfiles"

static void
powersave(void)
{
	if (sd_bus_open_system(&bus) < 0) {
		bus = NULL;
		return;
	}
	sd_bus_get_property_string(bus, PPD, PPDPATH, PPD, "ActiveProfile", NULL, &prevprofile);
	sd_bus_call_method(bus, PPD, PPDPATH, PPD, "HoldProfile", NULL, NULL, "sss",
	                   "power-saver", "screen locked", "wmsliver");
}

static void
powerrestore(void)
{
	/* setting a profile also drops our hold; releasing the hold alone lets
	 * the daemon pick its own default, which is not always what we had */
	if (bus && prevprofile)
		sd_bus_set_property(bus, PPD, PPDPATH, PPD, "ActiveProfile", NULL, "s", prevprofile);
	free(prevprofile);
	if (bus)
		sd_bus_flush_close_unref(bus);
}
#else
static void powersave(void) {}
static void powerrestore(void) {}
#endif

/* ---------- setup ---------- */

static void
grab(void)
{
	int kb = 0, ptr = 0;

	/* another client may hold a grab briefly (menus, key bindings); retry ~1s */
	for (int i = 0; i < 100 && !(kb && ptr); i++) {
		if (!ptr)
			ptr = XGrabPointer(dpy, win, False, ButtonPressMask | PointerMotionMask,
			                   GrabModeAsync, GrabModeAsync, None, None,
			                   CurrentTime) == GrabSuccess;
		if (!kb)
			kb = XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync,
			                   CurrentTime) == GrabSuccess;
		if (!(kb && ptr))
			usleep(10000);
	}
	if (!(kb && ptr))
		die("could not grab keyboard/pointer, not locking");
}

static void
setup(const char *avpath)
{
	Window root = RootWindow(dpy, scr);
	XSetWindowAttributes wa;
	XineramaScreenInfo *xi, whole;
	int nxi = 0, sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	char empty = 0, rc[4096];
	const char *home = getenv("HOME");
	XColor black = { 0 };
	Pixmap blank, bg;
	Cursor cur;

	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	depth = DefaultDepth(dpy, scr);

	theme_load("/etc/WindowMaker/WindowMaker");
	if (getenv("GNUSTEP_USER_ROOT"))
		snprintf(rc, sizeof rc, "%s/Defaults/WindowMaker", getenv("GNUSTEP_USER_ROOT"));
	else
		snprintf(rc, sizeof rc, "%s/GNUstep/Defaults/WindowMaker", home ? home : "");
	theme_load(rc);

	color(&cborder, ts("FrameFocusedBorderColor", ts("FrameBorderColor", NULL)), "black");
	color(&ctitle, ts("FTitleColor", NULL), "white");
	color(&ctext, ts("MenuTextColor", NULL), "black");
	color(&cfield, ts("IconTitleBack", NULL), "black");
	color(&cfieldbd, ts("FrameSelectedBorderColor", NULL), "white");
	color(&cfieldtx, ts("HighlightTextColor", ts("MenuTextColor", NULL)), "white");
	color(&cerr, errcolor, "red");
	tfont = openfont("WindowTitleFont", "Sans:bold:pixelsize=12");
	font = openfont("MenuTextFont", "Sans:pixelsize=12");
	bfont = openfont("MenuTitleFont", "Sans:bold:pixelsize=13");

	bw = atoi(ts("FrameBorderWidth", "1"));
	th = tfont->ascent + tfont->descent + 8;
	fw = PW + 2 * bw;
	fh = bw + th + BH + RH + bw;

	/* one window over the whole root; panel centered and dock in the corner of the first head */
	if (!(xi = XineramaQueryScreens(dpy, &nxi)) || nxi < 1) {
		whole = (XineramaScreenInfo){ 0, 0, 0, sw, sh };
		xi = &whole;
		nxi = 1;
	}
	px = xi[0].x_org + (xi[0].width - fw) / 2;
	py = xi[0].y_org + (xi[0].height - fh) / 2;
	dx = strstr(dockpos, "right") ? xi[0].x_org + xi[0].width - ndock * TILE : xi[0].x_org;
	dy = !strncmp(dockpos, "top", 3) ? xi[0].y_org : xi[0].y_org + xi[0].height - TILE;

	wa.override_redirect = True;
	wa.background_pixel = BlackPixel(dpy, scr);
	win = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, depth, CopyFromParent, vis,
	                    CWOverrideRedirect | CWBackPixel, &wa);
	gc = XCreateGC(dpy, win, 0, NULL);

	imlib_context_set_display(dpy);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cmap);

	bg = wallpaper(sw, sh, xi, nxi);
	XSetWindowBackgroundPixmap(dpy, win, bg);
	XFreePixmap(dpy, bg);
	if (xi != &whole)
		XFree(xi);

	/* invisible cursor */
	blank = XCreateBitmapFromData(dpy, win, &empty, 1, 1);
	cur = XCreatePixmapCursor(dpy, blank, blank, &black, &black, 0, 0);
	XDefineCursor(dpy, win, cur);
	XFreePixmap(dpy, blank);

	build_frame(avpath);
	buf = XCreatePixmap(dpy, win, fw, fh, depth);
	xd = XftDrawCreate(dpy, buf, vis, cmap);

	tilepm = XCreatePixmap(dpy, win, TILE, TILE, depth);
	texture(tv("IconBack"), TILE, TILE, "gray67");
	blit(tilepm, 0, 0);

	XSelectInput(dpy, win, ExposureMask | VisibilityChangeMask);
	XMapRaised(dpy, win);
	grab();
	XSync(dpy, False);
	dock_spawn();
	if (usepower)
		powersave();
}

/* ---------- main loop ---------- */

static void
onsig(int sig)
{
	(void)sig;
	quit = 1;
}

/* a dockapp dying under us must not kill the locker */
static int
xerror(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	return 0;
}

/* returns 1 when unlocked */
static int
handle(XEvent *ev)
{
	KeySym ks;
	char in[32];
	int n;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0) {
			drawdock();
			draw();
		}
		break;
	case VisibilityNotify:
		/* something mapped over us; stay on top */
		if (ev->xvisibility.state != VisibilityUnobscured)
			XRaiseWindow(dpy, win);
		break;
	case KeyPress:
		n = XLookupString(&ev->xkey, in, sizeof in, &ks, NULL);
		if (ks == XK_Return || ks == XK_KP_Enter) {
			pass[plen] = '\0';
			status = "Checking...";
			draw();
			if (authenticate())
				return 1;
			explicit_bzero(pass, sizeof pass);
			plen = 0;
			status = "Wrong password";
		} else if (ks == XK_Escape ||
		           ((ev->xkey.state & ControlMask) && ks == XK_u)) {
			explicit_bzero(pass, sizeof pass);
			plen = 0;
		} else if (ks == XK_BackSpace) {
			if (plen)
				pass[--plen] = '\0';
		} else if (n > 0 && !iscntrl((unsigned char)in[0]) &&
		           plen + n < (int)sizeof pass - 1) {
			memcpy(pass + plen, in, n);
			plen += n;
			status = "";
		}
		draw();
		break;
	}
	return 0;
}

static void
run(void)
{
	int xfd = ConnectionNumber(dpy), pending = ndock;
	struct timeval tv;
	fd_set fds;
	XEvent ev;

	while (!quit) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (handle(&ev))
				return;
		}
		if (pending)
			pending = dock_poll();
		XFlush(dpy);

		/* poll fast only while dockapps are still starting up */
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = pending ? 20000 : 500000;
		select(xfd + 1, &fds, NULL, NULL, &tv);
	}
}

int
main(int argc, char **argv)
{
	struct passwd *pw;
	char path[4096];
	const char *cfg = NULL, *xdg = getenv("XDG_CONFIG_HOME");
	char *cliavatar = NULL;
	int opt;

	/* keep the password out of swap */
	mlock(pass, sizeof pass);

	while ((opt = getopt(argc, argv, "c:a:vh")) != -1) {
		switch (opt) {
		case 'c': cfg = optarg; break;
		case 'a': cliavatar = optarg; break;
		case 'v': puts("wmsliver " VERSION); return 0;
		default: usage();
		}
	}
	if (optind < argc)
		usage();

	if (!(pw = getpwuid(getuid())))
		die("cannot get user");
	user = pw->pw_name;

	/* -c, else $XDG_CONFIG_HOME/wmsliver/config, else /etc/wmsliver.conf */
	if (cfg) {
		if (!config_load(cfg))
			die("cannot read config file");
	} else {
		if (xdg && *xdg)
			snprintf(path, sizeof path, "%s/wmsliver/config", xdg);
		else
			snprintf(path, sizeof path, "%s/.config/wmsliver/config", pw->pw_dir);
		if (!config_load(path))
			config_load("/etc/wmsliver.conf");
	}
	if (cliavatar)
		avatar = expand(cliavatar);
	if (!avatar) {
		snprintf(path, sizeof path, "%s/.face", pw->pw_dir);
		if (access(path, R_OK) != 0)
			snprintf(path, sizeof path, "%s/.face.icon", pw->pw_dir);
		if (access(path, R_OK) == 0)
			avatar = strdup(path);
	}

	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	scr = DefaultScreen(dpy);
	XSetErrorHandler(xerror);

	/* no SA_RESTART, so select() wakes up and we clean up the dockapps */
	struct sigaction sa = { .sa_handler = onsig };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	setup(avatar);
	run();

	dock_kill();
	if (usepower)
		powerrestore();
	explicit_bzero(pass, sizeof pass);
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	XCloseDisplay(dpy);
	return 0;
}
