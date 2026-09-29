/****************************************************************************
 * apps/graphics/fpcdesk/desk_bench.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU Bench: benchmarks of the GPU, the link and the compositor, live
 * meters of what the GPU is doing, and demonstrations of what it draws.
 *
 *   desk_bench.c        the window: its pages (the kit), the table of
 *                       results, the meters (/proc/gpu, once a second)
 *   desk_bench_run.c    the benchmarks, measured by the GPU's own clock
 *                       (v6 STATS) where it has one, else by the wall
 *   desk_bench_demo.c   the demonstrations, on the desktop's frames
 *
 * Everything the benchmarks draw goes into a hidden window of their own
 * (a surface in the GPU's memory, made for the run and closed after it),
 * never over other windows.  The GPU's counters, the link's rate and the
 * compositor's frames are the board's kernel functions (a flat build), as
 * deskctl has them.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_BENCH_H
#define __APPS_GRAPHICS_FPCDESK_DESK_BENCH_H

#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BENCH_NWG        48             /* Widgets */
#define BENCH_HIST       60             /* Meters: samples kept (a minute) */
#define BENCH_SDRAM      4              /* Scanout, blitter, DMA, CPU */
#define BENCH_MAXOBJ     200            /* Demos: things that move */

/* The pages */

#define PAGE_BENCH       0
#define PAGE_METERS      1
#define PAGE_DEMOS       2

/* The benchmarks, in the table's order */

enum bench_test_e
{
  BT_FILL = 0,
  BT_COPY,
  BT_BLEND,
  BT_BLEND_RGB,
  BT_BLEND_ARGB,
  BT_BLEND_A8,
  BT_BLEND_LUT,
  BT_BLEND_RAMP,
  BT_BC1,
  BT_EXPAND,
  BT_PUT,
  BT_READ,
  BT_MOVE,
  BT_COMPOSE,
  BT_OPS,
  BT_CMDS,
  BT_COUNT
};

/* A result's state and unit */

#define BR_NONE          0              /* Not run yet */
#define BR_QUEUED        1
#define BR_DONE          2
#define BR_NA            3              /* The GPU cannot */
#define BR_FAILED        4

#define BU_MPX           0              /* Mpixels/s */
#define BU_MBS           1              /* MB/s */
#define BU_FPS           2              /* Frames/s */
#define BU_OPS           3              /* Operations/s */

/* The demonstrations */

#define DEMO_PLASMA      0
#define DEMO_STARS       1
#define DEMO_ORBITS      2
#define DEMO_BLOBS       3
#define DEMO_COUNT       4

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct bench_result_s
{
  uint8_t  state;                       /* BR_* */
  uint8_t  unit;                        /* BU_* */
  bool     gpu;                         /* Timed by the GPU's clock */
  float    rate;                        /* In unit, by the time below */
  float    wall;                        /* In unit, end to end */
  float    us;                          /* An operation's time */
  uint32_t n;                           /* Operations timed */
  char     note[40];
};

/* What the GPU can do and has (found when the window opens) */

struct bench_caps_s
{
  int      version;                     /* Protocol: 4, 5, 6 ... */
  bool     stats;                       /* STATS: its own counters */
  bool     clock;                       /* ... with a running clock */
  bool     bc1;                         /* BLEND from BC1 images */
  uint32_t khz;                         /* Its clock */
  int      wire;                        /* The link's wire, MB/s */
};

/* The meters: /proc/gpu once a second */

struct bench_sample_s
{
  bool     hw;                          /* The GPU's own counters, clocked */
  uint16_t busy;                        /* Blitter, 0.1 % */
  uint16_t sdram[BENCH_SDRAM];          /* 0.1 % each */
  uint32_t link;                        /* Bytes/s */
  uint16_t wire;                        /* Of the wire, 0.01 % */
  uint16_t fps;                         /* Frames, 0.1 /s */
};

struct bench_rect_s
{
  int16_t  x;
  int16_t  y;
  int16_t  w;
  int16_t  h;
};

struct bench_s
{
  struct kit_s kit;
  struct kit_widget_s wg[BENCH_NWG];
  struct kit_list_s list;
  struct desk_win_s *w;
  struct desk_ops_s *o;                 /* Frames and runs: our own list */
  struct bench_caps_s caps;
  int      page;

  /* The benchmarks */

  struct bench_result_s res[BT_COUNT];
  uint8_t  queue[BT_COUNT];
  uint8_t  nqueue;
  uint8_t  qpos;
  bool     running;
  bool     runshown;                    /* The next one shown as running */
  struct kit_surface_s scratch;         /* Where they draw (hidden) */
  bool     scratchok;
  NXWINDOW bc1;                         /* BC1 blocks: an image window */
  sem_t    bc1sem;

  /* The meters */

  struct bench_sample_s hist[BENCH_HIST];
  uint32_t nsamples;
  bool     procgpu;                     /* /proc/gpu was there */

  /* The demonstrations: the one shown, its clock (ms, paused while not
   * shown), what it drew last (to draw again only where things moved)
   */

  int      demo;
  uint32_t t;
  uint32_t last;                        /* desk_ms() of the last frame */
  uint32_t lastdraw;                    /* ... it drew */
  bool     full;                        /* All of it at the next frame */
  struct bench_rect_s stage;            /* The demo's area */
  struct bench_rect_s clip;             /* What is being drawn */
  bool     inkit;                       /* Drawing into the kit's list */
  int      from;                        /* Our list: ops not yet cut */
  int      nobj;
  struct bench_rect_s prev[BENCH_MAXOBJ];
  uint32_t frames;                      /* Drawn since the last tick */
  uint32_t ops;                         /* ... operations */
  uint32_t fps;                         /* Last second's */
  uint32_t opsps;
  uint64_t tot[16];                     /* The driver's pixels by op */
  uint32_t st[41];                      /* The GPU's counters (v6) */
  uint32_t statms;                      /* desk_ms() of the last second's */
  uint32_t gpuload;                     /* 0.1 %: last second */
  bool     gpumeasured;                 /* ... from its clock */

  char     status[160];
  char     info[4][48];
  char     demoinfo[160];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* desk_bench_run.c */

void bench_caps(struct bench_caps_s *c);
const char *bench_test_name(int t);
int  bench_run(struct bench_s *b, int t);         /* One, into b->res[t] */
void bench_run_end(struct bench_s *b);            /* The scratch goes */
void bench_format(const struct bench_result_s *r, int col, char *buf,
                  int size);
void bench_detail(const struct bench_s *b, int t, char *buf, int size);

/* The driver's totals of pixels by op, and the GPU's counters (v6) */

void bench_totals(uint64_t px[16]);
bool bench_hwstats(uint32_t st[41]);
uint32_t bench_est_us(const uint64_t a[16], const uint64_t b[16]);

/* desk_bench_demo.c */

const char *bench_demo_name(int demo);
const char *bench_demo_about(int demo);
void bench_demo_start(struct bench_s *b);
void bench_demo_draw(struct bench_s *b, int x0, int y0, int x1, int y1);
bool bench_demo_frame(struct bench_s *b);         /* false: nothing due */

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_BENCH_H */
