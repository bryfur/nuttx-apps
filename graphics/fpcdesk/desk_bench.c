/****************************************************************************
 * apps/graphics/fpcdesk/desk_bench.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU Bench (desk_bench.h): three pages on the widget kit.
 *
 *   Benchmarks  what the GPU is and has (its protocol, its counters, BC1,
 *               the link's wire), and a table of benchmarks: Run (the one
 *               selected, or a double click) or Run All; each gives its
 *               rate, an operation's time and how it was timed, and the
 *               status bar says more of the one selected.  They run one a
 *               frame, so the table shows each as it comes.
 *   Meters      how busy the GPU's blitter is, the SDRAM's time by client,
 *               the link's bytes a second and the compositor's frames,
 *               from /proc/gpu once a second (only while the page shows),
 *               as graphs of the last minute.  The simulator's GPU has no
 *               clock: its cards say so.
 *   Demos       the demonstrations (desk_bench_demo.c) on the desktop's
 *               frames, only while the window is the active one and the
 *               page shows; the status bar says what each costs.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "desk_bench.h"

#ifdef CONFIG_FPCDESK_BENCH

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BENCH_W          720
#define BENCH_H          620
#define GPU_BUFSIZE      2048           /* /proc/gpu */
#define METER_TOP        44             /* A card's title and value */
#define METER_KEY        22             /* ... its key, below the graph */

#define C_GRAPH          THEMED(RGB(0x24, 0x24, 0x28), RGB(0xf4, 0xf4, 0xf7))
#define C_GRID           THEMED(RGB(0x36, 0x36, 0x3b), RGB(0xe0, 0xe0, 0xe5))

/* The widgets' ids */

enum
{
  W_NONE = 0,
  W_PAGES,
  W_DEMOS,
  W_RUN,
  W_RUNALL,
  W_BENCHPAGE,
  W_INFO,                               /* + 0 .. 3 */
  W_TABLE = W_INFO + 4,
  W_METERPAGE,
  W_M_BUSY,
  W_M_SDRAM,
  W_M_LINK,
  W_M_FPS,
  W_STAGE,
  W_STATUS
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_pages[] =
{
  "Benchmarks", "Meters", "Demos"
};

static const char *g_demos[DEMO_COUNT];

static const struct kit_col_s g_cols[] =
{
  { "Test",     0,   0,   0 },
  { "Rate",     116, 0,   KIT_COL_RIGHT },
  { "Time",     84,  0,   KIT_COL_RIGHT | KIT_COL_DIM },
  { "Measured", 150, 560, KIT_COL_DIM },
};

static const char * const g_clients[BENCH_SDRAM] =
{
  "scanout", "blitter", "DMA", "CPU", "codec"
};

static const uint16_t g_clientc[BENCH_SDRAM] =
{
  RGB(0x40, 0x9c, 0xff), RGB(0x30, 0xd1, 0x58), RGB(0xff, 0x9f, 0x0a),
  RGB(0xbf, 0x5a, 0xf2), RGB(0xff, 0x37, 0x5f)
};

/****************************************************************************
 * Private Functions: /proc/gpu
 ****************************************************************************/

/* The numbers after a line's label, in order ("12.3" is 12 and 3), at
 * most max; how many
 */

static int gpu_nums(const char *buf, const char *key, unsigned long *out,
                    int max)
{
  size_t len = strlen(key);
  const char *v = NULL;
  const char *p = buf;
  int n = 0;

  while (p != NULL && *p != '\0')
    {
      if (strncmp(p, key, len) == 0)
        {
          v = p + len;
          break;
        }

      p = strchr(p, '\n');
      p = p != NULL ? p + 1 : NULL;
    }

  while (v != NULL && *v != '\0' && *v != '\n' && n < max)
    {
      if (isdigit((unsigned char)*v))
        {
          unsigned long x = 0;

          while (isdigit((unsigned char)*v))
            {
              x = x * 10 + (*v++ - '0');
            }

          out[n++] = x;
        }
      else
        {
          v++;
        }
    }

  return n;
}

static void bench_sample(struct bench_s *b)
{
  static char buf[GPU_BUFSIZE];
  struct bench_sample_s *s = &b->hist[b->nsamples % BENCH_HIST];
  unsigned long n[12];
  ssize_t total = 0;
  ssize_t r;
  int fd;
  int i;

  memset(s, 0, sizeof(*s));
  fd = open("/proc/gpu", O_RDONLY);
  b->procgpu = fd >= 0;
  if (fd < 0)
    {
      return;
    }

  while (total < GPU_BUFSIZE - 1 &&
         (r = read(fd, buf + total, GPU_BUFSIZE - 1 - total)) > 0)
    {
      total += r;
    }

  close(fd);
  buf[total] = '\0';

  if (gpu_nums(buf, "Frames:", n, 2) == 2)
    {
      s->fps = n[0] * 10 + n[1];
    }

  if (gpu_nums(buf, "Link:", n, 3) == 3)
    {
      s->link = n[0];
      s->wire = n[1] * 100 + n[2];
    }

  /* A v6 GPU with a clock: "GPU: kHz busy b.b % ...", "SDRAM: t.t %
   * scanout a.a blitter b.b DMA c.c CPU d.d codec e.e"; without: "GPU:
   * no clock ..."
   */

  if (gpu_nums(buf, "GPU:", n, 3) == 3)
    {
      s->hw   = true;
      s->busy = n[1] * 10 + n[2];
      if (gpu_nums(buf, "SDRAM:", n, 12) == 12)
        {
          for (i = 0; i < BENCH_SDRAM; i++)
            {
              s->sdram[i] = n[2 + 2 * i] * 10 + n[3 + 2 * i];
            }
        }
    }

  b->nsamples++;
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

/* A rate in bytes: "812 B/s", "12.3 KB/s", "1.20 MB/s" */

static void fmt_bytes(char *buf, int size, uint32_t v)
{
  if (v >= 1000000)
    {
      snprintf(buf, size, "%lu.%02lu MB/s", (unsigned long)(v / 1000000),
               (unsigned long)(v % 1000000 / 10000));
    }
  else if (v >= 1000)
    {
      snprintf(buf, size, "%lu.%lu KB/s", (unsigned long)(v / 1000),
               (unsigned long)(v % 1000 / 100));
    }
  else
    {
      snprintf(buf, size, "%lu B/s", (unsigned long)v);
    }
}

/* A sample's value in a meter (for its graph, of max) */

static uint32_t meter_value(struct bench_sample_s *s, int id, int part)
{
  switch (id)
    {
      case W_M_BUSY:
        return s->busy;

      case W_M_SDRAM:
        return s->sdram[part];

      case W_M_LINK:
        return s->link;

      default:
        return s->fps;
    }
}

/* 1, 2 or 5 times a power of ten, at least v */

static uint32_t nice_max(uint32_t v)
{
  uint32_t m = 1;

  for (; ; )
    {
      if (v <= m)
        {
          return m;
        }

      if (v <= 2 * m)
        {
          return 2 * m;
        }

      if (v <= 5 * m)
        {
          return 5 * m;
        }

      if (m > 400000000)
        {
          return v;
        }

      m *= 10;
    }
}

/* A meter: a card, its title and value, a graph of the last minute (a
 * column a sample, the newest at the right), and for the SDRAM a key
 */

static void meter_draw(struct bench_s *b, struct kit_widget_s *wg)
{
  struct bench_sample_s *now = &b->hist[(b->nsamples + BENCH_HIST - 1) %
                                        BENCH_HIST];
  struct desk_ops_s *o = kit_ops(&b->kit, 32);
  const char *title;
  const char *note = NULL;
  char value[32];
  char caption[48];
  uint32_t max;
  bool clocked = wg->id == W_M_BUSY || wg->id == W_M_SDRAM;
  int count = b->nsamples < BENCH_HIST ? (int)b->nsamples : BENCH_HIST;
  int nparts = wg->id == W_M_SDRAM ? BENCH_SDRAM : 1;
  int gx = wg->x + 14;
  int gy = wg->y + METER_TOP;
  int gw = wg->w - 28;
  int gh = wg->h - METER_TOP - (wg->id == W_M_SDRAM ? METER_KEY : 0) - 14;
  int cw;
  int i;
  int j;
  int p;

  value[0]   = '\0';
  caption[0] = '\0';
  ops_fill(o, wg->x, wg->y, wg->w, wg->h, g_theme.window_bg);
  kit_paint_card(o, wg->x, wg->y, wg->w, wg->h);

  switch (wg->id)
    {
      case W_M_BUSY:
        title = "GPU busy";
        max   = 1000;
        if (count > 0 && now->hw)
          {
            snprintf(value, sizeof(value), "%u.%u %%", now->busy / 10,
                     now->busy % 10);
          }
        break;

      case W_M_SDRAM:
        title = "SDRAM by client";
        max   = 1000;
        if (count > 0 && now->hw)
          {
            uint32_t all = 0;

            for (i = 0; i < BENCH_SDRAM; i++)
              {
                all += now->sdram[i];
              }

            snprintf(value, sizeof(value), "%lu.%lu %%",
                     (unsigned long)(all / 10), (unsigned long)(all % 10));
          }
        break;

      case W_M_LINK:
        title = "Link";
        for (max = 0, i = 0; i < count; i++)
          {
            max = b->hist[i].link > max ? b->hist[i].link : max;
          }

        max = nice_max(max > 1000 ? max : 1000);
        if (count > 0)
          {
            fmt_bytes(value, sizeof(value), now->link);
            snprintf(caption, sizeof(caption), "%u.%02u %% of the wire",
                     now->wire / 100, now->wire % 100);
          }
        break;

      default:
        title = "Frames";
        max   = 600;
        if (count > 0)
          {
            snprintf(value, sizeof(value), "%u.%u /s", now->fps / 10,
                     now->fps % 10);
          }
        break;
    }

  if (!b->procgpu)
    {
      note = "No /proc/gpu";
    }
  else if (clocked && b->caps.stats && !b->caps.clock)
    {
      note = "No clock: the simulator's GPU model has none";
    }
  else if (clocked && !b->caps.stats)
    {
      note = "Needs a v6 GPU (STATS)";
    }

  ops_text(o, KIT_FONT_BOLD, wg->x + 14, wg->y + 14, title, g_theme.text,
           255);
  if (value[0] != '\0' && note == NULL)
    {
      ops_text(o, KIT_FONT_HEADING, wg->x + wg->w - 14 -
               text_width(KIT_FONT_HEADING, value), wg->y + 10, value,
               g_theme.text, 255);
    }

  if (caption[0] != '\0')
    {
      ops_text(o, KIT_FONT_SMALL, wg->x + 14 + text_width(KIT_FONT_BOLD,
               title) + 10, wg->y + 16, caption, g_theme.text_dim, 255);
    }

  if (gw < 20 || gh < 16)
    {
      return;
    }

  o = kit_ops(&b->kit, 8);
  ops_rrect(o, gx, gy, gw, gh, LUT_R6, 6, C_GRAPH, 255);
  for (i = 1; i < 4; i++)
    {
      ops_fill(o, gx + 6, gy + gh * i / 4, gw - 12, 1, C_GRID);
    }

  if (note != NULL)
    {
      ops_text_centre(o, KIT_FONT, gx + gw / 2,
                      gy + (gh - text_height(KIT_FONT)) / 2, note,
                      g_theme.text_dim, 255);
      return;
    }

  /* The columns: sample j of the last count, the newest at the right */

  cw = (gw - 12) / BENCH_HIST;
  cw = cw < 1 ? 1 : cw;
  for (j = 0; j < count; j++)
    {
      struct bench_sample_s *s = &b->hist[(b->nsamples - count + j) %
                                          BENCH_HIST];
      int x = gx + gw - 6 - (count - j) * cw;
      int base = gy + gh - 4;
      int room = gh - 8;

      if (clocked && !s->hw)
        {
          continue;
        }

      o = kit_ops(&b->kit, nparts);
      for (p = 0; p < nparts; p++)
        {
          uint32_t v = meter_value(s, wg->id, p);
          int h = (int)((uint64_t)(v > max ? max : v) * room / max);

          if (v > 0 && h == 0)
            {
              h = 1;
            }

          if (h > 0)
            {
              ops_fill(o, x, base - h, cw > 2 ? cw - 1 : cw, h,
                       nparts > 1 ? g_clientc[p] : g_theme.accent);
              base -= h;
            }
        }
    }

  /* The SDRAM's key */

  if (wg->id == W_M_SDRAM)
    {
      int x = gx;

      for (p = 0; p < BENCH_SDRAM; p++)
        {
          o = kit_ops(&b->kit, 16);
          ops_disc_atlas(o, x, gy + gh + 9, 8, g_clientc[p], 255);
          x += 12;
          x += ops_text(o, KIT_FONT_SMALL, x, gy + gh + 6, g_clients[p],
                        g_theme.text_dim, 255) + 12;
        }
    }
}

static void bench_draw_cb(struct kit_s *k, struct kit_widget_s *wg, int x0,
                          int y0, int x1, int y1)
{
  struct bench_s *b = k->priv;

  if (wg->id == W_STAGE)
    {
      b->stage.x = wg->x;
      b->stage.y = wg->y;
      b->stage.w = wg->w;
      b->stage.h = wg->h;
      bench_demo_draw(b, x0, y0, x1, y1);
    }
  else
    {
      meter_draw(b, wg);
    }
}

/****************************************************************************
 * Private Functions: state
 ****************************************************************************/

static void set_status(struct bench_s *b, const char *s)
{
  strlcpy(b->status, s, sizeof(b->status));
  kit_set_text(&b->kit, W_STATUS, b->status);
}

/* The window wants frames while benchmarks run, and while a demo shows in
 * the active window
 */

static bool demo_live(struct bench_s *b)
{
  return b->page == PAGE_DEMOS && b->w->active && !b->w->minimized &&
         !desk_frozen();
}

static void bench_frames(struct bench_s *b)
{
  b->w->frames = b->running || demo_live(b);
}

static void demo_status(struct bench_s *b)
{
  if (!demo_live(b))
    {
      set_status(b, bench_demo_about(b->demo));
    }
}

static void table_status(struct bench_s *b)
{
  if (b->list.sel >= 0)
    {
      bench_detail(b, b->list.sel, b->status, sizeof(b->status));
      kit_set_text(&b->kit, W_STATUS, b->status);
    }
  else
    {
      set_status(b, "Select a test and Run it, or Run All");
    }
}

static void bench_queue(struct bench_s *b, int t)
{
  struct bench_result_s *r = &b->res[t];

  if (r->state == BR_QUEUED || b->nqueue >= BT_COUNT)
    {
      return;
    }

  memset(r, 0, sizeof(*r));
  r->state = BR_QUEUED;
  b->queue[b->nqueue++] = t;
  b->running = true;
  kit_list_row(&b->kit, W_TABLE, t);
  bench_frames(b);
}

/* A frame while running: show which runs next, then (the frame after)
 * run it
 */

static void run_step(struct bench_s *b)
{
  char buf[96];
  int t;

  if (b->qpos < b->nqueue)
    {
      t = b->queue[b->qpos];
      if (!b->runshown)
        {
          snprintf(buf, sizeof(buf), "Running %s" DESK_CH_ELLIPSIS " (%d of %d)",
                   bench_test_name(t), b->qpos + 1, b->nqueue);
          set_status(b, buf);
          kit_list_row(&b->kit, W_TABLE, t);
          kit_flush(&b->kit);
          b->runshown = true;
          return;
        }

      b->runshown = false;
      bench_run(b, t);
      b->qpos++;
      kit_list_row(&b->kit, W_TABLE, t);
      if (b->qpos < b->nqueue)
        {
          kit_flush(&b->kit);
          return;
        }
    }

  /* All done */

  t = b->nqueue;
  b->running = false;
  b->nqueue  = 0;
  b->qpos    = 0;
  bench_run_end(b);
  if (b->page == PAGE_BENCH && b->list.sel < 0)
    {
      snprintf(buf, sizeof(buf), "Done: %d test%s, timed by %s", t,
               t == 1 ? "" : "s", b->caps.clock ? "the GPU's clock" :
               "the wall clock (the GPU has no clock)");
      set_status(b, buf);
    }
  else if (b->page == PAGE_BENCH)
    {
      table_status(b);
    }
  else if (b->page == PAGE_DEMOS)
    {
      demo_status(b);
    }

  kit_flush(&b->kit);
}

/* The last second of a demo: frames, operations, what the GPU did */

static void demo_stats(struct bench_s *b)
{
  uint64_t tot[16];
  uint32_t st[41];
  uint32_t now = desk_ms();
  uint32_t dt = now - b->statms;
  bool have = b->statms != 0 && dt > 0;
  char buf[160];

  b->statms = now;
  bench_totals(tot);
  if (have)
    {
      b->fps   = b->frames * 1000 / dt;
      b->opsps = b->frames > 0 ? b->ops / b->frames : 0;
      b->gpuload     = bench_est_us(b->tot, tot) / dt;
      b->gpumeasured = false;
      if (b->caps.clock && bench_hwstats(st))
        {
          uint32_t cyc = st[2] - b->st[2];

          if (cyc > 0)
            {
              b->gpuload = (uint32_t)((uint64_t)(st[29] - b->st[29]) *
                                      1000 / cyc);
              b->gpumeasured = true;
            }

          memcpy(b->st, st, sizeof(st));
        }
    }
  else if (b->caps.clock)
    {
      bench_hwstats(b->st);
    }

  memcpy(b->tot, tot, sizeof(tot));
  b->frames = 0;
  b->ops    = 0;
  if (have && demo_live(b) && !b->running)
    {
      snprintf(buf, sizeof(buf),
               "%s: %lu frames/s, %lu operations a frame, the GPU %lu.%lu "
               "%% busy%s", bench_demo_name(b->demo),
               (unsigned long)b->fps, (unsigned long)b->opsps,
               (unsigned long)(b->gpuload / 10),
               (unsigned long)(b->gpuload % 10),
               b->gpumeasured ? "" : " (estimated, compositing included)");
      set_status(b, buf);
    }
}

static void show_page(struct bench_s *b, int page)
{
  struct kit_s *k = &b->kit;

  b->page = page;
  kit_show(k, W_BENCHPAGE, page == PAGE_BENCH);
  kit_show(k, W_METERPAGE, page == PAGE_METERS);
  kit_show(k, W_STAGE, page == PAGE_DEMOS);
  kit_show(k, W_RUN, page == PAGE_BENCH);
  kit_show(k, W_RUNALL, page == PAGE_BENCH);
  kit_show(k, W_DEMOS, page == PAGE_DEMOS);
  switch (page)
    {
      case PAGE_BENCH:
        table_status(b);
        break;

      case PAGE_METERS:
        bench_sample(b);
        set_status(b, b->caps.clock ?
                   "From /proc/gpu, once a second: the GPU's own counters" :
                   "From /proc/gpu, once a second (the GPU's busy time and "
                   "SDRAM need its clock)");
        break;

      default:
        bench_demo_start(b);
        b->full = false;                    /* The kit draws it, shown */
        demo_status(b);
        break;
    }

  bench_frames(b);
}

/****************************************************************************
 * Private Functions: the kit
 ****************************************************************************/

static const char *table_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  struct bench_s *b = k->priv;

  if (col == 0)
    {
      return bench_test_name(row);
    }

  if (col == 1 && b->running && b->qpos < b->nqueue &&
      b->queue[b->qpos] == row)
    {
      return "Running" DESK_CH_ELLIPSIS;
    }

  bench_format(&b->res[row], col, buf, size);
  return buf;
}

static void bench_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct bench_s *b = k->priv;
  int id = wg != NULL ? wg->id : 0;
  int i;

  switch (ev)
    {
      case KIT_EV_CLICK:
        if (id == W_RUN)
          {
            bench_queue(b, b->list.sel >= 0 ? b->list.sel : 0);
          }
        else if (id == W_RUNALL)
          {
            for (i = 0; i < BT_COUNT; i++)
              {
                bench_queue(b, i);
              }
          }
        break;

      case KIT_EV_ACTIVATE:
        if (id == W_TABLE && b->list.sel >= 0)
          {
            bench_queue(b, b->list.sel);
          }
        break;

      case KIT_EV_CHANGE:
        if (id == W_PAGES)
          {
            show_page(b, wg->value);
          }
        else if (id == W_DEMOS)
          {
            b->demo = wg->value;
            bench_demo_start(b);
            kit_invalidate(k, W_STAGE);
            b->full = false;                /* The kit draws it now */
            demo_status(b);
          }
        else if (id == W_TABLE && !b->running)
          {
            table_status(b);
          }
        break;

      default:
        break;
    }
}

static void bench_build(struct bench_s *b, struct desk_win_s *w)
{
  struct kit_s *k = &b->kit;
  struct kit_widget_s *wg;
  int i;

  kit_init(k, w, b->wg, BENCH_NWG, bench_event);
  k->priv = b;
  k->draw = bench_draw_cb;

  kit_col(k, KIT_BOX_WINDOW);

  kit_row(k, KIT_BOX_BAR);
    kit_tabs(k, W_PAGES, g_pages, 3, 0, KIT_TABS_SEGMENTS);
    kit_spacer(k, 1);
    kit_tabs(k, W_DEMOS, g_demos, DEMO_COUNT, 0, KIT_TABS_SEGMENTS)->flags
      |= KIT_F_HIDDEN;
    kit_tip(k, "The demonstration");
    kit_button(k, W_RUN, "Run", KIT_ICON_NONE, KIT_BTN_NORMAL);
    kit_tip(k, "Run the test selected");
    kit_button(k, W_RUNALL, "Run All", KIT_ICON_NONE, KIT_BTN_DEFAULT);
  kit_end(k);

  /* Benchmarks: what the GPU is, then the table */

  wg = kit_col(k, KIT_BOX_NONE);
  wg->id   = W_BENCHPAGE;
  wg->padx = KIT_PAD;
  wg->pady = KIT_PAD;
  wg->gap  = KIT_PAD;
  kit_grow(k, 1);
    wg = kit_row(k, KIT_BOX_CARD);
    wg->padx = 16;
    wg->pady = 10;
      for (i = 0; i < 4; i++)
        {
          kit_label(k, W_INFO + i, b->info[i], KIT_TEXT_BOLD);
          kit_grow(k, 1);
        }
    kit_end(k);
    kit_list(k, W_TABLE, &b->list, KIT_LIST_TABLE);
    kit_grow(k, 1);
  kit_end(k);

  /* Meters: four cards */

  wg = kit_col(k, KIT_BOX_NONE);
  wg->id    = W_METERPAGE;
  wg->padx  = KIT_PAD;
  wg->pady  = KIT_PAD;
  wg->gap   = KIT_PAD;
  wg->flags |= KIT_F_HIDDEN;
  kit_grow(k, 1);
    kit_row(k, KIT_BOX_NONE)->gap = KIT_PAD;
    kit_grow(k, 1);
      kit_custom(k, W_M_BUSY)->flags |= KIT_F_NOFOCUS;
      kit_grow(k, 1);
      kit_custom(k, W_M_SDRAM)->flags |= KIT_F_NOFOCUS;
      kit_grow(k, 1);
    kit_end(k);
    kit_row(k, KIT_BOX_NONE)->gap = KIT_PAD;
    kit_grow(k, 1);
      kit_custom(k, W_M_LINK)->flags |= KIT_F_NOFOCUS;
      kit_grow(k, 1);
      kit_custom(k, W_M_FPS)->flags |= KIT_F_NOFOCUS;
      kit_grow(k, 1);
    kit_end(k);
  kit_end(k);

  /* Demos: the stage */

  kit_custom(k, W_STAGE)->flags |= KIT_F_NOFOCUS | KIT_F_HIDDEN;
  kit_grow(k, 1);

  kit_row(k, KIT_BOX_STATUS);
    kit_label(k, W_STATUS, b->status, KIT_TEXT_DIM);
    kit_grow(k, 1);
  kit_end(k);
  kit_end(k);
}

/* What the GPU is and has, for the page's card */

static void bench_info(struct bench_s *b)
{
  struct bench_caps_s *c = &b->caps;

  snprintf(b->info[0], sizeof(b->info[0]), "v%d\nGPU protocol", c->version);
  if (!c->stats)
    {
      snprintf(b->info[1], sizeof(b->info[1]), "None\nCounters (STATS)");
    }
  else if (!c->clock)
    {
      snprintf(b->info[1], sizeof(b->info[1]),
               "No clock\nCounters (STATS)");
    }
  else
    {
      snprintf(b->info[1], sizeof(b->info[1]), "%lu.%lu MHz\nCounters "
               "(STATS)", (unsigned long)(c->khz / 1000),
               (unsigned long)(c->khz % 1000 / 100));
    }

  snprintf(b->info[2], sizeof(b->info[2]), "%s\nBC1 images",
           c->bc1 ? "Yes" : "No");
  snprintf(b->info[3], sizeof(b->info[3]), "%d MB/s\nThe link's wire",
           c->wire);
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void bench_resize(struct desk_win_s *w)
{
  struct bench_s *b = w->priv;

  if (b != NULL)
    {
      kit_resize(&b->kit);
    }
}

static void bench_expose(struct desk_win_s *w, int x, int y, int cw,
                         int ch)
{
  struct bench_s *b = w->priv;

  if (b != NULL)
    {
      kit_expose(&b->kit, x, y, cw, ch);
    }
}

static void bench_draw_all(struct desk_win_s *w)
{
  struct bench_s *b = w->priv;

  if (b != NULL)
    {
      kit_redraw(&b->kit);
    }
}

static void bench_mouse(struct desk_win_s *w, int x, int y,
                        uint8_t buttons)
{
  struct bench_s *b = w->priv;

  if (b != NULL)
    {
      kit_mouse(&b->kit, x, y, buttons);
    }
}

static void bench_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct bench_s *b = w->priv;

  if (b != NULL)
    {
      kit_key(&b->kit, ch, n);
    }
}

static void bench_tick(struct desk_win_s *w)
{
  struct bench_s *b = w->priv;
  int i;

  if (b == NULL)
    {
      return;
    }

  kit_tick(&b->kit);
  if (b->page == PAGE_METERS && !b->running)
    {
      bench_sample(b);
      for (i = W_M_BUSY; i <= W_M_FPS; i++)
        {
          kit_invalidate(&b->kit, i);
        }
    }
  else if (b->page == PAGE_DEMOS)
    {
      demo_stats(b);
    }

  kit_flush(&b->kit);
}

static void bench_focus(struct desk_win_s *w)
{
  struct bench_s *b = w->priv;

  if (b == NULL)
    {
      return;
    }

  kit_activate(&b->kit);
  if (b->page == PAGE_DEMOS)
    {
      b->last = desk_ms();                 /* The clock stood still */
      demo_status(b);
      kit_flush(&b->kit);
    }

  bench_frames(b);
}

static void bench_frame(struct desk_win_s *w)
{
  struct bench_s *b = w->priv;

  if (b == NULL)
    {
      return;
    }

  if (b->running)
    {
      run_step(b);
    }
  else if (demo_live(b))
    {
      struct kit_widget_s *wg = kit_get(&b->kit, W_STAGE);

      if (wg->x != b->stage.x || wg->y != b->stage.y ||
          wg->w != b->stage.w || wg->h != b->stage.h)
        {
          b->stage.x = wg->x;
          b->stage.y = wg->y;
          b->stage.w = wg->w;
          b->stage.h = wg->h;
          b->full    = true;
        }

      bench_demo_frame(b);
    }

  bench_frames(b);
}

static void bench_close(struct desk_win_s *w)
{
  struct bench_s *b = w->priv;

  w->frames = false;
  if (b != NULL)
    {
      bench_run_end(b);
      free(b->o);
      b->o = NULL;
    }
}

/* The icon: a chip with its pins, and bars rising on it */

static void bench_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  static const uint16_t bars[3] =
  {
    RGB(0x30, 0xd1, 0x58), RGB(0xff, 0xd6, 0x0a), RGB(0xff, 0x45, 0x3a)
  };

  int cs = s * 13 / 24;
  int cx = x + (s - cs) / 2;
  int cy = y + (s - cs) / 2;
  int pin = s / 12;
  int pw = s / 24 > 2 ? s / 24 : 2;
  int bw = cs / 5;
  int i;

  icon_tile(o, x, y, s, RGB(0x3c, 0x44, 0x78), RGB(0x12, 0x14, 0x2e), a);
  for (i = 0; i < 4; i++)
    {
      int p = cs * (2 * i + 1) / 8 - pw / 2;

      ops_blend(o, cx + p, cy - pin, pw, pin, C_WHITE,
                (uint8_t)(a * 170 / 255));
      ops_blend(o, cx + p, cy + cs, pw, pin, C_WHITE,
                (uint8_t)(a * 170 / 255));
      ops_blend(o, cx - pin, cy + p, pin, pw, C_WHITE,
                (uint8_t)(a * 170 / 255));
      ops_blend(o, cx + cs, cy + p, pin, pw, C_WHITE,
                (uint8_t)(a * 170 / 255));
    }

  ops_rrect(o, cx, cy, cs, cs, LUT_R6, 6, RGB(0x1a, 0x1d, 0x30), a);
  ops_rrect(o, cx + 1, cy + 1, cs - 2, cs - 2, LUT_R6, 5,
            RGB(0x2c, 0x31, 0x52), a);
  for (i = 0; i < 3; i++)
    {
      int h = (cs - 2 * bw) * (i + 2) / 4;

      ops_rrect(o, cx + bw / 2 + i * (bw + bw / 2) + bw / 4,
                cy + cs - bw * 3 / 4 - h, bw, h, LUT_R3,
                bw / 2 < 3 ? bw / 2 : 3, bars[i], a);
    }
}

static struct desk_win_s *bench_open(void)
{
  struct desk_win_s *w;
  struct bench_s *b;
  int i;

  b = calloc(1, sizeof(*b));
  if (b == NULL)
    {
      return NULL;
    }

  b->o = malloc(sizeof(*b->o));
  if (b->o == NULL)
    {
      free(b);
      return NULL;
    }

  w = desk_win_open(APP_BENCH, "GPU Bench", BENCH_W, BENCH_H);
  if (w == NULL)
    {
      free(b->o);
      free(b);
      return NULL;
    }

  w->priv = b;
  b->w    = w;
  for (i = 0; i < DEMO_COUNT; i++)
    {
      g_demos[i] = bench_demo_name(i);
    }

  bench_caps(&b->caps);
  bench_info(b);
  b->list.n       = BT_COUNT;
  b->list.rowh    = 26;
  b->list.sel     = -1;
  b->list.ncols   = 4;
  b->list.cols    = g_cols;
  b->list.text    = table_text;
  b->list.sortcol = -1;

  bench_build(b, w);
  table_status(b);
  kit_layout(&b->kit);
  kit_flush(&b->kit);
  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_bench =
{
  .name   = "GPU Bench",
  .icon   = bench_icon,
  .open   = bench_open,
  .draw   = bench_draw_all,
  .expose = bench_expose,
  .resize = bench_resize,
  .tick   = bench_tick,
  .mouse  = bench_mouse,
  .key    = bench_key,
  .focus  = bench_focus,
  .frame  = bench_frame,
  .close  = bench_close,
  .single = true,
};

#endif /* CONFIG_FPCDESK_BENCH */
