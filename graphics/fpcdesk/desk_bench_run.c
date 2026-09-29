/****************************************************************************
 * apps/graphics/fpcdesk/desk_bench_run.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU Bench: the benchmarks (desk_bench.h).  Each draws into a hidden
 * window of its own, the scratch: a 512 x 256 surface in the GPU's memory,
 * made for a run and closed after it, so nothing is drawn over another
 * window.  The GPU operations go the way every application's do
 * (nx_gpuops(), nx_bitmap(), nx_getrectangle()), batches of them until
 * BENCH_MIN_MS have gone by, each batch waited for with a READ of one
 * pixel (it waits for all the drawing before it).
 *
 * Time: a v6 GPU with a clock counts its blitter's busy cycles (STATS),
 * read before and after: that is the GPU's time for the work, whatever
 * the link and the CPU did.  Without (the simulator's reference model has
 * no clock, a v5 GPU no STATS) the wall clock times the batches, end to
 * end.  The link, the readback and the commands a second are timed by the
 * wall clock anyway: that is what they are about.
 *
 * The compositor: the window itself moved round a small circle, a frame
 * at a time (the compositor's frame counter), and back where it was; the
 * background colour changed and changed back, which composites all of the
 * display each time (as deskctl redraw does).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/nx/nxbe.h>

#include "desk_bench.h"

#ifdef CONFIG_FPCDESK_BENCH

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BENCH_MIN_US     300000         /* A test runs this long, at least */
#define MAX_BATCHES      2000
#define SCR_W            512            /* The scratch surface */
#define SCR_H            256
#define TILE             256            /* An operation: 256 x 256 */
#define BATCH            16             /* ... 16 a batch */
#define BMP_W            256            /* EXPAND, PUT: 256 x 128 */
#define BMP_H            128
#define READ_H           32             /* READ: 256 x 32 (16 KB) */
#define OPS_N            256            /* nx_gpuops(): a full list */
#define CMDS_N           2048           /* Driver commands a batch */
#define MOVE_FRAMES      36             /* Moving: frames round the circle */
#define MOVE_R           12             /* ... its radius */
#define COMPOSE_N        8              /* Whole display: even (restores) */
#define FRAME_WAIT_US    500000

/* The STATS block's words (docs/gpu.md) */

#define ST_WORDS         41
#define ST_CLK_KHZ       1
#define ST_CYCLES        2
#define ST_BLIT_BUSY     29

/* The driver's ops (FPC_GPU_*) in its totals */

#define OP_FILL          0x01
#define OP_PUT           0x02
#define OP_COPY          0x03
#define OP_EXPAND        0x07
#define OP_BLEND         0x09
#define OP_BLENDIMG      15

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The driver (kernel functions of the board: a flat build), as
 * fpc/drivers/gpu/fpc_gpu.h has them
 */

struct fpc_gpu_totals_s
{
  uint64_t cmds[16];
  uint64_t pixels[16];
  uint64_t bytes[16];
  uint64_t linkbytes;
  uint32_t packets;
  uint64_t putpx[3];
  uint64_t putbytes;
  uint64_t readyus;
  uint32_t reads;
  uint64_t readpx;
  uint64_t readus;
};

int  fpc_gpu_version(void);
int  fpc_gpu_hwstats(uint32_t st[ST_WORDS]);
int  fpc_link_wire_mbps(void);
void fpc_gpu_copy(int sx, int sy, int dx, int dy, int w, int h);
void fpc_gpu_flush(void);
int  fpc_gpu_read(int x, int y, int w, int h, uint16_t *dst, size_t stride);
void fpc_gpu_totals(struct fpc_gpu_totals_s *t);

/* A measurement: from its start */

struct meas_s
{
  uint32_t t0;
  bool     hw;
  uint32_t st[ST_WORDS];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct
{
  const char *name;
  uint8_t unit;
  const char *what;                     /* An operation, for the status */
} g_tests[BT_COUNT] =
{
  { "FILL 256 x 256",             BU_MPX, "FILLs of 256 x 256" },
  { "COPY 256 x 256",             BU_MPX, "COPYs of 256 x 256" },
  { "BLEND colour",               BU_MPX, "BLENDs of a colour, 256 x 256" },
  { "BLEND RGB565 image",         BU_MPX, "BLENDs of an RGB565 image" },
  { "BLEND ARGB4444 image",       BU_MPX, "BLENDs of an ARGB4444 image" },
  { "BLEND A8 tint",              BU_MPX, "BLENDs of a colour through A8" },
  { "BLEND with a shape table",   BU_MPX, "BLENDs through a shape table" },
  { "BLEND with a ramp",          BU_MPX, "BLENDs along a ramp" },
  { "BLEND BC1 image",            BU_MPX, "BLENDs from BC1 blocks" },
  { "EXPAND (text) 256 x 128",    BU_MPX, "two-colour bitmaps (EXPAND)" },
  { "Link: PUT 64 KB",            BU_MBS, "PUTs of 256 x 128 pixels" },
  { "Readback: READ 16 KB",       BU_MBS, "READs of 256 x 32 pixels" },
  { "Compositor: moving a window", BU_FPS, "frames of the window moving" },
  { "Compositor: all the display", BU_MPX, "composites of all the display" },
  { "nx_gpuops(): commands",      BU_OPS, "1 x 1 FILLs through NX" },
  { "Driver: commands",           BU_OPS, "1 x 1 COPYs to the driver" },
};

/* GPU time of the system simulation's costs (deskctl stats), ps a pixel */

static const struct
{
  uint8_t op;
  uint32_t pspx;
} g_costs[] =
{
  { OP_FILL,     9714 },
  { OP_COPY,     19086 },
  { OP_BLEND,    20000 },
  { OP_BLENDIMG, 27143 },
  { OP_EXPAND,   5000 },
};

/****************************************************************************
 * Private Functions: time
 ****************************************************************************/

static uint32_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

/* Everything sent so far drawn: a READ of one pixel of the scratch waits
 * for the NX server to have done what came before it, and the GPU too
 */

static void sync_gpu(struct bench_s *b)
{
  struct nxgl_rect_s r;
  uint16_t px[2];

  memset(&r, 0, sizeof(r));
  nx_getrectangle(b->scratch.hwnd, &r, 0, (uint8_t *)px, sizeof(px));
}

static void meas_begin(struct bench_s *b, struct meas_s *m)
{
  sync_gpu(b);
  m->hw = b->caps.clock && fpc_gpu_hwstats(m->st) >= 0;
  m->t0 = now_us();
}

/* The wall time since the start, and with the GPU's clock the time its
 * blitter was busy (us; 0 without)
 */

static void meas_end(struct bench_s *b, struct meas_s *m, uint32_t *wall,
                     uint32_t *busy)
{
  uint32_t st[ST_WORDS];

  sync_gpu(b);
  *wall = now_us() - m->t0;
  *wall = *wall > 0 ? *wall : 1;
  *busy = 0;
  if (m->hw && fpc_gpu_hwstats(st) >= 0 && st[ST_CLK_KHZ] != 0)
    {
      uint32_t cyc = st[ST_BLIT_BUSY] - m->st[ST_BLIT_BUSY];

      *busy = (uint32_t)((uint64_t)cyc * 1000 / st[ST_CLK_KHZ]);
    }
}

static bool elapsed(struct meas_s *m, uint32_t n)
{
  return now_us() - m->t0 >= BENCH_MIN_US || n >= MAX_BATCHES;
}

/* n operations of amount each (pixels, bytes, frames, commands) took wall
 * us, and busy us of the GPU's time (0: not known)
 */

static void result(struct bench_result_s *r, int unit, uint32_t n,
                   float amount, uint32_t wall, uint32_t busy)
{
  float total = (float)n * amount;
  uint32_t t = busy > 0 ? busy : wall;

  r->state = BR_DONE;
  r->unit  = unit;
  r->n     = n;
  r->gpu   = busy > 0;
  r->wall  = total * 1e6f / (float)wall;
  r->rate  = total * 1e6f / (float)t;
  r->us    = n > 0 ? (float)t / (float)n : 0;
  strlcpy(r->note, r->gpu ? "GPU clock" : "wall clock", sizeof(r->note));
}

/* The compositor has made a frame since it had made f */

static bool wait_frame(uint32_t f)
{
  struct nx_compose_stats_s cs;
  uint32_t t0 = now_us();

  for (; ; )
    {
      nxbe_compose_stats(&cs);
      if (cs.frames != f)
        {
          return true;
        }

      if (now_us() - t0 > FRAME_WAIT_US)
        {
          return false;
        }

#ifdef CONFIG_ARCH_SIM
      usleep(1000);                     /* Its time moves only while idle */
#else
      sched_yield();                    /* The server is above us */
#endif
    }
}

static int iround(float v)
{
  return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

static uint32_t frames_now(void)
{
  struct nx_compose_stats_s cs;

  nxbe_compose_stats(&cs);
  return cs.frames;
}

/****************************************************************************
 * Private Functions: the surfaces
 ****************************************************************************/

static void bc1_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                       bool more, void *arg)
{
}

static void bc1_event(NXWINDOW hwnd, enum nx_event_e event, void *arg1,
                      void *arg2)
{
  struct bench_s *b = arg2 != NULL ? arg2 : arg1;

  if (event == NXEVENT_SYNCHED)
    {
      sem_post(&b->bc1sem);
    }
}

static const struct nx_callback_s g_bc1cb =
{
  bc1_redraw, NULL, NULL, NULL, bc1_event
};

/* BC1 blocks of a TILE x TILE image: an image window of TILE x TILE / 4,
 * a row of blocks to a row, each block two colours along a gradient and
 * a pattern of their four
 */

static int bc1_open(struct bench_s *b)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  struct nxgl_rect_s dest;
  struct timespec ts;
  const void *src[1];
  uint16_t *buf;
  int bx;
  int by;

  if (b->bc1 != NULL)
    {
      return 0;
    }

  buf = malloc(TILE * (TILE / 4) * 2);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  sem_init(&b->bc1sem, 0, 0);
  b->bc1 = nx_openwindow(g_desk.hnx, NXBE_WINDOW_RAMBACKED |
                         NXBE_WINDOW_HIDDEN | NXBE_WINDOW_IMAGE, &g_bc1cb,
                         b);
  if (b->bc1 == NULL)
    {
      sem_destroy(&b->bc1sem);
      free(buf);
      return -ENOMEM;
    }

  size.w = TILE;
  size.h = TILE / 4;
  pos.x  = 0;
  pos.y  = 0;
  nx_setsize(b->bc1, &size);
  nx_setposition(b->bc1, &pos);
  if (nx_synch(b->bc1, b) == 0)
    {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;
      while (sem_timedwait(&b->bc1sem, &ts) < 0 && errno == EINTR)
        {
        }
    }

  for (by = 0; by < TILE / 4; by++)
    {
      for (bx = 0; bx < TILE / 4; bx++)
        {
          uint16_t *p = &buf[by * TILE + bx * 4];
          uint16_t c0 = RGB(255 - by * 3, 80 + bx * 2, 120 + by);
          uint16_t c1 = RGB(bx * 3, 40, 200 - bx * 2);

          p[0] = c0 > c1 ? c0 : c1;     /* Four colours, none clear */
          p[1] = c0 > c1 ? c1 : c0;
          p[2] = (uint16_t)(0x1b1b ^ (bx * 0x0101));
          p[3] = (uint16_t)(0xe4e4 ^ (by * 0x0101));
        }
    }

  dest.pt1.x = 0;
  dest.pt1.y = 0;
  dest.pt2.x = TILE - 1;
  dest.pt2.y = TILE / 4 - 1;
  pos.x  = 0;
  pos.y  = 0;
  src[0] = buf;
  nx_bitmap(b->bc1, &dest, src, &pos, TILE * 2);
  free(buf);
  return 0;
}

static void bc1_close(struct bench_s *b)
{
  if (b->bc1 != NULL)
    {
      nx_closewindow(b->bc1);
      sem_destroy(&b->bc1sem);
      b->bc1 = NULL;
    }
}

/* The scratch: a gradient to draw from (image sources see its pixels) */

static int scratch_open(struct bench_s *b)
{
  int ret;

  if (b->scratch.hwnd != NULL)
    {
      return b->scratchok ? 0 : -ENOSPC;
    }

  ret = kit_surface_open(&b->scratch, SCR_W, SCR_H, NULL);
  if (ret < 0)
    {
      b->scratch.hwnd = NULL;
      return ret;
    }

  b->scratchok = ret == 0;
  ops_reset(b->o);
  ops_hgrad(b->o, 0, 0, SCR_W, SCR_H, RGB(0x10, 0x30, 0x80),
            RGB(0xf0, 0x90, 0x30), 255);
  ops_vgrad(b->o, 0, 0, SCR_W, SCR_H, RGB(0x20, 0x20, 0x20),
            RGB(0x60, 0xf0, 0x90), 128);
  kit_surface_draw(&b->scratch, b->o);
  return b->scratchok ? 0 : -ENOSPC;
}

/****************************************************************************
 * Private Functions: the tests
 ****************************************************************************/

/* A batch of GPU operations of TILE x TILE in the scratch */

static void gpu_batch(struct bench_s *b, int t, uint32_t n)
{
  struct desk_ops_s *o = b->o;
  int i;

  ops_reset(o);
  for (i = 0; i < BATCH; i++)
    {
      struct nx_gpuop_s *op = ops_add(o);
      uint16_t c = (uint16_t)((n * BATCH + i) * 0x1863u);

      op->op     = NX_GPUOP_BLEND;
      op->x      = TILE;                /* Image sources: left to right */
      op->y      = 0;
      op->w      = TILE;
      op->h      = TILE;
      op->colour = c;
      switch (t)
        {
          case BT_FILL:
            op->op = NX_GPUOP_FILL;
            op->x  = 0;
            break;

          case BT_COPY:
            op->op = NX_GPUOP_COPY;
            break;

          case BT_BLEND:
            op->x     = 0;
            op->mode  = NX_BLEND_SRC_COLOUR;
            op->alpha = 128;
            break;

          case BT_BLEND_RGB:
            op->mode  = NX_BLEND_SRC_RGB;
            op->alpha = 200;
            break;

          case BT_BLEND_ARGB:
            op->mode  = NX_BLEND_SRC_ARGB4444;
            break;

          case BT_BLEND_A8:
            op->mode  = NX_BLEND_SRC_A8;
            break;

          case BT_BLEND_LUT:
            op->x     = 0;
            op->mode  = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT;
            op->slot  = LUT_R12;
            op->alpha = 200;
            op->p[0]  = 12;
            op->p[1]  = 12;
            op->p[2]  = TILE - 13;
            op->p[3]  = TILE - 13;
            break;

          case BT_BLEND_RAMP:
            op->x     = 0;
            op->mode  = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_HRAMP |
                        NX_BLEND_DITHER;
            op->p[0]  = 0;
            op->p[1]  = 256;            /* 0 to 255 across */
            op->p[2]  = 0;
            break;

          case BT_BC1:
            op->src   = b->bc1;
            op->mode  = NX_BLEND_SRC_BC1;
            break;

          default:
            break;
        }
    }

  nx_gpuops(b->scratch.hwnd, o->op, o->n);
}

static int run_gpu(struct bench_s *b, int t, struct bench_result_s *r)
{
  struct meas_s m;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;

  if (t == BT_BC1 && (!b->caps.bc1 || bc1_open(b) < 0))
    {
      r->state = BR_NA;
      strlcpy(r->note, "needs a v6 GPU", sizeof(r->note));
      return 0;
    }

  meas_begin(b, &m);
  do
    {
      gpu_batch(b, t, n++);
      sync_gpu(b);
    }
  while (!elapsed(&m, n));

  meas_end(b, &m, &wall, &busy);
  result(r, BU_MPX, n * BATCH, TILE * TILE, wall, busy);
  return 0;
}

/* EXPAND and PUT: nx_bitmap() of a 256 x 128 picture of two colours (the
 * driver sends it as EXPANDs, 1 bit a pixel) or of noise (as pixels)
 */

static int run_bitmap(struct bench_s *b, int t, struct bench_result_s *r)
{
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  const void *src[1];
  struct meas_s m;
  uint32_t seed = 0x9e3779b9u;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;
  uint16_t *buf;
  int i;

  buf = malloc(BMP_W * BMP_H * 2);
  if (buf == NULL)
    {
      r->state = BR_FAILED;
      strlcpy(r->note, "no memory", sizeof(r->note));
      return -ENOMEM;
    }

  for (i = 0; i < BMP_W * BMP_H; i++)
    {
      seed = seed * 1664525u + 1013904223u;
      if (t == BT_EXPAND)
        {
          buf[i] = (seed >> 28) < 5 ? RGB(0xf0, 0xf0, 0xf0) :
                                      RGB(0x20, 0x24, 0x30);
        }
      else
        {
          buf[i] = (uint16_t)(seed >> 16);
        }
    }

  dest.pt1.x = 0;
  dest.pt1.y = 0;
  dest.pt2.x = BMP_W - 1;
  dest.pt2.y = BMP_H - 1;
  origin.x   = 0;
  origin.y   = 0;
  src[0]     = buf;

  meas_begin(b, &m);
  do
    {
      nx_bitmap(b->scratch.hwnd, &dest, src, &origin, BMP_W * 2);
      n++;
      sync_gpu(b);
    }
  while (!elapsed(&m, n));

  meas_end(b, &m, &wall, &busy);
  free(buf);
  if (t == BT_EXPAND)
    {
      result(r, BU_MPX, n, BMP_W * BMP_H, wall, busy);
    }
  else
    {
      int share;

      result(r, BU_MBS, n, BMP_W * BMP_H * 2, wall, 0);
      share = b->caps.wire > 0 ?
              (int)(r->rate / 1e6f * 100 / b->caps.wire + 0.5f) : 0;
      snprintf(r->note, sizeof(r->note), "%d %% of %d MB/s", share,
               b->caps.wire);
    }

  return 0;
}

static int run_read(struct bench_s *b, struct bench_result_s *r)
{
  struct nxgl_rect_s rect;
  struct meas_s m;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;
  uint16_t *buf;

  buf = malloc(TILE * READ_H * 2);
  if (buf == NULL)
    {
      r->state = BR_FAILED;
      strlcpy(r->note, "no memory", sizeof(r->note));
      return -ENOMEM;
    }

  rect.pt1.x = 0;
  rect.pt1.y = 0;
  rect.pt2.x = TILE - 1;
  rect.pt2.y = READ_H - 1;
  meas_begin(b, &m);
  do
    {
      nx_getrectangle(b->scratch.hwnd, &rect, 0, (uint8_t *)buf, TILE * 2);
      n++;
    }
  while (!elapsed(&m, n));

  meas_end(b, &m, &wall, &busy);
  free(buf);
  result(r, BU_MBS, n, TILE * READ_H * 2, wall, 0);
  return 0;
}

/* nx_gpuops(): full lists of 1 x 1 FILLs, as fast as they go through NX */

static int run_ops(struct bench_s *b, struct bench_result_s *r)
{
  struct desk_ops_s *o = b->o;
  struct meas_s m;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;
  int i;

  ops_reset(o);
  for (i = 0; i < OPS_N; i++)
    {
      ops_fill(o, (i % 32) * 8, (i / 32) * 8, 1, 1, (uint16_t)(i * 0x0841));
    }

  meas_begin(b, &m);
  do
    {
      nx_gpuops(b->scratch.hwnd, o->op, OPS_N);
      n++;
    }
  while (!elapsed(&m, n));

  meas_end(b, &m, &wall, &busy);
  result(r, BU_OPS, n * OPS_N, 1, wall, 0);
  return 0;
}

/* The driver's own rate: COPYs of a pixel of the scratch onto itself
 * (whatever is there stays: the commands run in order), straight to the
 * driver, then a READ of that pixel
 */

static int run_cmds(struct bench_s *b, struct bench_result_s *r)
{
  struct nxbe_window_s *wnd = (struct nxbe_window_s *)b->scratch.hwnd;
  struct meas_s m;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;
  uint16_t px[2];
  int x;
  int y;
  int i;

  if (!wnd->surface)
    {
      r->state = BR_NA;
      strlcpy(r->note, "no surface", sizeof(r->note));
      return 0;
    }

  x = wnd->surfpos.x;
  y = wnd->surfpos.y;
  meas_begin(b, &m);
  do
    {
      for (i = 0; i < CMDS_N; i++)
        {
          fpc_gpu_copy(x, y, x, y, 1, 1);
        }

      fpc_gpu_flush();
      fpc_gpu_read(x, y, 1, 1, px, sizeof(px));
      n++;
    }
  while (!elapsed(&m, n));

  meas_end(b, &m, &wall, &busy);
  result(r, BU_OPS, n * CMDS_N, 1, wall, 0);
  return 0;
}

/* The compositor: the window round a circle of MOVE_R pixels, a frame a
 * step, and back where it started
 */

static int run_move(struct bench_s *b, struct bench_result_s *r)
{
  struct desk_win_s *w = b->w;
  struct nxgl_point_s pos;
  struct meas_s m;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;
  int i;

  meas_begin(b, &m);
  for (i = 1; i <= MOVE_FRAMES; i++)
    {
      float a = 6.2831853f * i / MOVE_FRAMES;
      uint32_t f = frames_now();

      pos.x = w->pos.x + iround(MOVE_R * cosf(a)) - MOVE_R;
      pos.y = w->pos.y + iround(MOVE_R * sinf(a));
      nx_setposition((NXWINDOW)w->hwnd, &pos);
      if (wait_frame(f))
        {
          n++;
        }
    }

  pos = w->pos;
  nx_setposition((NXWINDOW)w->hwnd, &pos);
  meas_end(b, &m, &wall, &busy);
  if (n == 0)
    {
      r->state = BR_FAILED;
      strlcpy(r->note, "no frames", sizeof(r->note));
      return -ETIMEDOUT;
    }

  result(r, BU_FPS, n, 1, wall, busy);
  r->rate = r->wall;                    /* Frames: as they came */
  strlcpy(r->note, busy > 0 ? "time: GPU clock" : "wall clock",
          sizeof(r->note));
  return 0;
}

/* All the display composited again: the background colour changed (the
 * wallpaper hides it), then back, each time waited for
 */

static int run_compose(struct bench_s *b, struct bench_result_s *r)
{
  nxgl_mxpixel_t c;
  struct meas_s m;
  uint32_t wall;
  uint32_t busy;
  uint32_t n = 0;
  int i;

  meas_begin(b, &m);
  for (i = 0; i < COMPOSE_N; i++)
    {
      uint32_t f = frames_now();

      c = (i & 1) == 0 ? g_theme.desktop ^ 0x0841 : g_theme.desktop;
      nx_setbgcolor(g_desk.hnx, &c);
      if (wait_frame(f))
        {
          n++;
        }

      sync_gpu(b);
    }

  c = g_theme.desktop;
  nx_setbgcolor(g_desk.hnx, &c);
  meas_end(b, &m, &wall, &busy);
  if (n == 0)
    {
      r->state = BR_FAILED;
      strlcpy(r->note, "no frames", sizeof(r->note));
      return -ETIMEDOUT;
    }

  result(r, BU_MPX, n, (float)DESK_W * DESK_H, wall, busy);
  return 0;
}

/* v 3 significant figures */

static void sig3(char *buf, int size, float v)
{
  unsigned long x;

  if (v >= 99.95f)
    {
      snprintf(buf, size, "%lu", (unsigned long)(v + 0.5f));
    }
  else if (v >= 9.995f)
    {
      x = (unsigned long)(v * 10 + 0.5f);
      snprintf(buf, size, "%lu.%lu", x / 10, x % 10);
    }
  else
    {
      x = (unsigned long)(v * 100 + 0.5f);
      snprintf(buf, size, "%lu.%02lu", x / 100, x % 100);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void bench_caps(struct bench_caps_s *c)
{
  uint32_t a[ST_WORDS];
  uint32_t z[ST_WORDS];

  memset(c, 0, sizeof(*c));
  c->version = fpc_gpu_version();
  c->bc1     = (nx_compositor(g_desk.hnx) & NX_CAP_BC1) != 0;
  c->wire    = fpc_link_wire_mbps();
  if (c->version >= 6 && fpc_gpu_hwstats(a) >= 0)
    {
      c->stats = true;
      c->khz   = a[ST_CLK_KHZ];
      usleep(2000);
      c->clock = fpc_gpu_hwstats(z) >= 0 && z[ST_CYCLES] != a[ST_CYCLES] &&
                 c->khz != 0;
    }
}

const char *bench_test_name(int t)
{
  return t >= 0 && t < BT_COUNT ? g_tests[t].name : "";
}

int bench_run(struct bench_s *b, int t)
{
  struct bench_result_s *r = &b->res[t];
  int ret;

  memset(r, 0, sizeof(*r));
  r->unit = g_tests[t].unit;
  ret = scratch_open(b);
  if (ret < 0)
    {
      r->state = BR_FAILED;
      strlcpy(r->note, ret == -ENOSPC ? "no room in the GPU" : "no surface",
              sizeof(r->note));
      return ret;
    }

  switch (t)
    {
      case BT_EXPAND:
      case BT_PUT:
        return run_bitmap(b, t, r);

      case BT_READ:
        return run_read(b, r);

      case BT_MOVE:
        return run_move(b, r);

      case BT_COMPOSE:
        return run_compose(b, r);

      case BT_OPS:
        return run_ops(b, r);

      case BT_CMDS:
        return run_cmds(b, r);

      default:
        return run_gpu(b, t, r);
    }
}

void bench_run_end(struct bench_s *b)
{
  bc1_close(b);
  if (b->scratch.hwnd != NULL)
    {
      kit_surface_close(&b->scratch);
    }

  b->scratchok = false;
}

/* A result's column: 1 the rate, 2 an operation's time, 3 how measured */

void bench_format(const struct bench_result_s *r, int col, char *buf,
                  int size)
{
  static const char * const units[] =
  {
    "Mpx/s", "MB/s", "frames/s", "/s"
  };

  char v[16];

  buf[0] = '\0';
  if (r->state != BR_DONE)
    {
      if (col == 1)
        {
          strlcpy(buf, r->state == BR_QUEUED ? "Waiting" :
                       r->state == BR_NA ? "Not supported" :
                       r->state == BR_FAILED ? "Failed" : "", size);
        }
      else if (col == 3)
        {
          strlcpy(buf, r->note, size);
        }

      return;
    }

  switch (col)
    {
      case 1:
        if (r->unit == BU_OPS)
          {
            sig3(v, sizeof(v), r->rate >= 1e6f ? r->rate / 1e6f :
                 r->rate / 1e3f);
            snprintf(buf, size, "%s %s/s", v, r->rate >= 1e6f ? "M" : "K");
          }
        else
          {
            sig3(v, sizeof(v), r->unit == BU_FPS ? r->rate :
                 r->rate / 1e6f);
            snprintf(buf, size, "%s %s", v, units[r->unit]);
          }
        break;

      case 2:
        if (r->us >= 1000)
          {
            sig3(v, sizeof(v), r->us / 1000);
            snprintf(buf, size, "%s ms", v);
          }
        else if (r->us >= 1)
          {
            sig3(v, sizeof(v), r->us);
            snprintf(buf, size, "%s us", v);
          }
        else
          {
            snprintf(buf, size, "%lu ns",
                     (unsigned long)(r->us * 1000 + 0.5f));
          }
        break;

      case 3:
        strlcpy(buf, r->note, size);
        break;

      default:
        break;
    }
}

/* The status line of a result: what was timed, and end to end */

void bench_detail(const struct bench_s *b, int t, char *buf, int size)
{
  const struct bench_result_s *r = &b->res[t];
  char rate[24];
  char wall[24];

  if (r->state != BR_DONE)
    {
      snprintf(buf, size, "%s: %s", g_tests[t].name,
               r->state == BR_NONE ? "not run yet (Run, or double click)" :
               r->state == BR_QUEUED ? "waiting" : r->note);
      return;
    }

  bench_format(r, 1, rate, sizeof(rate));
  if (r->unit == BU_OPS || r->unit == BU_FPS)
    {
      snprintf(buf, size, "%s: %lu %s, %s", g_tests[t].name,
               (unsigned long)r->n, g_tests[t].what, rate);
      return;
    }

  sig3(wall, sizeof(wall), r->wall / 1e6f);
  snprintf(buf, size, "%s: %lu %s, %s by the %s; end to end %s %s",
           g_tests[t].name, (unsigned long)r->n, g_tests[t].what, rate,
           r->gpu ? "GPU's clock" : "wall clock", wall,
           r->unit == BU_MBS ? "MB/s" : "Mpx/s");
}

void bench_totals(uint64_t px[16])
{
  static struct fpc_gpu_totals_s t;

  fpc_gpu_totals(&t);
  memcpy(px, t.pixels, sizeof(t.pixels));
}

bool bench_hwstats(uint32_t st[ST_WORDS])
{
  return fpc_gpu_version() >= 6 && fpc_gpu_hwstats(st) >= 0;
}

/* The GPU time the pixels drawn between two totals take, by the system
 * simulation's costs (us)
 */

uint32_t bench_est_us(const uint64_t a[16], const uint64_t b[16])
{
  uint64_t ps = 0;
  size_t i;

  for (i = 0; i < sizeof(g_costs) / sizeof(g_costs[0]); i++)
    {
      ps += (b[g_costs[i].op] - a[g_costs[i].op]) * g_costs[i].pspx;
    }

  return (uint32_t)(ps / 1000000);
}

#endif /* CONFIG_FPCDESK_BENCH */
