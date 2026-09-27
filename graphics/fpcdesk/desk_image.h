/****************************************************************************
 * apps/graphics/fpcdesk/desk_image.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Images in the GPU's memory, for the Image Viewer and Paint.  An image is
 * a hidden window: NX gives it a surface of the GPU's space, so drawing it
 * is a COPY (or a BLEND, to fade it) from there, and nothing crosses the
 * link but the command.  Files are decoded on the CPU a strip of rows at a
 * time, scaled down on the way if they are larger than asked for or than
 * the GPU has room for, and put into the surface; a whole image is never
 * held in memory.
 *
 * The pixels are RGB565 for now.  Everything that shows an image goes
 * through img_copy(), img_blend() and img_zoom(), so an image kept
 * compressed in the GPU (BC1, decoded by BLEND) needs only a different
 * store in img_load() and a different source mode there.  With BC1
 * (protocol v6: NX_CAP_BC1, image windows NXBE_WINDOW_IMAGE): img_open()
 * opens an image window of w x h/4 (w, h rounded up to 4), img_load()
 * encodes each four rows of the strip into blocks (the wallpaper's
 * bc1_block()) and puts a row of blocks a row, and img_copy() becomes a
 * BLEND from NX_BLEND_SRC_BC1 (img_blend() the same with alpha) at the
 * image's pixel coordinates.  Paint's canvas stays RGB565: it is drawn
 * into, and read back.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_IMAGE_H
#define __APPS_GRAPHICS_FPCDESK_DESK_IMAGE_H

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IMG_MAXPIX      (1024 * 1024)    /* The most pixels an image keeps */
#define IMG_MAXDIM      16384            /* Larger files are refused */
#define IMG_SPACE       2048             /* The GPU's space: images are
                                          * no larger a side */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct desk_image_s
{
  NXWINDOW hwnd;                         /* Hidden window: the pixels */
  int w;                                 /* Its size */
  int h;
  int fw;                                /* The file's size, 0: not a file */
  int fh;
  bool gpu;                              /* In the GPU's memory (else in
                                          * PSRAM: slow to draw from) */
  bool armed;                            /* Made: a redraw now means lost */
  volatile bool lost;                    /* Its pixels are gone: no room
                                          * after a display mode change */
  struct desk_win_s *owner;              /* Woken when they are lost */
  sem_t sem;                             /* Synch and block handshakes */
  char fmt[24];                          /* "BMP, 24-bit" ... */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A name with an image's extension (.bmp, .ppm, .pgm, .pbm, .pnm) */

bool img_is_image(const char *name);

/* The file's size and format, from its header; 0 or a negated errno
 * (err says why, for people)
 */

int  img_info(const char *path, int *w, int *h, char *fmt, int fmtlen,
              char *err, int errlen);

/* Decode a file into a new image no larger than maxw x maxh (keeping its
 * aspect; never enlarged), smaller if the GPU has no room for that.
 * Returns the image, or NULL (err says why).  owner (or NULL) is woken
 * if the image is lost later.
 */

struct desk_image_s *img_load(const char *path, int maxw, int maxh,
                              struct desk_win_s *owner, char *err,
                              int errlen);

/* A new image of one colour */

struct desk_image_s *img_new(int w, int h, uint16_t c,
                             struct desk_win_s *owner);

void img_free(struct desk_image_s *img);

/* Pixels to and from the CPU (RGB565, stride in bytes).  img_read() reads
 * back from the GPU: about 1 MB/s on the board.
 */

int  img_read(struct desk_image_s *img, int x, int y, int w, int h,
              uint16_t *buf, int stride);
int  img_write(struct desk_image_s *img, int x, int y, int w, int h,
               const uint16_t *buf, int stride);

/* Save a rectangle of it as a 24-bit BMP (read back a strip at a time) */

int  img_save_bmp(struct desk_image_s *img, const char *path);

/* Drawing it into a window (operations for nx_gpuops() on that window):
 * the rectangle (sx, sy, w, h) of the image at (dx, dy); faded in with
 * alpha; or enlarged z times (nearest neighbour: COPYs of columns, then
 * of rows), cut to the clip rectangle [cx0, cx1) x [cy0, cy1), sent to
 * hwnd as the list fills.
 */

void img_copy(struct desk_ops_s *o, struct desk_image_s *img, int sx,
              int sy, int w, int h, int dx, int dy);
void img_blend(struct desk_ops_s *o, struct desk_image_s *img, int sx,
               int sy, int w, int h, int dx, int dy, uint8_t alpha);
void img_zoom(NXWINDOW hwnd, struct desk_ops_s *o, struct desk_image_s *img,
              int dx, int dy, int z, int cx0, int cy0, int cx1, int cy1);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_IMAGE_H */
