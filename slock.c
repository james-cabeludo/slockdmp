/* See LICENSE file for license details. */
#include <X11/X.h>
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#  include <shadow.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include "arg.h"
#include "util.h"

#include "lockscreen-data.h"

char *argv0;

enum
{
  INIT,
  INPUT,
  FAILED,
  NUMCOLS
};

struct lock
{
  int screen;
  Window root, win;
  Pixmap pmap;

  int width, height;
  XImage *typing, *error, *lock;
};

struct xrandr
{
  int active;
  int evbase;
  int errbase;
};

#include "config.h"

static void
die(const char *errstr, ...)
{
  va_list ap;

  va_start(ap, errstr);
  vfprintf(stderr, errstr, ap);
  va_end(ap);
  exit(1);
}

#ifdef __linux__
#  include <fcntl.h>
#  include <linux/oom.h>

static void
dontkillme(void)
{
  FILE *f;
  const char oomfile[] = "/proc/self/oom_score_adj";

  if (!(f = fopen(oomfile, "w")))
  {
    if (errno == ENOENT)
    {
      return;
    }
    die("slock: fopen %s: %s\n", oomfile, strerror(errno));
  }
  fprintf(f, "%d", OOM_SCORE_ADJ_MIN);
  if (fclose(f))
  {
    if (errno == EACCES)
    {
      die("slock: unable to disable OOM killer. "
          "Make sure to suid or sgid slock.\n");
    }
    else
    {
      die("slock: fclose %s: %s\n", oomfile, strerror(errno));
    }
  }
}
#endif

static const char *
gethash(void)
{
  const char *hash;
  struct passwd *pw;

  /* Check if the current user has a password entry */
  errno = 0;
  if (!(pw = getpwuid(getuid())))
  {
    if (errno)
    {
      die("slock: getpwuid: %s\n", strerror(errno));
    }
    else
    {
      die("slock: cannot retrieve password entry\n");
    }
  }
  hash = pw->pw_passwd;

#if HAVE_SHADOW_H
  if (!strcmp(hash, "x"))
  {
    struct spwd *sp;
    if (!(sp = getspnam(pw->pw_name)))
    {
      die("slock: getspnam: cannot retrieve shadow entry. "
          "Make sure to suid or sgid slock.\n");
    }
    hash = sp->sp_pwdp;
  }
#else
  if (!strcmp(hash, "*"))
  {
#  ifdef __OpenBSD__
    if (!(pw = getpwuid_shadow(getuid())))
    {
      die("slock: getpwnam_shadow: cannot retrieve shadow entry. "
          "Make sure to suid or sgid slock.\n");
    }
    hash = pw->pw_passwd;
#  else
    die("slock: getpwuid: cannot retrieve shadow entry. "
        "Make sure to suid or sgid slock.\n");
#  endif /* __OpenBSD__ */
  }
#endif   /* HAVE_SHADOW_H */

  return hash;
}

static void
readpw(Display *dpy, struct xrandr *rr, struct lock **locks, int nscreens, const char *hash)
{
  char buf[32], passwd[256], *inputhash;

  unsigned int len = 0;
  int running      = 1;
  int failure      = 0;
  int oldc         = INIT;

  XEvent ev;
  while (running && !XNextEvent(dpy, &ev))
  {
    if (ev.type == KeyPress)
    {
      explicit_bzero(&buf, sizeof(buf));

      KeySym ksym;
      int num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
      if (IsKeypadKey(ksym))
      {
        if (ksym == XK_KP_Enter)
        {
          ksym = XK_Return;
        }
        else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
        {
          ksym = (ksym - XK_KP_0) + XK_0;
        }
      }
      if (IsFunctionKey(ksym) || IsKeypadKey(ksym) || IsMiscFunctionKey(ksym) || IsPFKey(ksym) || IsPrivateKeypadKey(ksym))
      {
        continue;
      }
      switch (ksym)
      {
      case XK_Return:
        passwd[len] = '\0';
        errno       = 0;
        if (!(inputhash = crypt(passwd, hash)))
        {
          fprintf(stderr, "slock: crypt: %s\n", strerror(errno));
        }
        else
        {
          running = !!strcmp(inputhash, hash);
        }
        if (running)
        {
          XBell(dpy, 100);
          failure = 1;
        }
        explicit_bzero(&passwd, sizeof(passwd));
        len = 0;
        break;
      case XK_Escape:
        explicit_bzero(&passwd, sizeof(passwd));
        len = 0;
        break;
      case XK_BackSpace:
        if (len)
        {
          passwd[--len] = '\0';
        }
        break;
      default:
        if (num && !iscntrl((int)buf[0]) && (len + num < sizeof(passwd)))
        {
          memcpy(passwd + len, buf, num);
          len += num;
        }
        break;
      }

      unsigned int color = len ? INPUT : ((failure || failonclear) ? FAILED : INIT);

      if (running && oldc != color)
      {
        for (int screen = 0; screen < nscreens; screen++)
        {
          int window_width  = locks[screen]->width;
          int window_height = locks[screen]->height;
          int x_offset      = (window_width - INFO_WIDTH) / 2;
          int y_offset      = (window_height - INFO_WIDTH) - 30;

          switch (color)
          {
          default:
            {
              XClearArea(dpy, locks[screen]->win, x_offset, y_offset, INFO_WIDTH, INFO_WIDTH, 0);
            }
            break;
          case INPUT:
            {
              XPutImage(
                  dpy, locks[screen]->win, DefaultGC(dpy, screen), locks[screen]->typing, 0, 0, x_offset, y_offset, INFO_WIDTH, INFO_WIDTH);
            }
            break;
          case FAILED:
            {
              XPutImage(
                  dpy, locks[screen]->win, DefaultGC(dpy, screen), locks[screen]->error, 0, 0, x_offset, y_offset, INFO_WIDTH, INFO_WIDTH);
            }
            break;
          }
        }
        oldc = color;
      }
    }
    else if (ev.type == KeyRelease)
    {
      // printf("KeyRelease\n");
    }
    else if (rr->active && ev.type == rr->evbase + RRScreenChangeNotify)
    {
      XRRScreenChangeNotifyEvent *rre = (XRRScreenChangeNotifyEvent *)&ev;
      for (int screen = 0; screen < nscreens; screen++)
      {
        if (locks[screen]->win == rre->window)
        {
          if (rre->rotation == RR_Rotate_90 || rre->rotation == RR_Rotate_270)
          {
            XResizeWindow(dpy, locks[screen]->win, rre->height, rre->width);
            locks[screen]->width  = rre->height;
            locks[screen]->height = rre->width;
          }
          else
          {
            XResizeWindow(dpy, locks[screen]->win, rre->width, rre->height);
            locks[screen]->width  = rre->width;
            locks[screen]->height = rre->height;
          }
          XClearWindow(dpy, locks[screen]->win);
          break;
        }
      }
    }
    else
    {
      for (int screen = 0; screen < nscreens; screen++)
      {
        XRaiseWindow(dpy, locks[screen]->win);
      }
    }
  }
}

static struct lock *
lockscreen(Display *dpy, struct xrandr *rr, int screen)
{
  char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (dpy == 0 || screen < 0)
  {
    return 0;
  }

  struct lock *lock = malloc(sizeof(struct lock));
  if (lock == 0)
  {
    return 0;
  }

  lock->screen = screen;
  lock->root   = RootWindow(dpy, lock->screen);

  /* init */
  XSetWindowAttributes wa = {0};
  wa.override_redirect    = 1;
  wa.background_pixel     = 0x121212FF;
  lock->win               = XCreateWindow(dpy, lock->root, 0, 0, DisplayWidth(dpy, lock->screen), DisplayHeight(dpy, lock->screen), 0,
                    DefaultDepth(dpy, lock->screen), CopyFromParent, DefaultVisual(dpy, lock->screen), CWOverrideRedirect | CWBackPixel, &wa);
  lock->pmap              = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);

  XColor color;
  Cursor invisible = XCreatePixmapCursor(dpy, lock->pmap, lock->pmap, &color, &color, 0, 0);
  XDefineCursor(dpy, lock->win, invisible);
  XSetWindowBackground(dpy, lock->win, 0x12121212);

  XWindowAttributes attrs = {0};
  XGetWindowAttributes(dpy, lock->win, &attrs);
  lock->width  = attrs.width;
  lock->height = attrs.height;

  Visual *vi         = DefaultVisual(dpy, screen);
  unsigned int depth = DefaultDepth(dpy, screen);
  lock->lock         = XCreateImage(dpy, vi, depth, ZPixmap, 0, (char *)lockscreen_bg_data, BG_WIDTH, BG_HEIGHT, 32, 0);
  lock->typing       = XCreateImage(dpy, vi, depth, ZPixmap, 0, (char *)lockscreen_typing_data, INFO_WIDTH, INFO_WIDTH, 32, 0);
  lock->error        = XCreateImage(dpy, vi, depth, ZPixmap, 0, (char *)lockscreen_error_data, INFO_WIDTH, INFO_WIDTH, 32, 0);

  /* Try to grab mouse pointer *and* keyboard for 600ms, else fail the lock */

  int ptgrab = -1, kbgrab = -1;
  for (int i = 0; i < 6; i += 1)
  {
    if (ptgrab != GrabSuccess)
    {
      ptgrab = XGrabPointer(dpy, lock->root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
          None, invisible, CurrentTime);
    }
    if (kbgrab != GrabSuccess)
    {
      kbgrab = XGrabKeyboard(dpy, lock->root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    }

    /* input is grabbed: we can lock the screen */
    if (ptgrab == GrabSuccess && kbgrab == GrabSuccess)
    {
      XMapRaised(dpy, lock->win);

      if (rr->active)
      {
        XRRSelectInput(dpy, lock->win, RRScreenChangeNotifyMask);
      }
      XSelectInput(dpy, lock->root, SubstructureNotifyMask);

      int window_width  = lock->width;
      int window_height = lock->height;
      int img_width     = BG_WIDTH;
      int img_height    = BG_HEIGHT;

      int x_offset = (window_width - img_width) / 2;
      int y_offset = (window_height - img_height) / 2;
      XPutImage(dpy, lock->win, DefaultGC(dpy, screen), lock->lock, 0, 0, x_offset, y_offset, img_width, img_height);

      return lock;
    }

    /* retry on AlreadyGrabbed but fail on other errors */
    if ((ptgrab != AlreadyGrabbed && ptgrab != GrabSuccess) || (kbgrab != AlreadyGrabbed && kbgrab != GrabSuccess))
    {
      break;
    }

    usleep(100000);
  }

  /* we couldn't grab all input: fail out */
  if (ptgrab != GrabSuccess)
  {
    fprintf(stderr, "slock: unable to grab mouse pointer for screen %d\n", screen);
  }
  if (kbgrab != GrabSuccess)
  {
    fprintf(stderr, "slock: unable to grab keyboard for screen %d\n", screen);
  }
  return NULL;
}

static void
usage(void)
{
  die("usage: slock [-v] [cmd [arg ...]]\n");
}

int
main(int argc, char **argv)
{
  struct xrandr rr;
  struct lock **locks;
  struct passwd *pwd;
  struct group *grp;
  uid_t duid;
  gid_t dgid;
  const char *hash;
  Display *dpy;
  int s, nlocks, nscreens;

  ARGBEGIN
  {
  case 'v': fprintf(stderr, "slock-" VERSION "\n"); return 0;
  default : usage();
  }
  ARGEND

  /* validate drop-user and -group */
  errno = 0;
  if (!(pwd = getpwnam(user)))
  {
    die("slock: getpwnam %s: %s\n", user, errno ? strerror(errno) : "user entry not found");
  }
  duid  = pwd->pw_uid;
  errno = 0;
  if (!(grp = getgrnam(group)))
  {
    die("slock: getgrnam %s: %s\n", group, errno ? strerror(errno) : "group entry not found");
  }
  dgid = grp->gr_gid;

#ifdef __linux__
  dontkillme();
#endif

  hash  = gethash();
  errno = 0;
  if (!crypt("", hash))
  {
    die("slock: crypt: %s\n", strerror(errno));
  }

  if (!(dpy = XOpenDisplay(NULL)))
  {
    die("slock: cannot open display\n");
  }

  /* drop privileges */
  if (setgroups(0, NULL) < 0)
  {
    die("slock: setgroups: %s\n", strerror(errno));
  }
  if (setgid(dgid) < 0)
  {
    die("slock: setgid: %s\n", strerror(errno));
  }
  if (setuid(duid) < 0)
  {
    die("slock: setuid: %s\n", strerror(errno));
  }

  /* check for Xrandr support */
  rr.active = XRRQueryExtension(dpy, &rr.evbase, &rr.errbase);

  /* get number of screens in display "dpy" and blank them */
  nscreens = ScreenCount(dpy);
  if (!(locks = calloc(nscreens, sizeof(struct lock *))))
  {
    die("slock: out of memory\n");
  }
  for (nlocks = 0, s = 0; s < nscreens; s++)
  {
    if ((locks[s] = lockscreen(dpy, &rr, s)) != NULL)
    {
      nlocks++;
    }
    else
    {
      break;
    }
  }
  XSync(dpy, 0);

  /* did we manage to lock everything? */
  if (nlocks != nscreens)
  {
    return 1;
  }

  /* run post-lock command */
  if (argc > 0)
  {
    switch (fork())
    {
    case -1: die("slock: fork failed: %s\n", strerror(errno));
    case 0:
      if (close(ConnectionNumber(dpy)) < 0)
      {
        die("slock: close: %s\n", strerror(errno));
      }
      execvp(argv[0], argv);
      fprintf(stderr, "slock: execvp %s: %s\n", argv[0], strerror(errno));
      _exit(1);
    }
  }

  /* everything is now blank. Wait for the correct password */
  readpw(dpy, &rr, locks, nscreens, hash);

  return 0;
}
