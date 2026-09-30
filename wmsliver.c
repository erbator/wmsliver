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
#include "common.h"
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

const char *prog = "wmsliver";

/* settings; defaults here, overridden by the config file, then the command line */
static char *avatar;                            /* NULL: ~/.face, then ~/.face.icon */
static char *title = "Screen Locked";
static char *errcolor = "#e05050";
static char dockpos[16] = "bottom-left";        /* bottom-left/-right, top-left/-right */
static int usepower = 1;
static char *dockargv[MAXDOCK][MAXARG];
static int ndock;

static Window win;
static Pixmap frame, buf, tilepm;
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

/* ---------- config ---------- */

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
setup(const char *avpath)
{
	Window root = RootWindow(dpy, scr);
	XSetWindowAttributes wa;
	XineramaScreenInfo *xi, whole;
	int nxi = 0, sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	char empty = 0;
	XColor black = { 0 };
	Pixmap blank, bg;
	Cursor cur;

	theme_init();

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
	if (!grab(win))
		die("could not grab keyboard/pointer, not locking");
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
