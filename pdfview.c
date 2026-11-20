#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/* Rendering is done by Poppler's pdftoppm / pdfinfo (poppler-utils). */

#define STEP     50
#define BASE_DPI 100.0
#define ZOOM_F   1.25
#define BG       0x000000UL   /* color of the space around the page */

struct pv {
    Display *dpy;
    Window win;
    GC gc;
    char *q;                  /* shell-quoted pdf path */
    char base[64];            /* temp file base */
    int n, cur;
    double zoom;
    unsigned char *img;       /* RGB */
    int iw, ih;
    long px, py;              /* page offset from centered position */
};

static char *quote(const char *s)
{
    char *r, *p;
    r = malloc(strlen(s) * 4 + 3);
    if (!r)
        return NULL;
    p = r;
    *p++ = '\'';
    for (; *s; s++) {
        if (*s == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = *s;
        }
    }
    *p++ = '\'';
    *p = '\0';
    return r;
}

static int page_count(struct pv *v)
{
    char *cmd;
    char line[256];
    FILE *f;
    int n = 0;

    cmd = malloc(strlen(v->q) + 128);
    if (!cmd)
        return 0;
    sprintf(cmd, "pdfinfo %s > %s.info", v->q, v->base);
    if (system(cmd) == 0) {
        sprintf(cmd, "%s.info", v->base);
        f = fopen(cmd, "r");
        if (f) {
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "Pages: %d", &n) == 1)
                    break;
            fclose(f);
        }
        remove(cmd);
    }
    free(cmd);
    return n;
}

/* render page v->cur at v->zoom; keeps old image on failure */
static int load(struct pv *v)
{
    char *cmd;
    char ppm[96];
    FILE *f;
    int w, h, mx, ok = -1;
    unsigned char *data;

    cmd = malloc(strlen(v->q) + 256);
    if (!cmd)
        return -1;
    sprintf(ppm, "%s.ppm", v->base);
    sprintf(cmd, "pdftoppm -r %d -f %d -l %d -singlefile %s %s",
            (int)(BASE_DPI * v->zoom), v->cur + 1, v->cur + 1, v->q, v->base);
    if (system(cmd) == 0 && (f = fopen(ppm, "rb")) != NULL) {
        if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) == 3 && w > 0 && h > 0) {
            fgetc(f);
            data = malloc((size_t)w * h * 3);
            if (data && fread(data, 3, (size_t)w * h, f) == (size_t)w * h) {
                free(v->img);
                v->img = data;
                v->iw = w;
                v->ih = h;
                ok = 0;
            } else {
                free(data);
            }
        }
        fclose(f);
        remove(ppm);
    }
    free(cmd);
    return ok;
}

static int shift_of(unsigned long m)
{
    int s = 0;
    while (m && !(m & 1UL)) {
        m >>= 1;
        s++;
    }
    return s;
}

static void clamp(long *p, int is, int ws)
{
    long lim;
    if (is <= ws) {
        *p = 0;
        return;
    }
    lim = (is - ws) / 2;
    if (*p > lim)
        *p = lim;
    if (*p < -lim)
        *p = -lim;
}

static void draw(struct pv *v)
{
    XWindowAttributes wa;
    XImage *xi;
    unsigned int *buf, *row;
    int rs, gs, bs, ww, wh, x, y, ox, oy, sx, sy;
    unsigned char *p;

    XGetWindowAttributes(v->dpy, v->win, &wa);
    ww = wa.width;
    wh = wa.height;
    if (ww < 1 || wh < 1)
        return;
    clamp(&v->px, v->iw, ww);
    clamp(&v->py, v->ih, wh);
    ox = (ww - v->iw) / 2 + (int)v->px;
    oy = (wh - v->ih) / 2 + (int)v->py;
    rs = shift_of(wa.visual->red_mask);
    gs = shift_of(wa.visual->green_mask);
    bs = shift_of(wa.visual->blue_mask);

    buf = malloc((size_t)ww * wh * 4);
    if (!buf)
        return;
    for (y = 0; y < wh; y++) {
        row = buf + (size_t)y * ww;
        sy = y - oy;
        for (x = 0; x < ww; x++) {
            sx = x - ox;
            if (sy < 0 || sy >= v->ih || sx < 0 || sx >= v->iw) {
                row[x] = BG;
            } else {
                p = v->img + ((size_t)sy * v->iw + sx) * 3;
                row[x] = ((unsigned int)p[0] << rs) |
                         ((unsigned int)p[1] << gs) |
                         ((unsigned int)p[2] << bs);
            }
        }
    }
    xi = XCreateImage(v->dpy, wa.visual, wa.depth, ZPixmap, 0,
                      (char *)buf, ww, wh, 32, 0);
    if (!xi) {
        free(buf);
        return;
    }
    XPutImage(v->dpy, v->win, v->gc, xi, 0, 0, 0, 0, ww, wh);
    XDestroyImage(xi);   /* frees buf */
    XFlush(v->dpy);
}

static void set_title(struct pv *v, const char *path)
{
    char buf[512];
    sprintf(buf, "%.400s  [%d/%d]", path, v->cur + 1, v->n);
    XStoreName(v->dpy, v->win, buf);
}

static int set_zoom(struct pv *v, double nz)
{
    double old = v->zoom;

    if (nz < 0.1 || nz > 6.0)
        return 0;
    v->zoom = nz;
    if (load(v) != 0) {
        v->zoom = old;
        return 0;
    }
    v->px = (long)(v->px * nz / old);
    v->py = (long)(v->py * nz / old);
    return 1;
}

static int set_page(struct pv *v, int np, const char *path)
{
    int old = v->cur;

    if (np < 0 || np >= v->n)
        return 0;
    v->cur = np;
    if (load(v) != 0) {
        v->cur = old;
        return 0;
    }
    v->px = 0;
    v->py = 1L << 30;     /* clamped to top of page */
    set_title(v, path);
    return 1;
}

static int run(const char *path)
{
    struct pv v;
    XEvent e;
    KeySym ks;
    char kb[16];
    Atom wmdel;
    int running = 1, redraw;

    memset(&v, 0, sizeof v);
    v.zoom = 1.0;
    v.py = 1L << 30;
    v.q = quote(path);
    if (!v.q)
        return -1;
    sprintf(v.base, "/tmp/pdfview.%ld", (long)getpid());

    v.n = page_count(&v);
    if (v.n < 1 || load(&v) != 0) {
        fprintf(stderr, "pdfview: cannot read %s (is poppler-utils installed?)\n", path);
        free(v.q);
        return -1;
    }
    v.dpy = XOpenDisplay(NULL);
    if (!v.dpy) {
        free(v.q);
        free(v.img);
        return -1;
    }
    v.win = XCreateSimpleWindow(v.dpy, DefaultRootWindow(v.dpy), 0, 0, 800, 900,
                                0, 0, BG);
    v.gc = XCreateGC(v.dpy, v.win, 0, NULL);
    XSelectInput(v.dpy, v.win, ExposureMask | KeyPressMask);
    wmdel = XInternAtom(v.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(v.dpy, v.win, &wmdel, 1);
    set_title(&v, path);
    XMapWindow(v.dpy, v.win);

    while (running) {
        XNextEvent(v.dpy, &e);
        redraw = 0;
        if (e.type == Expose && e.xexpose.count == 0) {
            redraw = 1;
        } else if (e.type == ClientMessage) {
            if ((Atom)e.xclient.data.l[0] == wmdel)
                running = 0;
        } else if (e.type == KeyPress) {
            XLookupString(&e.xkey, kb, sizeof kb, &ks, NULL);
            switch (ks) {
            case XK_h: v.px += STEP; redraw = 1; break;
            case XK_l: v.px -= STEP; redraw = 1; break;
            case XK_k: v.py += STEP; redraw = 1; break;
            case XK_j: v.py -= STEP; redraw = 1; break;
            case XK_H: redraw = set_page(&v, v.cur - 1, path); break;
            case XK_L: redraw = set_page(&v, v.cur + 1, path); break;
            case XK_plus:
            case XK_equal:
            case XK_KP_Add:
                redraw = set_zoom(&v, v.zoom * ZOOM_F);
                break;
            case XK_minus:
            case XK_KP_Subtract:
                redraw = set_zoom(&v, v.zoom / ZOOM_F);
                break;
            case XK_0:
                redraw = set_zoom(&v, 1.0);
                v.px = 0;
                v.py = 1L << 30;
                break;
            case XK_q:
            case XK_Escape: running = 0; break;
            default: break;
            }
        }
        if (redraw && running)
            draw(&v);
    }

    XFreeGC(v.dpy, v.gc);
    XDestroyWindow(v.dpy, v.win);
    XCloseDisplay(v.dpy);
    free(v.img);
    free(v.q);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s file.pdf\n", argv[0]);
        return 1;
    }
    return run(argv[1]) == 0 ? 0 : 1;
}
