/* wmsliver-logout - a Window Maker style session menu: lock, log out,
 * suspend, reboot, shut down.
 *
 * It is drawn as a Window Maker menu from the active theme (MenuTitleBack,
 * MenuTextBack, MenuStyle, HighlightColor, menu fonts), pops up in a corner
 * (or the center) of the monitor with the pointer, and grabs the keyboard until you pick
 * something or press Escape. The chosen command runs through /bin/sh after
 * the menu has closed.
 */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <Imlib2.h>
#include <ctype.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "common.h"

#ifndef VERSION
#define VERSION "dev"
#endif

#define MINW    180             /* minimum menu width */
#define PAD     8               /* text indent inside title and items */
#define GRACE   300             /* ms to ignore keys after popping up (autorepeat of the hotkey) */

const char *prog = "wmsliver-logout";

static struct {
	const char *key;        /* config key */
	const char *label;
	char hotkey;
	char *cmd;              /* NULL or empty: item hidden */
} items[] = {
	{ "lock",     "Lock",      'l', NULL },
	{ "logout",   "Log Out",   'o', NULL },
	{ "suspend",  "Suspend",   's', NULL },
	{ "reboot",   "Reboot",    'r', NULL },
	{ "shutdown", "Shut Down", 'd', NULL },
};
#define NITEMS  (int)(sizeof items / sizeof items[0])

static char *title = "Session";
static char position[16] = "bottom-left";      /* a corner, or center */
static int shown[NITEMS], nshown;       /* indexes of visible items */
static int sel;                         /* selected row, -1 for none */

static Window win;
static Pixmap frame, buf;
static XftDraw *xd;
static XftFont *tfont, *font;
static XftColor cborder, ctitle, ctext, chl, chltext;
static int W, H, bw, th, ih;            /* size, border, title and item heights */

/* ---------- config ---------- */

static int
config_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024], *k, *v, *eq;
	int ln = 0, known;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		ln++;
		k = trim(line);
		if (!*k || *k == '#')
			continue;
		if (!(eq = strchr(k, '='))) {
			fprintf(stderr, "%s: %s:%d: expected key = value\n", prog, path, ln);
			continue;
		}
		*eq = '\0';
		k = trim(k);
		v = trim(eq + 1);

		if (!strcmp(k, "title")) {
			title = strdup(v);
			continue;
		}
		if (!strcmp(k, "position")) {
			if (strcmp(v, "bottom-right") && strcmp(v, "bottom-left") && strcmp(v, "top-right") &&
			    strcmp(v, "top-left") && strcmp(v, "center"))
				fprintf(stderr, "%s: %s:%d: position must be bottom-right, bottom-left, "
				        "top-right, top-left or center\n", prog, path, ln);
			else
				snprintf(position, sizeof position, "%s", v);
			continue;
		}
		known = 0;
		for (int i = 0; i < NITEMS; i++)
			if (!strcmp(k, items[i].key)) {
				items[i].cmd = strdup(v);
				known = 1;
			}
		if (!known)
			fprintf(stderr, "%s: %s:%d: unknown key '%s'\n", prog, path, ln, k);
	}
	fclose(f);
	return 1;
}

/* wmsliver from the same directory as us if it's there, so an uninstalled
 * build works; otherwise from $PATH */
static const char *
locker(void)
{
	static char path[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", path, sizeof path - 16);
	char *slash;

	if (n > 0) {
		path[n] = '\0';
		if ((slash = strrchr(path, '/'))) {
			strcpy(slash + 1, "wmsliver");
			if (access(path, X_OK) == 0)
				return path;
		}
	}
	return "wmsliver";
}

static void
defaults(void)
{
	static char suspend[PATH_MAX + 64];
	const char *lock = locker();

	snprintf(suspend, sizeof suspend, "%s & sleep 1; exec systemctl suspend", lock);
	const char *def[NITEMS] = {
		lock,
		/* wmaker saves its state and exits on SIGTERM; -n picks the real
		 * wmaker, not the wrapper that started it */
		"pkill -TERM -n -x wmaker",
		suspend,
		"systemctl reboot",
		"systemctl poweroff",
	};
	for (int i = 0; i < NITEMS; i++)
		items[i].cmd = (char *)def[i];
}

static void
usage(void)
{
	fputs("usage: wmsliver-logout [-c config] [-v]\n", stderr);
	exit(1);
}

/* ---------- drawing ---------- */

static long
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int
textwidth(XftFont *f, const char *s)
{
	XGlyphInfo gi;
	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &gi);
	return gi.xOff;
}

static void
label(XftDraw *d, XftColor *c, int row)
{
	char key[2] = { toupper((unsigned char)items[shown[row]].hotkey), '\0' };
	int y = bw + th + row * ih;
	int base = y + (ih + font->ascent - font->descent) / 2;

	text(d, font, c, bw + PAD, W - 2 * bw - 2 * PAD, base, 0, items[shown[row]].label);
	text(d, font, c, bw + PAD, W - 2 * bw - 2 * PAD, base, 2, key);
}

/* the menu without a selection, drawn once */
static void
build_frame(void)
{
	const char *style = ts("MenuStyle", "normal");
	int iw = W - 2 * bw;
	XftDraw *fd;

	frame = XCreatePixmap(dpy, win, W, H, depth);
	fd = XftDrawCreate(dpy, frame, vis, cmap);
	rect(frame, &cborder, 0, 0, W, H);

	texture(tv("MenuTitleBack"), iw, th, "black");
	imbevel(iw, th);
	blit(frame, bw, bw);
	text(fd, tfont, &ctitle, bw + PAD, iw - 2 * PAD,
	     bw + (th - 1 + tfont->ascent - tfont->descent) / 2, 0, title);

	/* "normal" menus bevel every item; "flat" and "singletexture" share one texture */
	if (!strcasecmp(style, "normal")) {
		for (int r = 0; r < nshown; r++) {
			texture(tv("MenuTextBack"), iw, ih, "gray67");
			imbevel(iw, ih);
			blit(frame, bw, bw + th + r * ih);
		}
	} else {
		texture(tv("MenuTextBack"), iw, nshown * ih, "gray67");
		blit(frame, bw, bw + th);
	}
	for (int r = 0; r < nshown; r++)
		label(fd, &ctext, r);

	XftDrawDestroy(fd);
}

static void
draw(void)
{
	XCopyArea(dpy, frame, buf, gc, 0, 0, W, H, 0, 0);
	if (sel >= 0) {
		rect(buf, &chl, bw, bw + th + sel * ih, W - 2 * bw, ih);
		label(xd, &chltext, sel);
	}
	XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
	XFlush(dpy);
}

/* row under window coordinates x,y, or -1 */
static int
rowat(int x, int y)
{
	if (x < bw || x >= W - bw || y < bw + th || y >= H - bw)
		return -1;
	return (y - bw - th) / ih;
}

/* ---------- setup ---------- */

static void
setup(void)
{
	XSetWindowAttributes wa;
	XineramaScreenInfo *xi;
	Window r, c;
	int n, rx, ry, wx, wy, w, x, y;
	int sx = 0, sy = 0, sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	unsigned int mask;

	theme_init();
	color(&cborder, ts("FrameBorderColor", NULL), "black");
	color(&ctitle, ts("MenuTitleColor", NULL), "white");
	color(&ctext, ts("MenuTextColor", NULL), "black");
	color(&chl, ts("HighlightColor", NULL), "white");
	color(&chltext, ts("HighlightTextColor", NULL), "black");
	tfont = openfont("MenuTitleFont", "Sans:bold:pixelsize=13");
	font = openfont("MenuTextFont", "Sans:pixelsize=12");

	bw = 1;
	th = tfont->ascent + tfont->descent + 8;
	ih = font->ascent + font->descent + 6;

	W = textwidth(tfont, title) + 2 * PAD;
	for (int i = 0; i < nshown; i++) {
		w = textwidth(font, items[shown[i]].label) + textwidth(font, "M") + 4 * PAD;
		if (w > W)
			W = w;
	}
	W = (W < MINW ? MINW : W) + 2 * bw;
	H = bw + th + nshown * ih + bw;

	/* place on the monitor that has the pointer */
	XQueryPointer(dpy, RootWindow(dpy, scr), &r, &c, &rx, &ry, &wx, &wy, &mask);
	if ((xi = XineramaQueryScreens(dpy, &n))) {
		for (int i = 0; i < n; i++)
			if (rx >= xi[i].x_org && rx < xi[i].x_org + xi[i].width &&
			    ry >= xi[i].y_org && ry < xi[i].y_org + xi[i].height) {
				sx = xi[i].x_org; sy = xi[i].y_org;
				sw = xi[i].width; sh = xi[i].height;
			}
		XFree(xi);
	}

	wa.override_redirect = True;
	wa.background_pixel = BlackPixel(dpy, scr);
	if (!strcmp(position, "center")) {
		x = sx + (sw - W) / 2;
		y = sy + (sh - H) / 2;
	} else {
		x = strstr(position, "right") ? sx + sw - W : sx;
		y = !strncmp(position, "bottom", 6) ? sy + sh - H : sy;
	}
	win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y,
	                    W, H, 0, depth, CopyFromParent, vis,
	                    CWOverrideRedirect | CWBackPixel, &wa);
	gc = XCreateGC(dpy, win, 0, NULL);

	build_frame();
	buf = XCreatePixmap(dpy, win, W, H, depth);
	xd = XftDrawCreate(dpy, buf, vis, cmap);

	XSelectInput(dpy, win, ExposureMask | VisibilityChangeMask);
	XMapRaised(dpy, win);
	if (!grab(win))
		die("could not grab keyboard/pointer");
}

/* ---------- main loop ---------- */

/* returns the chosen item, or -1 to cancel */
static int
run(void)
{
	long start = now_ms();
	XEvent ev;
	KeySym ks;
	char in[8];
	int r;

	for (;;) {
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				draw();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		case MotionNotify:
			if ((r = rowat(ev.xmotion.x, ev.xmotion.y)) != sel && r >= 0) {
				sel = r;
				draw();
			}
			break;
		case ButtonPress:
			/* a click outside the menu cancels */
			if (ev.xbutton.x < 0 || ev.xbutton.y < 0 || ev.xbutton.x >= W || ev.xbutton.y >= H)
				return -1;
			break;
		case ButtonRelease:
			/* like wmaker menus, act on release */
			if ((r = rowat(ev.xbutton.x, ev.xbutton.y)) >= 0)
				return shown[r];
			break;
		case KeyPress:
			if (now_ms() - start < GRACE)
				break;
			XLookupString(&ev.xkey, in, sizeof in, &ks, NULL);
			switch (ks) {
			case XK_Escape:
				return -1;
			case XK_Return: case XK_KP_Enter: case XK_space:
				if (sel >= 0)
					return shown[sel];
				break;
			case XK_Up: case XK_KP_Up: case XK_ISO_Left_Tab: case XK_k:
				sel = sel <= 0 ? nshown - 1 : sel - 1;
				break;
			case XK_Down: case XK_KP_Down: case XK_Tab: case XK_j:
				sel = sel >= nshown - 1 ? 0 : sel + 1;
				break;
			case XK_Home:
				sel = 0;
				break;
			case XK_End:
				sel = nshown - 1;
				break;
			default:
				for (r = 0; r < nshown; r++)
					if (tolower((int)ks) == items[shown[r]].hotkey)
						return shown[r];
			}
			draw();
			break;
		}
	}
}

int
main(int argc, char **argv)
{
	const char *cfg = NULL, *xdg = getenv("XDG_CONFIG_HOME");
	struct passwd *pw = getpwuid(getuid());
	char path[4096];
	int opt, choice;

	while ((opt = getopt(argc, argv, "c:vh")) != -1) {
		switch (opt) {
		case 'c': cfg = optarg; break;
		case 'v': puts("wmsliver-logout " VERSION); return 0;
		default: usage();
		}
	}
	if (optind < argc)
		usage();

	defaults();
	/* -c, else $XDG_CONFIG_HOME/wmsliver/logout, else /etc/wmsliver-logout.conf */
	if (cfg) {
		if (!config_load(cfg))
			die("cannot read config file");
	} else {
		if (xdg && *xdg)
			snprintf(path, sizeof path, "%s/wmsliver/logout", xdg);
		else
			snprintf(path, sizeof path, "%s/.config/wmsliver/logout", pw ? pw->pw_dir : "");
		if (!config_load(path))
			config_load("/etc/wmsliver-logout.conf");
	}
	for (int i = 0; i < NITEMS; i++)
		if (items[i].cmd && *items[i].cmd)
			shown[nshown++] = i;
	if (!nshown)
		die("every item is disabled in the config");

	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	scr = DefaultScreen(dpy);

	setup();
	choice = run();

	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);

	if (choice < 0)
		return 0;
	execl("/bin/sh", "sh", "-c", items[choice].cmd, (char *)NULL);
	die("cannot run /bin/sh");
	return 1;
}
