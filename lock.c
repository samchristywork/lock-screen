#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include <security/pam_appl.h>

#define LOCK_PAM_SERVICE "lock"

#define FONT_TIME "Iosevka NFM:size=64:antialias=true:hinting=true"
#define FONT_DATE "Iosevka NFM:size=16:antialias=true:hinting=true"
#define FONT_DOTS "Iosevka NFM:size=18:antialias=true:hinting=true"

#define COL_BG "#1a1a2e"
#define COL_INPUT "#16213e"
#define COL_FAIL "#2d1b2e"
#define COL_GRID "#2e2e58"
#define COL_TEXT "#e0e0e0"
#define COL_SUBTEXT "#666680"
#define COL_DOT "#e94560"
#define COL_FAIL_TEXT "#e94560"

#define DOT_CHAR "●"
#define DOT_SPACING 6

#define MAX_PASSWD_LEN 256
#define MAX_DOTS 48

enum { ST_INIT, ST_INPUT, ST_FAIL };

typedef struct {
  int screen;
  Window root, win;
  XftDraw *draw;
  int x, y, w, h;
  unsigned long bgcol[3];
  unsigned long gridcol;
} Lock;

static Display *dpy;
static Lock **locks;
static int nlocks;
static int state = ST_INIT;
static char passwd[MAX_PASSWD_LEN];
static int passwd_len = 0;

static XftFont *fnt_time;
static XftFont *fnt_date;
static XftFont *fnt_dots;
static XftColor col_text;
static XftColor col_sub;
static XftColor col_dot;
static XftColor col_fail_text;

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  if (fmt[strlen(fmt) - 1] == ':')
    fprintf(stderr, " %s", strerror(errno));
  fputc('\n', stderr);
  exit(1);
}

static unsigned long xcolor(int screen, const char *name) {
  Colormap cmap = DefaultColormap(dpy, screen);
  XColor c;
  if (!XAllocNamedColor(dpy, cmap, name, &c, &c))
    die("cannot allocate color: %s", name);
  return c.pixel;
}

static void xft_color(int screen, XftColor *out, const char *name) {
  Visual *vis = DefaultVisual(dpy, screen);
  Colormap cmap = DefaultColormap(dpy, screen);
  if (!XftColorAllocName(dpy, vis, cmap, name, out))
    die("cannot allocate Xft color: %s", name);
}

static const char *pam_pw;

static int pam_conv_cb(int nmsg, const struct pam_message **msg,
                       struct pam_response **resp, void *data) {
  (void)data;
  struct pam_response *r = calloc(nmsg, sizeof(*r));
  if (!r)
    return PAM_CONV_ERR;
  for (int i = 0; i < nmsg; i++) {
    if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
        msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
      r[i].resp = strdup(pam_pw);
  }
  *resp = r;
  return PAM_SUCCESS;
}

static int authenticate(const char *pass) {
  struct passwd *pw = getpwuid(getuid());
  struct pam_conv conv = {pam_conv_cb, NULL};
  pam_handle_t *pamh = NULL;
  int ret;

  pam_pw = pass;
  ret = pam_start(LOCK_PAM_SERVICE, pw->pw_name, &conv, &pamh);
  if (ret == PAM_SUCCESS)
    ret = pam_authenticate(pamh, 0);
  pam_end(pamh, ret);
  return ret == PAM_SUCCESS;
}

static void draw_background(Lock *l) {
  GC gc = DefaultGC(dpy, l->screen);
  XSetForeground(dpy, gc, l->gridcol);
  for (int y = 28; y < l->h; y += 28) {
    int offset = ((y / 28) % 2) ? 14 : 0;
    for (int x = 28; x < l->w; x += 28)
      XFillArc(dpy, l->win, gc, x + offset - 2, y - 2, 4, 4, 0, 360 * 64);
  }
}

static void draw_centered_string(XftDraw *draw, XftFont *fnt, XftColor *col,
                                 int cx, int y, const char *s) {
  XGlyphInfo ext;
  XftTextExtentsUtf8(dpy, fnt, (FcChar8 *)s, strlen(s), &ext);
  XftDrawStringUtf8(draw, col, fnt, cx - (int)ext.width / 2, y, (FcChar8 *)s,
                    strlen(s));
}

static void draw_lock(Lock *l) {
  GC gc = DefaultGC(dpy, l->screen);

  XSetForeground(dpy, gc, l->bgcol[state]);
  XFillRectangle(dpy, l->win, gc, 0, 0, l->w, l->h);
  draw_background(l);

  if (l->draw)
    XftDrawDestroy(l->draw);
  l->draw = XftDrawCreate(dpy, l->win, DefaultVisual(dpy, l->screen),
                          DefaultColormap(dpy, l->screen));
  if (!l->draw)
    return;

  int cx = l->x + l->w / 2;
  int cy = l->y + l->h / 2;

  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char timebuf[8];
  strftime(timebuf, sizeof(timebuf), "%H:%M", tm);

  XGlyphInfo ext;
  XftTextExtentsUtf8(dpy, fnt_time, (FcChar8 *)timebuf, strlen(timebuf), &ext);
  int time_y = cy - fnt_time->descent - 16;
  draw_centered_string(l->draw, fnt_time, &col_text, cx, time_y, timebuf);

  char datebuf[32];
  strftime(datebuf, sizeof(datebuf), "%A, %B %d", tm);
  int date_y = time_y + fnt_date->ascent + 10;
  draw_centered_string(l->draw, fnt_date, &col_sub, cx, date_y, datebuf);

  if (state == ST_FAIL) {
    int fail_y = date_y + fnt_date->ascent + 24;
    draw_centered_string(l->draw, fnt_date, &col_fail_text, cx, fail_y,
                         "authentication failed");
  }

  int ndots = passwd_len > MAX_DOTS ? MAX_DOTS : passwd_len;
  if (ndots > 0) {
    XftTextExtentsUtf8(dpy, fnt_dots, (FcChar8 *)DOT_CHAR, strlen(DOT_CHAR),
                       &ext);
    int dw = ext.width + DOT_SPACING;
    int total = ndots * dw - DOT_SPACING;
    int dot_x = cx - total / 2;
    int dot_y = cy + l->h / 4;

    for (int i = 0; i < ndots; i++) {
      XftDrawStringUtf8(l->draw, &col_dot, fnt_dots, dot_x + i * dw, dot_y,
                        (FcChar8 *)DOT_CHAR, strlen(DOT_CHAR));
    }
  }

  XFlush(dpy);
}

static void redraw_all(void) {
  for (int i = 0; i < nlocks; i++)
    draw_lock(locks[i]);
}

static Lock *lock_screen(int screen) {
  Lock *l = calloc(1, sizeof(*l));
  if (!l)
    die("calloc:");

  l->screen = screen;
  l->root = RootWindow(dpy, screen);
  l->x = 0;
  l->y = 0;
  l->w = DisplayWidth(dpy, screen);
  l->h = DisplayHeight(dpy, screen);

  int nout = 0;
  XRRMonitorInfo *monitors = XRRGetMonitors(dpy, l->root, True, &nout);
  if (monitors && nout > 0) {
    (void)monitors;
    XRRFreeMonitors(monitors);
  }

  l->bgcol[ST_INIT] = xcolor(screen, COL_BG);
  l->bgcol[ST_INPUT] = xcolor(screen, COL_INPUT);
  l->bgcol[ST_FAIL] = xcolor(screen, COL_FAIL);
  l->gridcol = xcolor(screen, COL_GRID);

  XSetWindowAttributes wa = {
      .override_redirect = True,
      .background_pixel = l->bgcol[ST_INIT],
      .event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask,
  };

  l->win = XCreateWindow(dpy, l->root, l->x, l->y, l->w, l->h, 0,
                         DefaultDepth(dpy, screen), CopyFromParent,
                         DefaultVisual(dpy, screen),
                         CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);

  char data[1] = {0};
  Pixmap blank = XCreateBitmapFromData(dpy, l->win, data, 1, 1);
  XColor dummy = {0};
  Cursor cur = XCreatePixmapCursor(dpy, blank, blank, &dummy, &dummy, 0, 0);
  XDefineCursor(dpy, l->win, cur);
  XFreePixmap(dpy, blank);

  XMapRaised(dpy, l->win);
  return l;
}

static void grab_inputs(void) {
  struct timespec ts = {.tv_nsec = 50000000L};
  for (int i = 0; i < 20; i++) {
    if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
                      GrabModeAsync, CurrentTime) == GrabSuccess &&
        XGrabPointer(dpy, DefaultRootWindow(dpy), False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None,
                     CurrentTime) == GrabSuccess)
      return;
    nanosleep(&ts, NULL);
  }
  die("cannot grab keyboard/pointer");
}

int main(void) {
  if (!(dpy = XOpenDisplay(NULL)))
    die("cannot open display");

  int nscreens = ScreenCount(dpy);
  locks = calloc(nscreens, sizeof(*locks));
  nlocks = nscreens;

  fnt_time = XftFontOpenName(dpy, 0, FONT_TIME);
  fnt_date = XftFontOpenName(dpy, 0, FONT_DATE);
  fnt_dots = XftFontOpenName(dpy, 0, FONT_DOTS);
  if (!fnt_time || !fnt_date || !fnt_dots)
    die("cannot load font(s)");

  xft_color(0, &col_text, COL_TEXT);
  xft_color(0, &col_sub, COL_SUBTEXT);
  xft_color(0, &col_dot, COL_DOT);
  xft_color(0, &col_fail_text, COL_FAIL_TEXT);

  for (int s = 0; s < nscreens; s++)
    locks[s] = lock_screen(s);

  grab_inputs();
  state = ST_INIT;
  redraw_all();

  int running = 1;
  while (running) {
    fd_set fds;
    int xfd = ConnectionNumber(dpy);
    struct timeval tv = {.tv_sec = 1};
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);

    if (select(xfd + 1, &fds, NULL, NULL, &tv) == 0) {
      redraw_all();
      continue;
    }

    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);

      if (ev.type == KeyPress) {
        char buf[8] = {0};
        KeySym ks;
        XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);

        if (ks == XK_Return || ks == XK_KP_Enter) {
          passwd[passwd_len] = '\0';
          if (authenticate(passwd)) {
            running = 0;
          } else {
            state = ST_FAIL;
            passwd_len = 0;
            redraw_all();
            struct timespec ts = {.tv_nsec = 600000000L};
            nanosleep(&ts, NULL);
            state = ST_INIT;
          }
        } else if (ks == XK_Escape) {
          passwd_len = 0;
          state = ST_INIT;
        } else if (ks == XK_BackSpace || ks == XK_Delete) {
          if (passwd_len > 0)
            passwd_len--;
          state = passwd_len ? ST_INPUT : ST_INIT;
        } else if (buf[0] && !iscntrl((unsigned char)buf[0])) {
          int n = strlen(buf);
          if (passwd_len + n < MAX_PASSWD_LEN) {
            memcpy(passwd + passwd_len, buf, n);
            passwd_len += n;
            state = ST_INPUT;
          }
        }
        redraw_all();
      } else if (ev.type == Expose || ev.type == VisibilityNotify) {
        for (int i = 0; i < nlocks; i++)
          XRaiseWindow(dpy, locks[i]->win);
        redraw_all();
      }
    }
  }

  XUngrabPointer(dpy, CurrentTime);
  XUngrabKeyboard(dpy, CurrentTime);
  for (int i = 0; i < nlocks; i++) {
    if (locks[i]->draw)
      XftDrawDestroy(locks[i]->draw);
    XDestroyWindow(dpy, locks[i]->win);
    free(locks[i]);
  }
  free(locks);
  XftFontClose(dpy, fnt_time);
  XftFontClose(dpy, fnt_date);
  XftFontClose(dpy, fnt_dots);
  XCloseDisplay(dpy);
  return 0;
}
