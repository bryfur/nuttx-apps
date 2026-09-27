/****************************************************************************
 * apps/graphics/fpcdesk/desk_viewer.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Image Viewer: BMP, PPM, PGM and PBM files (desk_image.c), opened from
 * Files or by typing a path.  The image lives in the GPU's memory (a
 * hidden window) and is drawn into the viewer's window by the GPU:
 *
 *   - Fit: decoded to the size of the view (averaging the pixels, since
 *     the GPU cannot shrink an image), centred on a dark backdrop, one
 *     COPY;
 *   - 100% and 200%: decoded at full size (or as large as there is room
 *     for), 200% enlarged by COPYs of columns and rows; dragging pans by
 *     a COPY of the view within the window and draws only what comes
 *     into view;
 *   - the images of the folder in turn (arrow keys, the buttons) and a
 *     slideshow: a thread wakes the viewer every few seconds; it decodes
 *     the next image and fades it in over the current one, a BLEND a step.
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "desk_ui.h"
#include "desk_image.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VIEW_W         720              /* Default view */
#define VIEW_H         480
#define VIEW_MINW      520
#define VIEW_MINH      300

#define TB_H           44               /* Tool bar */
#define ST_H           28               /* Status bar */
#define BTN_H          28

#define C_BACKDROP     RGB(0x10, 0x10, 0x12)

#define ZOOM_FIT       0
#define ZOOM_100       1
#define ZOOM_200       2

#define SLIDE_MS       3500             /* An image shows this long */
#define FADE_MS        40               /* A step of the fade */
#define FADE_N         10
#define DBLCLICK_MS    400
#define MAX_NAMES      1024

/* Zones */

#define ZG_TOOL        0
#define ZG_CARD        1

#define Z_PREV         1
#define Z_NEXT         2
#define Z_FIT          3
#define Z_100          4
#define Z_200          5
#define Z_SHOW         6
#define Z_OPEN         7
#define Z_EDIT         8
#define Z_OK           9
#define Z_CANCEL       10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vlay_s
{
  int cw;
  int vx;                               /* The view, frame coordinates */
  int vy;
  int vw;
  int vh;
  int sy;                               /* The status bar */
};

struct viewer_s
{
  char path[PATH_MAX];                  /* The image shown ("": none) */
  char **names;                         /* The folder's images, sorted */
  int nnames;
  int idx;                              /* path's among them, -1 */

  struct desk_image_s *fit;             /* At the view's size */
  struct desk_image_s *full;            /* At full size (or the most the
                                         * GPU has room for) */
  struct desk_image_s *next;            /* The slideshow's next */
  int nextidx;
  int fw;                               /* The file's size */
  int fh;
  char fmt[24];
  char err[160];                        /* Instead of an image */

  int zoom;                             /* ZOOM_* */
  int px;                               /* Panned: the view's top-left in */
  int py;                               /* the enlarged image */

  /* The slideshow: a thread wakes the window (desk_wake()), then waits
   * until the wake has been handled (ack) and delay is up to date
   */

  bool show;
  int fade;                             /* Step of the fade, 0: none */
  pthread_t thread;
  bool thread_on;
  volatile bool quit;
  volatile int delay;
  sem_t ack;

  /* The pointer and the keyboard */

  struct ui_zones_s zs;
  uint8_t buttons;
  bool drag;
  int grabx;
  int graby;
  int grabpx;
  int grabpy;
  uint32_t clickms;
  bool prompt;                          /* The Open card */
  struct ui_field_s field;
  struct ui_keys_s keys;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;
static const char *g_openpath;

/****************************************************************************
 * Private Functions: layout
 ****************************************************************************/

static void layout(struct desk_win_s *w, struct vlay_s *l)
{
  l->cw = w->csize.w;
  l->vx = 0;
  l->vy = TITLE_H + TB_H;
  l->vw = w->csize.w;
  l->sy = TITLE_H + w->csize.h - ST_H;
  l->vh = l->sy - l->vy;
  if (l->vh < 1)
    {
      l->vh = 1;
    }
}

static const char *base_name(const char *path)
{
  const char *s = strrchr(path, '/');

  return s != NULL ? s + 1 : path;
}

/* The image the view shows now (NULL: none), and how many times larger */

static struct desk_image_s *shown(struct viewer_s *v)
{
  if (v->zoom == ZOOM_FIT)
    {
      return v->fit;
    }

  /* A small image's fit is its full size */

  if (v->full == NULL && v->fit != NULL && v->fit->w == v->fw &&
      v->fit->h == v->fh)
    {
      return v->fit;
    }

  return v->full;
}

static int zfactor(struct viewer_s *v)
{
  return v->zoom == ZOOM_200 ? 2 : 1;
}

/* Where the shown image goes (frame coordinates of its top-left): centred
 * where it is smaller than the view, else panned (px, py kept in range)
 */

static void place(struct viewer_s *v, struct vlay_s *l,
                  struct desk_image_s *img, int z, int *dx, int *dy)
{
  int zw = img->w * z;
  int zh = img->h * z;

  if (zw <= l->vw)
    {
      v->px = 0;
      *dx = l->vx + (l->vw - zw) / 2;
    }
  else
    {
      v->px = v->px < 0 ? 0 : v->px > zw - l->vw ? zw - l->vw : v->px;
      *dx = l->vx - v->px;
    }

  if (zh <= l->vh)
    {
      v->py = 0;
      *dy = l->vy + (l->vh - zh) / 2;
    }
  else
    {
      v->py = v->py < 0 ? 0 : v->py > zh - l->vh ? zh - l->vh : v->py;
      *dy = l->vy - v->py;
    }
}

static bool can_pan(struct viewer_s *v, struct vlay_s *l)
{
  struct desk_image_s *img = shown(v);

  return img != NULL && v->err[0] == '\0' &&
         (img->w * zfactor(v) > l->vw || img->h * zfactor(v) > l->vh);
}

/****************************************************************************
 * Private Functions: the tool bar, the status bar, the Open card (UI)
 ****************************************************************************/

static int tb_button(struct viewer_s *v, int x, int y, int id,
                     const char *label, bool enabled, bool lit)
{
  int bw = label != NULL ? ui_button_width(label) : 32;
  int lk = ui_look(&v->zs, id, enabled);

  if (lit && lk == UI_NORMAL)
    {
      lk = UI_DEFAULT;
    }

  ui_button(&g_o, x, y, bw, BTN_H, label, lk);
  ui_zone_add(&v->zs, ZG_TOOL, id, x, y, bw, BTN_H, enabled);
  return x + bw + 6;
}

static void toolbar_draw(struct desk_win_s *w, struct vlay_s *l)
{
  struct viewer_s *v = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int y = TITLE_H + (TB_H - BTN_H) / 2;
  bool many = v->nnames > 1;
  bool have = v->path[0] != '\0' && v->err[0] == '\0';
  int x = 10;
  int rx;

  ui_zones_clear(&v->zs, ZG_TOOL);
  ui_flush(hwnd, &g_o, 120);
  ops_fill(&g_o, 0, TITLE_H, l->cw, TB_H, C_UI_BAR);
  ops_fill(&g_o, 0, TITLE_H + TB_H - 1, l->cw, 1, C_UI_LINE);

  tb_button(v, x, y, Z_PREV, NULL, many, false);
  ui_triangle(&g_o, x + 15, y + BTN_H / 2, 6, 2, many ? C_TEXT : C_TEXT_DIM,
              255);
  x += 38;
  tb_button(v, x, y, Z_NEXT, NULL, many, false);
  ui_triangle(&g_o, x + 17, y + BTN_H / 2, 6, 3, many ? C_TEXT : C_TEXT_DIM,
              255);
  x += 38 + 10;

  x = tb_button(v, x, y, Z_FIT, "Fit", have, v->zoom == ZOOM_FIT);
  x = tb_button(v, x, y, Z_100, "100%", have, v->zoom == ZOOM_100);
  x = tb_button(v, x, y, Z_200, "200%", have, v->zoom == ZOOM_200);
  x += 10;
  tb_button(v, x, y, Z_SHOW, v->show ? "Stop" : "Slideshow", many,
            v->show);

  rx = l->cw - 10 - ui_button_width("Edit");
  if (rx > x + ui_button_width("Slideshow") + 10)
    {
      tb_button(v, rx, y, Z_EDIT, "Edit", have, false);
      rx -= 6 + ui_button_width("Open" DESK_CH_ELLIPSIS);
      tb_button(v, rx, y, Z_OPEN, "Open" DESK_CH_ELLIPSIS, true, false);
    }

  ops_draw(hwnd, &g_o);
}

static void status_draw(struct desk_win_s *w, struct vlay_s *l)
{
  struct viewer_s *v = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  struct desk_image_s *img = shown(v);
  int ty = l->sy + (ST_H - text_height(font)) / 2;
  char left[PATH_MAX + 64];
  char right[64];
  char fit[128];
  int rw;

  right[0] = '\0';
  if (v->err[0] != '\0')
    {
      snprintf(left, sizeof(left), "%s", v->err);
    }
  else if (v->path[0] == '\0')
    {
      snprintf(left, sizeof(left), "No image");
    }
  else
    {
      snprintf(left, sizeof(left), "%s  " DESK_CH_MIDDOT "  %d x %d  "
               DESK_CH_MIDDOT "  %s", base_name(v->path), v->fw, v->fh,
               v->fmt);
      if (img != NULL && v->fw > 0)
        {
          int pct = (int)((int64_t)img->w * zfactor(v) * 100 / v->fw);

          snprintf(right, sizeof(right), "%s%s%d%%", v->show ?
                   "Slideshow    " : "", v->zoom == ZOOM_FIT ? "Fit, " :
                   "", pct);
        }

      if (v->idx >= 0 && v->nnames > 1)
        {
          size_t n = strlen(right);

          snprintf(right + n, sizeof(right) - n, "    %d / %d", v->idx + 1,
                   v->nnames);
        }
    }

  ui_flush(hwnd, &g_o, 140);
  ops_fill(&g_o, 0, l->sy, l->cw, ST_H, C_UI_BAR);
  ops_fill(&g_o, 0, l->sy, l->cw, 1, C_UI_LINE);
  rw = text_width(font, right);
  ops_text(&g_o, font, l->cw - 14 - rw, ty, right, C_TEXT_DIM, 255);
  ui_fit(font, left, l->cw - 40 - rw, fit, sizeof(fit));
  ops_text(&g_o, font, 12, ty, fit, v->err[0] != '\0' ? C_UI_ERROR :
           C_TEXT_DIM, 255);
  ops_draw(hwnd, &g_o);
}

/* The Open card, over the middle of the view */

static void card_rect(struct vlay_s *l, int *x, int *y, int *cw, int *ch)
{
  *cw = 440 < l->vw - 24 ? 440 : l->vw - 24;
  *ch = 128;
  *x = l->vx + (l->vw - *cw) / 2;
  *y = l->vy + (l->vh - *ch) / 2;
  *y = *y < l->vy + 4 ? l->vy + 4 : *y;
}

static void card_draw(struct desk_win_s *w, struct vlay_s *l)
{
  struct viewer_s *v = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int x;
  int y;
  int cw;
  int ch;
  int bx;
  int by;
  int bw;

  ui_zones_clear(&v->zs, ZG_CARD);
  if (!v->prompt)
    {
      return;
    }

  card_rect(l, &x, &y, &cw, &ch);
  by = y + ch - 44;
  bx = x + cw - 16;
  ui_flush(hwnd, &g_o, 140);
  ops_rrect(&g_o, x - 1, y - 1, cw + 2, ch + 2, LUT_R12, 12, C_UI_LINE, 255);
  ops_rrect(&g_o, x, y, cw, ch, LUT_R12, 12, C_CARD, 255);
  ops_text(&g_o, &g_desk_font_uib, x + 18, y + 14, "Open an image", C_TEXT,
           255);
  ui_field_draw(&g_o, &v->field, x + 16, y + 38, cw - 32, 30, true);

  bw = ui_button_width("Open");
  bx -= bw;
  ui_button(&g_o, bx, by, bw, BTN_H, "Open", v->field.len == 0 ?
            UI_DISABLED : ui_look(&v->zs, Z_OK, true) == UI_PRESSED ?
            UI_PRESSED : UI_DEFAULT);
  ui_zone_add(&v->zs, ZG_CARD, Z_OK, bx, by, bw, BTN_H, v->field.len > 0);
  bw = ui_button_width("Cancel");
  bx -= bw + 8;
  ui_button(&g_o, bx, by, bw, BTN_H, "Cancel",
            ui_look(&v->zs, Z_CANCEL, true));
  ui_zone_add(&v->zs, ZG_CARD, Z_CANCEL, bx, by, bw, BTN_H, true);
  ops_draw(hwnd, &g_o);
}

/* A message in the middle of the view (no image, or a bad one) */

static void message_draw(struct desk_win_s *w, struct vlay_s *l)
{
  struct viewer_s *v = w->priv;
  const struct desk_font_s *f = &g_desk_font_ui;
  int cx = l->vx + l->vw / 2;
  int cy = l->vy + l->vh / 2;
  char fit[160];

  /* A picture's outline: a frame, a hill and a sun */

  ops_rrect(&g_o, cx - 28, cy - 62, 56, 44, LUT_R6, 6, C_TEXT_DIM, 90);
  ops_rrect(&g_o, cx - 25, cy - 59, 50, 38, LUT_R3, 3, C_BACKDROP, 255);
  ops_disc(&g_o, cx + 10, cy - 48, LUT_R3, 3, C_TEXT_DIM, 110);
  ui_triangle(&g_o, cx - 6, cy - 30, 12, 0, C_TEXT_DIM, 90);

  if (v->err[0] != '\0')
    {
      ui_fit(&g_desk_font_uib, base_name(v->path), l->vw - 40, fit,
             sizeof(fit));
      ops_text_centre(&g_o, &g_desk_font_uib, cx, cy - 4, fit, C_TEXT, 255);
      ui_fit(f, v->err, l->vw - 40, fit, sizeof(fit));
      ops_text_centre(&g_o, f, cx, cy + 18, fit, C_UI_ERROR, 255);
    }
  else
    {
      ops_text_centre(&g_o, &g_desk_font_uib, cx, cy - 4, "No image",
                      C_TEXT, 255);
      ops_text_centre(&g_o, f, cx, cy + 18, "Open one from Files, or with "
                      "Open" DESK_CH_ELLIPSIS " (BMP, PPM, PGM)",
                      C_TEXT_DIM, 255);
    }

  ui_clip(&g_o, 0, l->vx, l->vy, l->vx + l->vw, l->vy + l->vh);
}

/****************************************************************************
 * Private Functions: the view
 ****************************************************************************/

/* The view, all of it */

static void view_draw(struct desk_win_s *w, struct vlay_s *l)
{
  struct viewer_s *v = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_image_s *img = shown(v);
  int dx;
  int dy;

  ops_draw(hwnd, &g_o);
  ops_fill(&g_o, l->vx, l->vy, l->vw, l->vh, C_BACKDROP);
  if (img == NULL || v->err[0] != '\0')
    {
      message_draw(w, l);
    }
  else
    {
      place(v, l, img, zfactor(v), &dx, &dy);
      img_zoom(hwnd, &g_o, img, dx, dy, zfactor(v), l->vx, l->vy,
               l->vx + l->vw, l->vy + l->vh);
    }

  ops_draw(hwnd, &g_o);
  card_draw(w, l);
}

/* Pan to (px, py): what stays in view moves with one COPY within the
 * window; the strips that come into view are drawn
 */

static void view_pan(struct desk_win_s *w, struct vlay_s *l, int px, int py)
{
  struct viewer_s *v = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_image_s *img = shown(v);
  struct nx_gpuop_s *op;
  int z = zfactor(v);
  int ox = v->px;
  int oy = v->py;
  int ddx;
  int ddy;
  int dx;
  int dy;
  int x0;
  int x1;
  int y0;
  int y1;

  if (img == NULL || v->prompt)
    {
      return;
    }

  v->px = px;
  v->py = py;
  place(v, l, img, z, &dx, &dy);
  ddx = v->px - ox;                     /* The content moves by -dd */
  ddy = v->py - oy;
  if (ddx == 0 && ddy == 0)
    {
      return;
    }

  if (ddx <= -l->vw || ddx >= l->vw || ddy <= -l->vh || ddy >= l->vh)
    {
      view_draw(w, l);
      return;
    }

  /* What stays */

  op = ops_add(&g_o);
  op->op = NX_GPUOP_COPY;
  op->sx = l->vx + (ddx > 0 ? ddx : 0);
  op->sy = l->vy + (ddy > 0 ? ddy : 0);
  op->x  = l->vx + (ddx > 0 ? 0 : -ddx);
  op->y  = l->vy + (ddy > 0 ? 0 : -ddy);
  op->w  = l->vw - (ddx > 0 ? ddx : -ddx);
  op->h  = l->vh - (ddy > 0 ? ddy : -ddy);

  /* What comes in: a band across (rows) and one down the rest (columns) */

  y0 = ddy > 0 ? l->vy + l->vh - ddy : l->vy;
  y1 = ddy > 0 ? l->vy + l->vh : l->vy - ddy;
  if (ddy != 0)
    {
      ops_fill(&g_o, l->vx, y0, l->vw, y1 - y0, C_BACKDROP);
      img_zoom(hwnd, &g_o, img, dx, dy, z, l->vx, y0, l->vx + l->vw, y1);
    }

  x0 = ddx > 0 ? l->vx + l->vw - ddx : l->vx;
  x1 = ddx > 0 ? l->vx + l->vw : l->vx - ddx;
  y0 = ddy > 0 ? l->vy : l->vy - ddy;
  y1 = ddy > 0 ? l->vy + l->vh - ddy : l->vy + l->vh;
  if (ddx != 0)
    {
      ui_flush(hwnd, &g_o, 1);
      ops_fill(&g_o, x0, y0, x1 - x0, y1 - y0, C_BACKDROP);
      img_zoom(hwnd, &g_o, img, dx, dy, z, x0, y0, x1, y1);
    }

  ops_draw(hwnd, &g_o);
}

/* A step of the slideshow's fade: the next image (centred) blended over
 * the current one with alpha a, the backdrop around it blended the same
 */

static void fade_draw(struct desk_win_s *w, struct vlay_s *l, uint8_t a)
{
  struct viewer_s *v = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_image_s *cur = v->err[0] == '\0' ? v->fit : NULL;
  struct desk_image_s *nx = v->next;
  int nx0 = l->vx + (l->vw - nx->w) / 2;
  int ny0 = l->vy + (l->vh - nx->h) / 2;
  int nx1 = nx0 + nx->w;
  int ny1 = ny0 + nx->h;
  int vx1 = l->vx + l->vw;
  int vy1 = l->vy + l->vh;

  ops_draw(hwnd, &g_o);
  ops_fill(&g_o, l->vx, l->vy, l->vw, l->vh, C_BACKDROP);
  if (cur != NULL)
    {
      img_copy(&g_o, cur, 0, 0, cur->w, cur->h,
               l->vx + (l->vw - cur->w) / 2, l->vy + (l->vh - cur->h) / 2);
    }

  ops_blend(&g_o, l->vx, l->vy, l->vw, ny0 - l->vy, C_BACKDROP, a);
  ops_blend(&g_o, l->vx, ny1, l->vw, vy1 - ny1, C_BACKDROP, a);
  ops_blend(&g_o, l->vx, ny0, nx0 - l->vx, nx->h, C_BACKDROP, a);
  ops_blend(&g_o, nx1, ny0, vx1 - nx1, nx->h, C_BACKDROP, a);
  img_blend(&g_o, nx, 0, 0, nx->w, nx->h, nx0, ny0, a);
  ui_clip(&g_o, 0, l->vx, l->vy, vx1, vy1);
  ops_draw(hwnd, &g_o);
}

/****************************************************************************
 * Private Functions: images and the folder
 ****************************************************************************/

static void set_title(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;

  snprintf(w->title, sizeof(w->title), "%s", v->path[0] != '\0' ?
           base_name(v->path) : "Image Viewer");
  desk_win_title(w);
}

static void images_free(struct viewer_s *v)
{
  img_free(v->fit);
  img_free(v->full);
  img_free(v->next);
  v->fit = NULL;
  v->full = NULL;
  v->next = NULL;
}

static void names_free(struct viewer_s *v)
{
  int i;

  for (i = 0; i < v->nnames; i++)
    {
      free(v->names[i]);
    }

  free(v->names);
  v->names = NULL;
  v->nnames = 0;
  v->idx = -1;
}

static int name_cmp(const void *a, const void *b)
{
  return strcasecmp(*(char * const *)a, *(char * const *)b);
}

/* The images in path's folder, sorted by name, and path's place */

static void names_load(struct viewer_s *v)
{
  char dir[PATH_MAX];
  const char *base = base_name(v->path);
  struct dirent *de;
  char *slash;
  DIR *d;
  int cap = 0;
  int i;

  names_free(v);
  strlcpy(dir, v->path, sizeof(dir));
  slash = strrchr(dir, '/');
  if (slash == NULL)
    {
      return;
    }

  *(slash == dir ? slash + 1 : slash) = '\0';
  d = opendir(dir);
  if (d == NULL)
    {
      return;
    }

  while ((de = readdir(d)) != NULL && v->nnames < MAX_NAMES)
    {
      char *s;

      if (de->d_name[0] == '.' || !img_is_image(de->d_name))
        {
          continue;
        }

      if (v->nnames == cap)
        {
          char **n = realloc(v->names, (cap + 32) * sizeof(char *));

          if (n == NULL)
            {
              break;
            }

          v->names = n;
          cap += 32;
        }

      s = strdup(de->d_name);
      if (s == NULL)
        {
          break;
        }

      v->names[v->nnames++] = s;
    }

  closedir(d);
  if (v->nnames > 0)
    {
      qsort(v->names, v->nnames, sizeof(char *), name_cmp);
    }

  for (i = 0; i < v->nnames; i++)
    {
      if (strcmp(v->names[i], base) == 0)
        {
          v->idx = i;
        }
    }
}

/* The folder's image i, as a path */

static void name_path(struct viewer_s *v, int i, char *buf, size_t n)
{
  char dir[PATH_MAX];
  char *slash;

  strlcpy(dir, v->path, sizeof(dir));
  slash = strrchr(dir, '/');
  if (slash != NULL)
    {
      *slash = '\0';
    }

  if (snprintf(buf, n, "%s/%s", dir, v->names[i]) >= (int)n)
    {
      buf[0] = '\0';                     /* Too long: none */
    }
}

/* The fit size of a w x h image in the view */

static void fit_size(struct vlay_s *l, int w, int h, int *tw, int *th)
{
  *tw = w;
  *th = h;
  if (*tw > l->vw)
    {
      *th = (int)((int64_t)*th * l->vw / *tw);
      *tw = l->vw;
    }

  if (*th > l->vh)
    {
      *tw = (int)((int64_t)*tw * l->vh / *th);
      *th = l->vh;
    }
}

/* What the zoom needs, decoded (an error goes to v->err) */

static void ensure(struct desk_win_s *w, struct vlay_s *l)
{
  struct viewer_s *v = w->priv;
  struct desk_image_s *img;

  if (v->path[0] == '\0' || v->err[0] != '\0' || shown(v) != NULL)
    {
      return;
    }

  /* The other size goes first: the GPU's room is for the one shown */

  if (v->zoom == ZOOM_FIT)
    {
      img_free(v->full);
      v->full = NULL;
    }
  else
    {
      img_free(v->fit);
      v->fit = NULL;
    }

  img = img_load(v->path, v->zoom == ZOOM_FIT ? l->vw : IMG_MAXDIM,
                 v->zoom == ZOOM_FIT ? l->vh : IMG_MAXDIM, w, v->err,
                 sizeof(v->err));
  if (img == NULL)
    {
      return;
    }

  v->fw = img->fw;
  v->fh = img->fh;
  strlcpy(v->fmt, img->fmt, sizeof(v->fmt));
  if (v->zoom == ZOOM_FIT)
    {
      v->fit = img;
    }
  else
    {
      v->full = img;
    }
}

/* Show the image at path (list its folder again if it is another one) */

static void show_path(struct desk_win_s *w, const char *path, bool relist)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;

  images_free(v);
  v->fade = 0;
  v->err[0] = '\0';
  v->fw = 0;
  v->fh = 0;
  v->fmt[0] = '\0';
  if (path != v->path)
    {
      strlcpy(v->path, path, sizeof(v->path));
    }

  if (relist)
    {
      names_load(v);
    }

  layout(w, &l);
  ensure(w, &l);

  /* Enlarged, it starts at the middle */

  if (shown(v) != NULL)
    {
      v->px = (shown(v)->w * zfactor(v) - l.vw) / 2;
      v->py = (shown(v)->h * zfactor(v) - l.vh) / 2;
    }

  set_title(w);
  toolbar_draw(w, &l);
  view_draw(w, &l);
  status_draw(w, &l);
}

static void go(struct desk_win_s *w, int delta)
{
  struct viewer_s *v = w->priv;
  char path[PATH_MAX];
  int i;

  if (v->nnames == 0)
    {
      return;
    }

  i = v->idx < 0 ? 0 : (v->idx + delta + v->nnames) % v->nnames;
  name_path(v, i, path, sizeof(path));
  v->idx = i;
  show_path(w, path, false);
}

static void set_zoom(struct desk_win_s *w, int zoom)
{
  struct viewer_s *v = w->priv;
  struct desk_image_s *img;
  struct vlay_s l;
  int oz = zfactor(v);
  int cx;
  int cy;

  if (v->path[0] == '\0' || v->err[0] != '\0' || zoom == v->zoom)
    {
      return;
    }

  /* Keep the middle of the view where it was */

  layout(w, &l);
  img = shown(v);
  cx = 500;
  cy = 500;
  if (img != NULL)
    {
      int dx;
      int dy;

      place(v, &l, img, oz, &dx, &dy);
      cx = (l.vx + l.vw / 2 - dx) * 1000 / (img->w * oz);
      cy = (l.vy + l.vh / 2 - dy) * 1000 / (img->h * oz);
    }

  v->zoom = zoom;
  ensure(w, &l);
  img = shown(v);
  if (img != NULL)
    {
      v->px = cx * img->w * zfactor(v) / 1000 - l.vw / 2;
      v->py = cy * img->h * zfactor(v) / 1000 - l.vh / 2;
    }

  toolbar_draw(w, &l);
  view_draw(w, &l);
  status_draw(w, &l);
}

/****************************************************************************
 * Private Functions: the slideshow
 ****************************************************************************/

static void *slide_thread(void *arg)
{
  struct desk_win_s *w = arg;
  struct viewer_s *v = w->priv;

  while (!v->quit)
    {
      int ms = v->delay;

      while (ms > 0 && !v->quit)
        {
          usleep((ms > 50 ? 50 : ms) * 1000);
          ms -= 50;
        }

      if (v->quit)
        {
          break;
        }

      desk_wake(w);
      sem_wait(&v->ack);
    }

  return NULL;
}

static void show_stop(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;

  if (v->thread_on)
    {
      v->quit = true;
      sem_post(&v->ack);
      pthread_join(v->thread, NULL);
      v->thread_on = false;
    }

  v->show = false;
  if (v->fade > 0)
    {
      /* Stopped in a fade: the next one is shown */

      v->fade = FADE_N;
    }
}

static void show_start(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  pthread_attr_t attr;

  if (v->nnames < 2 || v->thread_on)
    {
      return;
    }

  v->show = true;
  v->quit = false;
  v->delay = SLIDE_MS;
  while (sem_trywait(&v->ack) == 0)
    {
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2048);
  if (pthread_create(&v->thread, &attr, slide_thread, w) == 0)
    {
      v->thread_on = true;
    }
  else
    {
      v->show = false;
    }

  pthread_attr_destroy(&attr);
}

/* The fade is over: the next image is the current one */

static void fade_end(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  char path[PATH_MAX];

  img_free(v->fit);
  img_free(v->full);
  v->full = NULL;
  v->fit = v->next;
  v->next = NULL;
  v->fade = 0;
  v->err[0] = '\0';
  v->idx = v->nextidx;
  v->fw = v->fit->fw;
  v->fh = v->fit->fh;
  strlcpy(v->fmt, v->fit->fmt, sizeof(v->fmt));
  name_path(v, v->idx, path, sizeof(path));
  strlcpy(v->path, path, sizeof(v->path));
  set_title(w);
}

/* A slideshow step: the next image decoded and a fade begun, or a step
 * of the fade
 */

static void show_step(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;
  char path[PATH_MAX];
  char err[8];
  int tries;

  layout(w, &l);
  if (v->fade == 0)
    {
      if (v->zoom != ZOOM_FIT)
        {
          v->zoom = ZOOM_FIT;
          ensure(w, &l);
          toolbar_draw(w, &l);
        }

      /* The next one that opens */

      for (tries = 0; tries < v->nnames && v->next == NULL; tries++)
        {
          v->nextidx = ((v->nextidx > v->idx ? v->nextidx : v->idx) + 1) %
                       v->nnames;
          if (v->nextidx == v->idx)
            {
              break;
            }

          name_path(v, v->nextidx, path, sizeof(path));
          v->next = img_load(path, l.vw, l.vh, w, err, sizeof(err));
        }

      if (v->next == NULL)
        {
          v->nextidx = v->idx;
          v->delay = SLIDE_MS;
          return;
        }

      v->fade = 1;
    }
  else
    {
      v->fade++;
    }

  if (v->fade < FADE_N)
    {
      fade_draw(w, &l, (uint8_t)(255 * v->fade / FADE_N));
      v->delay = FADE_MS;
      return;
    }

  fade_end(w);
  view_draw(w, &l);
  status_draw(w, &l);
  v->delay = SLIDE_MS;
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

static void prompt_open(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;
  char p[PATH_MAX];
  char *slash;

  if (v->show)
    {
      show_stop(w);
    }

  if (v->path[0] != '\0')
    {
      strlcpy(p, v->path, sizeof(p));
      slash = strrchr(p, '/');
      if (slash != NULL)
        {
          slash[1] = '\0';
        }
    }
  else
    {
      const char *home = getenv("HOME");

      if (home == NULL || strcmp(home, "/") == 0)
        {
          home = access("/mnt/sd0", F_OK) == 0 ? "/mnt/sd0" : "/tmp";
        }

      snprintf(p, sizeof(p), "%s/", home);
    }

  ui_field_set(&v->field, p);
  v->prompt = true;
  v->zs.hot = 0;
  v->zs.pressed = 0;
  layout(w, &l);
  card_draw(w, &l);
}

static void prompt_end(struct desk_win_s *w, bool ok)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;
  char path[UI_FIELD_MAX];

  v->prompt = false;
  ui_zones_clear(&v->zs, ZG_CARD);
  layout(w, &l);
  if (ok && v->field.len > 0)
    {
      strlcpy(path, v->field.text, sizeof(path));
      show_path(w, path, true);
      return;
    }

  view_draw(w, &l);
}

static void action(struct desk_win_s *w, int id)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;

  if (v->fade > 0 && id != Z_SHOW)
    {
      layout(w, &l);
      fade_end(w);
      view_draw(w, &l);
    }

  switch (id)
    {
      case Z_PREV:
      case Z_NEXT:
        if (v->show)
          {
            show_stop(w);
          }

        go(w, id == Z_PREV ? -1 : 1);
        return;

      case Z_FIT:
        set_zoom(w, ZOOM_FIT);
        return;

      case Z_100:
        if (v->show)
          {
            show_stop(w);
          }

        set_zoom(w, ZOOM_100);
        return;

      case Z_200:
        if (v->show)
          {
            show_stop(w);
          }

        set_zoom(w, ZOOM_200);
        return;

      case Z_SHOW:
        if (v->show)
          {
            show_stop(w);
            if (v->fade > 0)
              {
                layout(w, &l);
                fade_end(w);
                view_draw(w, &l);
              }
          }
        else
          {
            set_zoom(w, ZOOM_FIT);
            show_start(w);
          }

        layout(w, &l);
        toolbar_draw(w, &l);
        status_draw(w, &l);
        return;

      case Z_OPEN:
        prompt_open(w);
        return;

      case Z_EDIT:
        if (v->path[0] != '\0')
          {
            desk_paint_open(v->path);
          }
        return;

      case Z_OK:
      case Z_CANCEL:
        prompt_end(w, id == Z_OK);
        return;

      default:
        return;
    }
}

static void set_hot(struct desk_win_s *w, int id)
{
  struct viewer_s *v = w->priv;
  int g0 = ui_zone_group(&v->zs, v->zs.hot);
  int g1 = ui_zone_group(&v->zs, id);
  struct vlay_s l;

  if (id == v->zs.hot)
    {
      return;
    }

  v->zs.hot = id;
  layout(w, &l);
  if (g0 == ZG_TOOL || g1 == ZG_TOOL)
    {
      toolbar_draw(w, &l);
    }

  if (g0 == ZG_CARD || g1 == ZG_CARD)
    {
      card_draw(w, &l);
    }
}

static void viewer_mouse(struct desk_win_s *w, int x, int cy,
                         uint8_t buttons)
{
  struct viewer_s *v = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(v->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (v->buttons & NX_MOUSE_LEFTBUTTON);
  struct ui_zone_s *z;
  struct vlay_s l;
  int y = cy + TITLE_H;

  v->buttons = buttons;
  layout(w, &l);

  if (x < 0 && cy < 0 && buttons == 0)
    {
      set_hot(w, 0);
      v->zs.pressed = 0;
      v->drag = false;
      return;
    }

  if (v->drag)
    {
      if (buttons & NX_MOUSE_LEFTBUTTON)
        {
          view_pan(w, &l, v->grabpx - (x - v->grabx),
                   v->grabpy - (y - v->graby));
        }
      else
        {
          v->drag = false;
        }

      return;
    }

  z = ui_zone_at(&v->zs, x, y);
  if (v->prompt && z != NULL && z->group != ZG_CARD)
    {
      z = NULL;
    }

  if (!(buttons & NX_MOUSE_LEFTBUTTON) || press)
    {
      set_hot(w, z != NULL && z->enabled ? z->id : 0);
    }

  if (press)
    {
      if (z != NULL)
        {
          if (z->enabled)
            {
              v->zs.pressed = z->id;
              v->zs.hot = 0;
              set_hot(w, z->id);
            }
        }
      else if (!v->prompt && y >= l.vy && y < l.vy + l.vh &&
               v->path[0] != '\0' && v->err[0] == '\0')
        {
          uint32_t now = desk_ms();

          /* A double click: fit, or 100% at the point clicked */

          if (now - v->clickms < DBLCLICK_MS)
            {
              struct desk_image_s *img = shown(v);
              int dx;
              int dy;
              int iw;
              int ih;

              v->clickms = 0;
              if (v->zoom != ZOOM_FIT || img == NULL)
                {
                  set_zoom(w, ZOOM_FIT);
                  return;
                }

              place(v, &l, img, 1, &dx, &dy);
              iw = img->w;
              ih = img->h;
              v->zoom = ZOOM_100;
              ensure(w, &l);                    /* img may go */
              if (shown(v) != NULL && iw > 0 && ih > 0)
                {
                  v->px = (x - dx) * shown(v)->w / iw - (x - l.vx);
                  v->py = (y - dy) * shown(v)->h / ih - (y - l.vy);
                }

              toolbar_draw(w, &l);
              view_draw(w, &l);
              status_draw(w, &l);
              return;
            }

          v->clickms = now;
          if (can_pan(v, &l))
            {
              v->drag = true;
              v->grabx = x;
              v->graby = y;
              v->grabpx = v->px;
              v->grabpy = v->py;
            }
        }
    }

  if (release && v->zs.pressed != 0)
    {
      int id = v->zs.pressed;

      v->zs.pressed = 0;
      if (z != NULL && z->id == id)
        {
          v->zs.hot = 0;
          action(w, id);
          if (!w->closing)
            {
              set_hot(w, id);
            }
        }
      else
        {
          toolbar_draw(w, &l);
          card_draw(w, &l);
        }
    }
}

static void viewer_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;
  int keys[16];
  int nk = ui_keys(&v->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk && !w->closing; i++)
    {
      int k = keys[i];

      layout(w, &l);
      if (v->prompt)
        {
          if (k == '\r' || k == '\n')
            {
              prompt_end(w, true);
            }
          else if (k == KEY_ESC)
            {
              prompt_end(w, false);
            }
          else if (ui_field_key(&v->field, k))
            {
              card_draw(w, &l);
            }

          continue;
        }

      switch (k)
        {
          case KEY_LEFT:
          case KEY_RIGHT:
          case KEY_PGUP:
          case KEY_PGDN:
            action(w, k == KEY_LEFT || k == KEY_PGUP ? Z_PREV : Z_NEXT);
            break;

          case KEY_HOME:
          case KEY_END:
            if (v->nnames > 0)
              {
                if (v->show)
                  {
                    show_stop(w);
                  }

                v->idx = k == KEY_HOME ? 1 : v->nnames - 2;
                go(w, k == KEY_HOME ? -1 : 1);
              }
            break;

          case KEY_UP:
          case KEY_DOWN:
            view_pan(w, &l, v->px, v->py + (k == KEY_UP ? -64 : 64));
            break;

          case 'f':
          case '0':
            action(w, Z_FIT);
            break;

          case '1':
            action(w, Z_100);
            break;

          case '2':
            action(w, Z_200);
            break;

          case '+':
          case '=':
            action(w, v->zoom == ZOOM_FIT ? Z_100 : Z_200);
            break;

          case '-':
            action(w, v->zoom == ZOOM_200 ? Z_100 : Z_FIT);
            break;

          case ' ':
          case 's':
            action(w, Z_SHOW);
            break;

          case KEY_ESC:
            if (v->show)
              {
                action(w, Z_SHOW);
              }
            break;

          case 'e':
            action(w, Z_EDIT);
            break;

          case KEY_CTL('o'):
            action(w, Z_OPEN);
            break;

          default:
            break;
        }
    }
}

/* The slideshow's thread, or an image that has lost its pixels */

static void viewer_wake(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;

  if ((v->fit != NULL && v->fit->lost) ||
      (v->full != NULL && v->full->lost))
    {
      img_free(v->fit);
      img_free(v->full);
      v->fit = NULL;
      v->full = NULL;
      layout(w, &l);
      ensure(w, &l);
      view_draw(w, &l);
    }

  if (v->next != NULL && v->next->lost)
    {
      img_free(v->next);
      v->next = NULL;
      v->fade = 0;
    }

  if (v->thread_on)
    {
      if (v->show && !v->prompt)
        {
          show_step(w);
        }

      sem_post(&v->ack);
    }
}

static void viewer_draw(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;

  if (v == NULL)
    {
      return;
    }

  layout(w, &l);
  ops_reset(&g_o);
  toolbar_draw(w, &l);
  view_draw(w, &l);
  status_draw(w, &l);
}

/* A new size: a fitted image is decoded again if its size changes */

static void viewer_resize(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;
  struct vlay_s l;
  int tw;
  int th;

  layout(w, &l);
  if (v->fade > 0)
    {
      fade_end(w);
    }

  if (v->fit != NULL && v->zoom == ZOOM_FIT)
    {
      fit_size(&l, v->fw, v->fh, &tw, &th);
      if (tw != v->fit->w || th != v->fit->h)
        {
          img_free(v->fit);
          v->fit = NULL;
          ensure(w, &l);
        }
    }

  viewer_draw(w);
}

static void viewer_close(struct desk_win_s *w)
{
  struct viewer_s *v = w->priv;

  if (v == NULL)
    {
      return;
    }

  show_stop(w);
  images_free(v);
  names_free(v);
  sem_destroy(&v->ack);
}

static void viewer_icon(struct desk_ops_s *o, int x, int y, int s,
                        uint8_t a)
{
  icon_tile(o, x, y, s, RGB(0x5c, 0xc8, 0xfa), RGB(0x1e, 0x6e, 0xd8), a);

  /* A photograph: a white frame, a sky, hills and a sun */

  ops_rrect(o, x + s / 6, y + s / 4, s * 2 / 3, s / 2, LUT_R3, 3, C_WHITE,
            a);
  ops_fill(o, x + s / 6 + 3, y + s / 4 + 3, s * 2 / 3 - 6, s / 2 - 6,
           RGB(0x9c, 0xd4, 0xff));
  ops_disc(o, x + s / 2 + s / 7, y + s / 4 + s / 8 + 1, LUT_R3, 3,
           RGB(0xff, 0xd2, 0x3c), a);
  ui_triangle(o, x + s / 2 - s / 10, y + s * 3 / 4 - 3 - s / 10, s / 5, 0,
              RGB(0x34, 0xa8, 0x53), a);
  ui_triangle(o, x + s / 2 + s / 8, y + s * 3 / 4 - 3 - s / 16, s / 8, 0,
              RGB(0x2a, 0x8c, 0x44), a);
}

static struct desk_win_s *viewer_open(void)
{
  const char *path = g_openpath;
  struct viewer_s *v;
  struct desk_win_s *w;
  char fmt[24];
  char err[8];
  int vw = VIEW_W;
  int vh = VIEW_H;
  int iw;
  int ih;

  v = calloc(1, sizeof(*v));
  if (v == NULL)
    {
      return NULL;
    }

  v->idx = -1;
  sem_init(&v->ack, 0, 0);

  /* The window takes the image's size, within reason */

  if (path != NULL &&
      img_info(path, &iw, &ih, fmt, sizeof(fmt), err, sizeof(err)) == 0)
    {
      int maxw = DESK_W - 120;
      int maxh = WORK_H - TITLE_H - TB_H - ST_H - 40;

      vw = iw > maxw ? maxw : iw < VIEW_MINW ? VIEW_MINW : iw;
      vh = ih > maxh ? maxh : ih < VIEW_MINH ? VIEW_MINH : ih;
    }

  w = desk_win_open(APP_VIEWER, "Image Viewer", vw, vh + TB_H + ST_H);
  if (w == NULL)
    {
      sem_destroy(&v->ack);
      free(v);
      return NULL;
    }

  w->priv = v;
  if (path != NULL)
    {
      show_path(w, path, true);
    }
  else
    {
      viewer_draw(w);
    }

  return w;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_viewer_open(const char *path)
{
  struct desk_win_s *w = desk_app_window(APP_VIEWER);

  if (w != NULL && path != NULL)
    {
      struct viewer_s *v = w->priv;

      if (v->show)
        {
          show_stop(w);
        }

      if (v->prompt)
        {
          v->prompt = false;
          ui_zones_clear(&v->zs, ZG_CARD);
        }

      desk_win_restore(w);
      show_path(w, path, true);
      return 0;
    }

  g_openpath = path;
  desk_launch(APP_VIEWER);
  g_openpath = NULL;
  return 0;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_viewer =
{
  .name   = "Image Viewer",
  .icon   = viewer_icon,
  .open   = viewer_open,
  .draw   = viewer_draw,
  .resize = viewer_resize,
  .mouse  = viewer_mouse,
  .key    = viewer_key,
  .wake   = viewer_wake,
  .close  = viewer_close,
  .single = true,
};
