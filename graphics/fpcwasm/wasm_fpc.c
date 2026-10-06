/****************************************************************************
 * apps/graphics/fpcwasm/wasm_fpc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The host API of WebAssembly apps (sdk/fpc_wasm.h, docs/wasm.md): the
 * functions of the "env" module named fpc_*, on the game SDK
 * (apps/graphics/fpcgame).
 *
 * The sandbox's side of it: every parameter is an i32; a pointer is an
 * offset into the app's linear memory, and every buffer is checked against
 * it whole (its size from the other parameters, in 64 bits) before it is
 * touched: one outside it is a trap (the runtime's "out of bounds memory
 * access"), and the app ends.  What the SDK keeps beyond the call (a
 * sample, a song) is copied out of the linear memory, which moves when it
 * grows.  Handles and values out of range are the SDK's to ignore (it
 * checks them all), or are clamped here first.  The GPU, input and sound
 * are touched only while fpc_game_run() runs: the SDK's state (the
 * canvases, the images' rectangles) belongs to a running game, and the
 * GPU's memory to the desktop the rest of the time.
 ****************************************************************************/

#include "wasm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxkeys.h>

#include "fpcgame.h"

#define MAX_IMAGE_FILE   (4 * 1024 * 1024)
#define MAX_SAMPLE       (1024 * 1024)    /* Samples a sample */
#define MAX_NOTES        8192

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR int8_t *g_sample[FG_SAMPLES];  /* Copies of the app's samples */
static FAR struct fg_note_s *g_notes;     /* ... of its song */
static struct fg_song_s g_song;

/****************************************************************************
 * Private Functions: the linear memory
 ****************************************************************************/

/* The app's buffer of size bytes at offset off, or NULL (a trap: the
 * exception is set, and the caller returns at once)
 */

static FAR void *app_buf(wasm_exec_env_t env, uint32_t off, uint64_t size)
{
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

  if (size == 0)
    {
      size = 1;                           /* (still a valid offset) */
    }

  if (size > UINT32_MAX ||
      !wasm_runtime_validate_app_addr(inst, off, size))
    {
      return NULL;
    }

  return wasm_runtime_addr_app_to_native(inst, off);
}

/* A rectangle of elements (w x h, rows stride elements apart, each esize
 * bytes) in the linear memory: its size, or 0 if the numbers are wrong
 */

static uint64_t rect_size(int32_t w, int32_t h, int32_t stride, int esize)
{
  if (w <= 0 || h <= 0 || stride < w)
    {
      return 0;
    }

  return ((uint64_t)(h - 1) * (uint64_t)stride + (uint64_t)w) *
         (uint64_t)esize;
}

/****************************************************************************
 * The system
 ****************************************************************************/

static int32_t n_api_version(wasm_exec_env_t env)
{
  return WASM_API_VERSION;
}

static void n_log(wasm_exec_env_t env, uint32_t text, int32_t len)
{
  FAR const char *s;

  if (len <= 0)
    {
      return;
    }

  s = app_buf(env, text, (uint32_t)len);
  if (s != NULL)
    {
      syslog(LOG_INFO, "wasm %s: %.*s\n", g_wapp.name, len > 200 ? 200 : len,
             s);
    }
}

static uint32_t n_time_us(wasm_exec_env_t env)
{
  struct timespec ts;

  if (g_wapp.in_game)
    {
      return fg_time_us();
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

static uint32_t n_time_ms(wasm_exec_env_t env)
{
  struct timespec ts;

  if (g_wapp.in_game)
    {
      return fg_time_ms();
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
}

static void n_sleep_ms(wasm_exec_env_t env, int32_t ms)
{
  if (ms > 0)
    {
      usleep((useconds_t)(ms > 10000 ? 10000 : ms) * 1000);
    }
}

static uint32_t n_random(wasm_exec_env_t env)
{
  return fg_random();
}

static int32_t n_random_range(wasm_exec_env_t env, int32_t lo, int32_t hi)
{
  if (hi < lo)
    {
      return lo;
    }

  return lo + (int32_t)(fg_random() % ((uint32_t)hi - (uint32_t)lo + 1u));
}

static void n_random_seed(wasm_exec_env_t env, uint32_t seed)
{
  fg_random_seed(seed);
}

static int32_t n_sin(wasm_exec_env_t env, int32_t angle)
{
  return fg_sin(angle & 4095);
}

static int32_t n_cos(wasm_exec_env_t env, int32_t angle)
{
  return fg_cos(angle & 4095);
}

/****************************************************************************
 * The game
 ****************************************************************************/

static struct
{
  wasm_function_inst_t load;
  wasm_function_inst_t init;
  wasm_function_inst_t update;
  wasm_function_inst_t draw;
  wasm_function_inst_t quit;
} g_cb;

/* An exported function of no parameters and no result, or NULL */

static wasm_function_inst_t export_void(const char *name, bool *bad)
{
  wasm_function_inst_t f = wasm_runtime_lookup_function(g_wapp.inst, name);

  if (f != NULL && (wasm_func_get_param_count(f, g_wapp.inst) != 0 ||
                    wasm_func_get_result_count(f, g_wapp.inst) != 0))
    {
      fprintf(stderr, "wasm: %s: %s must take and return nothing\n",
              g_wapp.name, name);
      *bad = true;
      return NULL;
    }

  return f;
}

/* Call one of them; a trap (or the watchdog's end, or WASI's proc_exit)
 * ends the game: the SDK gives the display back when fg_run() returns,
 * and the exception, still set, ends the app when fpc_game_run() returns
 * to it
 */

static void call_export(wasm_function_inst_t f)
{
  if (f == NULL || g_wapp.trapped)
    {
      return;
    }

  g_wapp.in_callback = true;
  if (!wasm_runtime_call_wasm(g_wapp.env, f, 0, NULL))
    {
      g_wapp.trapped = true;
      fg_exit();
    }

  g_wapp.in_callback = false;
  g_wapp.beat++;
}

static void cb_load(void)
{
  call_export(g_cb.load);
}

static void cb_init(void)
{
  call_export(g_cb.init);
}

static void cb_update(void)
{
  call_export(g_cb.update);
}

static void cb_draw(void)
{
  call_export(g_cb.draw);
}

static void cb_quit(void)
{
  call_export(g_cb.quit);
}

int wasm_fpc_run_game(int w, int h)
{
  struct fg_game_s game;
  bool bad = false;
  int ret;

  if (g_wapp.in_game)
    {
      return -EBUSY;
    }

  if (w < 0 || h < 0 || w > 1280 || h > 800 || (w == 0) != (h == 0) ||
      (w > 0 && (w < 16 || h < 16)))
    {
      return -EINVAL;
    }

  g_cb.load   = export_void("fpc_load", &bad);
  g_cb.init   = export_void("fpc_init", &bad);
  g_cb.update = export_void("fpc_update", &bad);
  g_cb.draw   = export_void("fpc_draw", &bad);
  g_cb.quit   = export_void("fpc_quit", &bad);
  if (bad)
    {
      return -EINVAL;
    }

  if (g_cb.draw == NULL)
    {
      fprintf(stderr, "wasm: %s: no fpc_draw export\n", g_wapp.name);
      return -ENOENT;
    }

  memset(&game, 0, sizeof(game));
  game.name   = g_wapp.name;
  game.width  = w;
  game.height = h;
  game.load   = cb_load;
  game.init   = cb_init;
  game.update = cb_update;
  game.draw   = cb_draw;
  game.quit   = cb_quit;

  if (g_wapp.script != NULL)
    {
      fg_script(g_wapp.script);
    }

  g_wapp.trapped = false;
  g_wapp.in_game = true;
  ret = fg_run(&game);
  g_wapp.in_game = false;
  wasm_fpc_game_end();
  return ret;
}

void wasm_fpc_game_end(void)
{
  int i;

  /* (fg_run() has stopped the sound's thread: nothing reads these now) */

  for (i = 0; i < FG_SAMPLES; i++)
    {
      free(g_sample[i]);
      g_sample[i] = NULL;
    }

  free(g_notes);
  g_notes = NULL;
  memset(&g_song, 0, sizeof(g_song));
}

static int32_t n_game_run(wasm_exec_env_t env, int32_t w, int32_t h)
{
  return wasm_fpc_run_game(w, h);
}

static void n_game_exit(wasm_exec_env_t env)
{
  if (g_wapp.in_game)
    {
      fg_exit();
    }
}

static int32_t n_game_width(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_width() : 0;
}

static int32_t n_game_height(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_height() : 0;
}

static uint32_t n_game_frame(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_frame() : 0;
}

static int32_t n_game_display(wasm_exec_env_t env, uint32_t out, int32_t n)
{
  FAR int32_t *o;
  int v[4];
  int i;

  if (!g_wapp.in_game || n <= 0)
    {
      return 0;
    }

  n = n > 4 ? 4 : n;
  o = app_buf(env, out, (uint64_t)n * 4);
  if (o == NULL)
    {
      return 0;
    }

  fg_display_info(&v[0], &v[1], &v[2], &v[3]);
  for (i = 0; i < n; i++)
    {
      o[i] = v[i];
    }

  return 4;
}

static int32_t n_game_stats(wasm_exec_env_t env, uint32_t out, int32_t n)
{
  FAR const struct fg_stats_s *st = fg_stats();
  FAR uint32_t *o;
  uint32_t v[7];
  int i;

  if (n <= 0)
    {
      return 7;
    }

  n = n > 7 ? 7 : n;
  o = app_buf(env, out, (uint64_t)n * 4);
  if (o == NULL)
    {
      return 0;
    }

  v[0] = st->frames;
  v[1] = st->frame_us;
  v[2] = st->frame_us_max;
  v[3] = st->wait_us;
  v[4] = st->late;
  v[5] = st->sprites;
  v[6] = st->tiles;
  for (i = 0; i < n; i++)
    {
      o[i] = v[i];
    }

  return 7;
}

/****************************************************************************
 * Images
 ****************************************************************************/

static int32_t n_image_new(wasm_exec_env_t env, int32_t w, int32_t h)
{
  return g_wapp.in_game ? fg_image_new(w, h) : -EPERM;
}

static void n_image_free(wasm_exec_env_t env, int32_t img)
{
  if (g_wapp.in_game)
    {
      fg_image_free(img);
    }
}

static int32_t n_image_width(wasm_exec_env_t env, int32_t img)
{
  return g_wapp.in_game ? fg_image_width(img) : 0;
}

static int32_t n_image_height(wasm_exec_env_t env, int32_t img)
{
  return g_wapp.in_game ? fg_image_height(img) : 0;
}

static void n_image_put(wasm_exec_env_t env, int32_t img, int32_t x,
                        int32_t y, int32_t w, int32_t h, uint32_t px,
                        int32_t stride)
{
  uint64_t size = rect_size(w, h, stride, 2);
  FAR const uint16_t *p;

  if (size == 0 || (px & 1) != 0)
    {
      return;
    }

  p = app_buf(env, px, size);
  if (p != NULL && g_wapp.in_game)
    {
      fg_image_put(img, x, y, w, h, p, (size_t)stride * 2);
    }
}

static void n_image_put8(wasm_exec_env_t env, int32_t img, int32_t x,
                         int32_t y, int32_t w, int32_t h, uint32_t px,
                         int32_t stride, uint32_t pal, int32_t npal)
{
  uint64_t size = rect_size(w, h, stride, 1);
  uint16_t pal565[256];
  FAR const uint16_t *pp = NULL;
  FAR const uint8_t *p;
  int i;

  if (size == 0 || npal < 0 || (npal > 0 && (pal & 1) != 0))
    {
      return;
    }

  npal = npal > 256 ? 256 : npal;
  p = app_buf(env, px, size);
  if (p == NULL || (npal > 0 && (pp = app_buf(env, pal, npal * 2)) == NULL))
    {
      return;
    }

  memset(pal565, 0, sizeof(pal565));
  for (i = 0; i < npal; i++)
    {
      pal565[i] = pp[i];
    }

  if (g_wapp.in_game)
    {
      fg_image_put8(img, x, y, w, h, p, (size_t)stride, pal565);
    }
}

static int32_t n_image_decode(wasm_exec_env_t env, uint32_t data,
                              int32_t len)
{
  FAR const uint8_t *p;

  if (len <= 0)
    {
      return -EINVAL;
    }

  p = app_buf(env, data, (uint32_t)len);
  if (p == NULL)
    {
      return -EFAULT;
    }

  return g_wapp.in_game ? wasm_image_decode(p, (size_t)len) : -EPERM;
}

/* A path of the app's own directory: relative, no "..", no empty parts */

static int app_path(FAR const char *s, int len, FAR char *out, size_t size)
{
  FAR const char *p;
  int n;

  if (g_wapp.dir[0] == '\0' || len <= 0 || len > 255 || s[0] == '/' ||
      memchr(s, '\0', len) != NULL)
    {
      return -EACCES;
    }

  for (p = s; p < s + len; p += n + 1)
    {
      FAR const char *e = memchr(p, '/', s + len - p);

      n = (int)((e != NULL ? e : s + len) - p);
      if (n == 0 || (n == 2 && p[0] == '.' && p[1] == '.'))
        {
          return -EACCES;
        }
    }

  if (snprintf(out, size, "%s/%.*s", g_wapp.dir, len, s) >= (int)size)
    {
      return -ENAMETOOLONG;
    }

  return OK;
}

static int32_t n_image_load(wasm_exec_env_t env, uint32_t path, int32_t len)
{
  char full[PATH_MAX];
  FAR const char *s;
  FAR uint8_t *buf;
  struct stat st;
  ssize_t got;
  int ret;
  int fd;

  if (len <= 0)
    {
      return -EINVAL;
    }

  s = app_buf(env, path, (uint32_t)len);
  if (s == NULL)
    {
      return -EFAULT;
    }

  if (!g_wapp.in_game)
    {
      return -EPERM;
    }

  ret = app_path(s, len, full, sizeof(full));
  if (ret < 0)
    {
      return ret;
    }

  fd = open(full, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > MAX_IMAGE_FILE)
    {
      close(fd);
      return -EFBIG;
    }

  buf = malloc(st.st_size);
  if (buf == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  got = read(fd, buf, st.st_size);
  close(fd);
  ret = got == st.st_size ? wasm_image_decode(buf, (size_t)got) : -EIO;
  free(buf);
  return ret;
}

/****************************************************************************
 * Drawing
 ****************************************************************************/

static void n_clear(wasm_exec_env_t env, int32_t colour)
{
  if (g_wapp.in_game)
    {
      fg_clear((uint16_t)colour);
    }
}

static void n_rect(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w,
                   int32_t h, int32_t colour)
{
  if (g_wapp.in_game && w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
      x > -4096 && x < 4096 && y > -4096 && y < 4096)
    {
      fg_rect(x, y, w, h, (uint16_t)colour);
    }
}

/* Positions far off the canvas, and scales out of all proportion, are not
 * drawn (the SDK's arithmetic is 32-bit)
 */

static bool sane_pos(int32_t x, int32_t y)
{
  return x > -32768 && x < 32768 && y > -32768 && y < 32768;
}

static void n_sprite(wasm_exec_env_t env, int32_t img, int32_t sx,
                     int32_t sy, int32_t w, int32_t h, int32_t x, int32_t y,
                     int32_t flags)
{
  if (g_wapp.in_game && sane_pos(x, y))
    {
      fg_sprite(img, sx, sy, w, h, x, y, (uint32_t)flags);
    }
}

static void n_sprite_rot(wasm_exec_env_t env, int32_t img, int32_t sx,
                         int32_t sy, int32_t w, int32_t h, int32_t cx,
                         int32_t cy, int32_t flags, int32_t angle,
                         int32_t scale)
{
  if (g_wapp.in_game && sane_pos(cx, cy) && scale > 0 && scale <= 16 * 256)
    {
      fg_sprite_rot(img, sx, sy, w, h, cx, cy, (uint32_t)flags,
                    angle & 4095, scale);
    }
}

static void n_sprite_matrix(wasm_exec_env_t env, int32_t img, int32_t sx,
                            int32_t sy, int32_t w, int32_t h, int32_t cx,
                            int32_t cy, int32_t flags, int32_t a, int32_t b,
                            int32_t c, int32_t d)
{
  const int32_t lim = 64 * 256;

  if (g_wapp.in_game && sane_pos(cx, cy) && a > -lim && a < lim &&
      b > -lim && b < lim && c > -lim && c < lim && d > -lim && d < lim)
    {
      fg_sprite_matrix(img, sx, sy, w, h, cx, cy, (uint32_t)flags, a, b, c,
                       d);
    }
}

static void n_quad(wasm_exec_env_t env, int32_t img, int32_t sx, int32_t sy,
                   int32_t w, int32_t h, uint32_t xy, int32_t flags)
{
  FAR const int32_t *p;
  int32_t x[4];
  int32_t y[4];
  int i;

  if ((xy & 3) != 0)
    {
      return;
    }

  p = app_buf(env, xy, 8 * 4);
  if (p == NULL || !g_wapp.in_game)
    {
      return;
    }

  for (i = 0; i < 4; i++)
    {
      x[i] = p[2 * i];
      y[i] = p[2 * i + 1];
      if (!sane_pos(x[i] / 16, y[i] / 16))
        {
          return;
        }
    }

  fg_quad(img, sx, sy, w, h, x, y, (uint32_t)flags);
}

static int32_t n_quad3d(wasm_exec_env_t env, int32_t img, int32_t sx,
                        int32_t sy, int32_t w, int32_t h, int32_t cx,
                        int32_t cy, int32_t flags, int32_t yaw,
                        int32_t pitch, int32_t roll, int32_t scale)
{
  if (!g_wapp.in_game || !sane_pos(cx, cy) || scale <= 0 ||
      scale > 16 * 256)
    {
      return 0;
    }

  return fg_quad3d(img, sx, sy, w, h, cx, cy, (uint32_t)flags, yaw & 4095,
                   pitch & 4095, roll & 4095, scale) ? 1 : 0;
}

static void n_mode7(wasm_exec_env_t env, int32_t img, int32_t top,
                    int32_t bottom, int32_t camx, int32_t camy,
                    int32_t angle, int32_t height, int32_t horizon,
                    int32_t focal, int32_t flags)
{
  if (g_wapp.in_game && top >= -4096 && bottom <= 4096 && height > 0 &&
      height < 65536 && focal > 0 && focal < 65536 && horizon > -4096 &&
      horizon < 4096)
    {
      fg_mode7(img, top, bottom, camx, camy, angle & 4095, height, horizon,
               focal, (uint32_t)flags);
    }
}

/****************************************************************************
 * Tile layers, text
 ****************************************************************************/

static int32_t n_layer_init(wasm_exec_env_t env, int32_t layer,
                            int32_t tileset, int32_t tw, int32_t th,
                            int32_t mapw, int32_t maph)
{
  if (!g_wapp.in_game)
    {
      return -EPERM;
    }

  if (mapw <= 0 || maph <= 0 || mapw > 2048 || maph > 2048)
    {
      return -EINVAL;
    }

  return fg_layer_init(layer, tileset, tw, th, mapw, maph);
}

static void n_layer_free(wasm_exec_env_t env, int32_t layer)
{
  if (g_wapp.in_game)
    {
      fg_layer_free(layer);
    }
}

static void n_layer_tileset(wasm_exec_env_t env, int32_t layer,
                            int32_t tileset)
{
  if (g_wapp.in_game)
    {
      fg_layer_tileset(layer, tileset);
    }
}

static void n_layer_set(wasm_exec_env_t env, int32_t layer, int32_t x,
                        int32_t y, int32_t entry)
{
  if (g_wapp.in_game)
    {
      fg_layer_set(layer, x, y, (uint16_t)entry);
    }
}

static int32_t n_layer_get(wasm_exec_env_t env, int32_t layer, int32_t x,
                           int32_t y)
{
  return g_wapp.in_game ? fg_layer_get(layer, x, y) : 0;
}

static void n_layer_put(wasm_exec_env_t env, int32_t layer, int32_t x,
                        int32_t y, int32_t w, int32_t h, uint32_t entries,
                        int32_t stride)
{
  uint64_t size = rect_size(w, h, stride, 2);
  FAR const uint16_t *p;
  int i;
  int j;

  if (size == 0 || (entries & 1) != 0 || w > 2048 || h > 2048)
    {
      return;
    }

  p = app_buf(env, entries, size);
  if (p == NULL || !g_wapp.in_game)
    {
      return;
    }

  for (j = 0; j < h; j++)
    {
      for (i = 0; i < w; i++)
        {
          fg_layer_set(layer, x + i, y + j, p[(size_t)j * stride + i]);
        }
    }
}

static void n_layer_redraw(wasm_exec_env_t env, int32_t layer)
{
  if (g_wapp.in_game)
    {
      fg_layer_redraw(layer);
    }
}

static void n_layer_draw(wasm_exec_env_t env, int32_t layer, int32_t sx,
                         int32_t sy)
{
  if (g_wapp.in_game)
    {
      fg_layer_draw(layer, sx, sy);
    }
}

static void n_text_print(wasm_exec_env_t env, int32_t col, int32_t row,
                         uint32_t text, int32_t len)
{
  char buf[FG_TEXT_COLS + 1];
  FAR const char *s;
  int i;

  if (len <= 0)
    {
      return;
    }

  s = app_buf(env, text, (uint32_t)len);
  if (s == NULL || !g_wapp.in_game || col <= -FG_TEXT_COLS || row < 0 ||
      col >= FG_TEXT_COLS || row >= FG_TEXT_ROWS)
    {
      return;
    }

  len = len > FG_TEXT_COLS ? FG_TEXT_COLS : len;
  for (i = 0; i < len && s[i] != '\0'; i++)
    {
      buf[i] = s[i];
    }

  buf[i] = '\0';
  fg_text_print(col, row, "%s", buf);
}

static void n_text_clear(wasm_exec_env_t env)
{
  if (g_wapp.in_game)
    {
      fg_text_clear();
    }
}

static void n_text_color(wasm_exec_env_t env, int32_t colour)
{
  if (g_wapp.in_game)
    {
      fg_text_color((uint16_t)colour);
    }
}

static void n_text_draw(wasm_exec_env_t env)
{
  if (g_wapp.in_game)
    {
      fg_text_draw();
    }
}

/****************************************************************************
 * Input
 ****************************************************************************/

static uint32_t n_pad(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_pad() : 0;
}

static uint32_t n_pad_pressed(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_pad_pressed() : 0;
}

static uint32_t n_pad_released(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_pad_released() : 0;
}

static int32_t n_axis_x(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_axis_x() : 0;
}

static int32_t n_axis_y(wasm_exec_env_t env)
{
  return g_wapp.in_game ? fg_axis_y() : 0;
}

/* The SDK's key codes (NX's: nxkeys.h) as the API's (fpc_wasm.h) */

static int key_code(uint8_t code)
{
  static const int special[] =
  {
    /* FPC_KEY_LEFT .. FPC_KEY_DELETE, in order */

    NX_KEY_SPECIAL(KEYCODE_LEFT), NX_KEY_SPECIAL(KEYCODE_RIGHT),
    NX_KEY_SPECIAL(KEYCODE_UP), NX_KEY_SPECIAL(KEYCODE_DOWN),
    NX_KEY_SPECIAL(KEYCODE_HOME), NX_KEY_SPECIAL(KEYCODE_END),
    NX_KEY_SPECIAL(KEYCODE_PAGEUP), NX_KEY_SPECIAL(KEYCODE_PAGEDOWN),
    NX_KEY_SPECIAL(KEYCODE_INSERT), NX_KEY_SPECIAL(KEYCODE_FWDDEL)
  };

  int i;

  if (code < 0x80)
    {
      return code >= 'A' && code <= 'Z' ? code + 0x20 : code;
    }

  for (i = 0; i < (int)(sizeof(special) / sizeof(special[0])); i++)
    {
      if (code == special[i])
        {
          return 0x80 + i;
        }
    }

  if (code >= NX_KEY_SPECIAL(KEYCODE_F1) &&
      code < NX_KEY_SPECIAL(KEYCODE_F1) + 12)
    {
      return 0x90 + (code - NX_KEY_SPECIAL(KEYCODE_F1));
    }

  return -1;
}

static int32_t n_key_next(wasm_exec_env_t env)
{
  uint8_t flags;
  uint8_t code;
  uint8_t ch;
  int k;

  while (g_wapp.in_game && fg_key_next(&flags, &code, &ch))
    {
      k = key_code(code);
      if (k >= 0)
        {
          return k | ((flags & NX_KEY_RELEASE) ? 0x100 : 0) |
                 (ch >= 0x20 && ch < 0x7f ? (int32_t)ch << 16 : 0);
        }
    }

  return -1;
}

static int32_t n_mouse(wasm_exec_env_t env, uint32_t out, int32_t n)
{
  FAR int32_t *o;
  int v[4];
  int i;

  if (!g_wapp.in_game || n <= 0)
    {
      return 0;
    }

  n = n > 4 ? 4 : n;
  o = app_buf(env, out, (uint64_t)n * 4);
  if (o == NULL || !fg_mouse(&v[0], &v[1], &v[2], &v[3]))
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      o[i] = v[i];
    }

  return 1;
}

/****************************************************************************
 * Sound
 ****************************************************************************/

static int32_t n_sample(wasm_exec_env_t env, int32_t id, uint32_t data,
                        int32_t samples, int32_t loop_start, int32_t rate)
{
  FAR const int8_t *p;
  FAR int8_t *copy;
  FAR int8_t *old;
  int ret;

  if (id < 0 || id >= FG_SAMPLES || samples <= 0 || samples > MAX_SAMPLE ||
      rate <= 0 || rate > 192000)
    {
      return -EINVAL;
    }

  p = app_buf(env, data, (uint32_t)samples);
  if (p == NULL)
    {
      return -EFAULT;
    }

  if (!g_wapp.in_game)
    {
      return -EPERM;
    }

  copy = malloc(samples);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  memcpy(copy, p, samples);
  ret = fg_sample(id, copy, (uint32_t)samples,
                  loop_start < 0 || loop_start >= samples ? ~0u :
                  (uint32_t)loop_start, (uint32_t)rate);
  if (ret < 0)
    {
      free(copy);
      return ret;
    }

  /* (the engine looks a sample up under its lock each time: the old copy
   * is no longer read)
   */

  old = g_sample[id];
  g_sample[id] = copy;
  free(old);
  return OK;
}

static void n_sfx_play(wasm_exec_env_t env, int32_t voice, int32_t sample,
                       uint32_t pitch16, int32_t vol_l, int32_t vol_r)
{
  if (g_wapp.in_game && sample >= 0 && sample < FG_SAMPLES &&
      g_sample[sample] != NULL)
    {
      fg_sfx_play(voice, sample, pitch16, vol_l, vol_r);
    }
}

static void n_sfx_stop(wasm_exec_env_t env, int32_t voice)
{
  if (g_wapp.in_game)
    {
      fg_sfx_stop(voice);
    }
}

static void n_patch(wasm_exec_env_t env, int32_t id, uint32_t patch)
{
  FAR const uint8_t *p = app_buf(env, patch, 18);
  struct fg_patch_s pt;
  int k;

  if (p == NULL || !g_wapp.in_game)
    {
      return;
    }

  pt.alg      = p[0];
  pt.feedback = p[1];
  for (k = 0; k < 2; k++)
    {
      FAR const uint8_t *o = p + 2 + 8 * k;

      pt.op[k].mult    = o[0];
      pt.op[k].detune  = (int8_t)o[1];
      pt.op[k].wave    = o[2];
      pt.op[k].level   = o[3];
      pt.op[k].attack  = o[4];
      pt.op[k].decay   = o[5];
      pt.op[k].sustain = o[6];
      pt.op[k].release = o[7];
    }

  fg_patch(id, &pt);
}

static void n_fm_on(wasm_exec_env_t env, int32_t voice, int32_t patch,
                    uint32_t freq16, int32_t vol_l, int32_t vol_r)
{
  if (g_wapp.in_game)
    {
      fg_fm_on(voice, patch, freq16, vol_l, vol_r);
    }
}

static void n_fm_off(wasm_exec_env_t env, int32_t voice)
{
  if (g_wapp.in_game)
    {
      fg_fm_off(voice);
    }
}

static void n_fm_freq(wasm_exec_env_t env, int32_t voice, uint32_t freq16)
{
  if (g_wapp.in_game)
    {
      fg_fm_freq(voice, freq16);
    }
}

static uint32_t n_note_freq(wasm_exec_env_t env, int32_t note)
{
  return fg_note_freq(note);
}

static void n_master_volume(wasm_exec_env_t env, int32_t l, int32_t r)
{
  if (g_wapp.in_game)
    {
      fg_master_volume(l, r);
    }
}

static void n_voice_volume(wasm_exec_env_t env, int32_t voice, int32_t l,
                           int32_t r)
{
  if (g_wapp.in_game)
    {
      fg_voice_volume(voice, l, r);
    }
}

static void n_audio_stop_all(wasm_exec_env_t env)
{
  if (g_wapp.in_game)
    {
      fg_audio_stop_all();
    }
}

static int32_t n_music_play(wasm_exec_env_t env, uint32_t notes,
                            int32_t count, int32_t length, int32_t loop)
{
  FAR struct fg_note_s *copy;
  FAR const uint8_t *p;
  FAR struct fg_note_s *old;
  int i;

  if (count <= 0 || count > MAX_NOTES || length <= 0 || length > 65535)
    {
      return -EINVAL;
    }

  p = app_buf(env, notes, (uint64_t)count * 8);
  if (p == NULL)
    {
      return -EFAULT;
    }

  if (!g_wapp.in_game)
    {
      return -EPERM;
    }

  copy = malloc(sizeof(*copy) * count);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < count; i++, p += 8)
    {
      copy[i].frame = (uint16_t)(p[0] | p[1] << 8);
      copy[i].voice = p[2];
      copy[i].patch = p[3];
      copy[i].note  = p[4];
      copy[i].vel   = p[5];
      copy[i].len   = (uint16_t)(p[6] | p[7] << 8);
    }

  /* (the sequencer runs in this thread, once a frame: the old song is
   * done with as soon as the new one is set)
   */

  old = g_notes;
  g_notes = copy;
  g_song.notes = copy;
  g_song.count = count;
  g_song.length_frames = (uint16_t)length;
  g_song.loop = loop != 0;
  fg_music_play(&g_song);
  free(old);
  return OK;
}

static void n_music_stop(wasm_exec_env_t env)
{
  if (g_wapp.in_game)
    {
      fg_music_stop();
    }
}

static int32_t n_music_playing(wasm_exec_env_t env)
{
  return g_wapp.in_game && fg_music_playing() ? 1 : 0;
}

/****************************************************************************
 * The table
 ****************************************************************************/

#define N(name, sig) { "fpc_" #name, (void *)n_##name, sig, NULL }

static NativeSymbol g_natives[] =
{
  N(api_version,    "()i"),
  N(log,            "(ii)"),
  N(time_us,        "()i"),
  N(time_ms,        "()i"),
  N(sleep_ms,       "(i)"),
  N(random,         "()i"),
  N(random_range,   "(ii)i"),
  N(random_seed,    "(i)"),
  N(sin,            "(i)i"),
  N(cos,            "(i)i"),

  N(game_run,       "(ii)i"),
  N(game_exit,      "()"),
  N(game_width,     "()i"),
  N(game_height,    "()i"),
  N(game_frame,     "()i"),
  N(game_display,   "(ii)i"),
  N(game_stats,     "(ii)i"),

  N(image_new,      "(ii)i"),
  N(image_free,     "(i)"),
  N(image_width,    "(i)i"),
  N(image_height,   "(i)i"),
  N(image_put,      "(iiiiiii)"),
  N(image_put8,     "(iiiiiiiii)"),
  N(image_decode,   "(ii)i"),
  N(image_load,     "(ii)i"),

  N(clear,          "(i)"),
  N(rect,           "(iiiii)"),
  N(sprite,         "(iiiiiiii)"),
  N(sprite_rot,     "(iiiiiiiiii)"),
  N(sprite_matrix,  "(iiiiiiiiiiii)"),
  N(quad,           "(iiiiiii)"),
  N(quad3d,         "(iiiiiiiiiiii)i"),
  N(mode7,          "(iiiiiiiiii)"),

  N(layer_init,     "(iiiiii)i"),
  N(layer_free,     "(i)"),
  N(layer_tileset,  "(ii)"),
  N(layer_set,      "(iiii)"),
  N(layer_get,      "(iii)i"),
  N(layer_put,      "(iiiiiii)"),
  N(layer_redraw,   "(i)"),
  N(layer_draw,     "(iii)"),

  N(text_print,     "(iiii)"),
  N(text_clear,     "()"),
  N(text_color,     "(i)"),
  N(text_draw,      "()"),

  N(pad,            "()i"),
  N(pad_pressed,    "()i"),
  N(pad_released,   "()i"),
  N(axis_x,         "()i"),
  N(axis_y,         "()i"),
  N(key_next,       "()i"),
  N(mouse,          "(ii)i"),

  N(sample,         "(iiiii)i"),
  N(sfx_play,       "(iiiii)"),
  N(sfx_stop,       "(i)"),
  N(patch,          "(ii)"),
  N(fm_on,          "(iiiii)"),
  N(fm_off,         "(i)"),
  N(fm_freq,        "(ii)"),
  N(note_freq,      "(i)i"),
  N(master_volume,  "(ii)"),
  N(voice_volume,   "(iii)"),
  N(audio_stop_all, "()"),
  N(music_play,     "(iiii)i"),
  N(music_stop,     "()"),
  N(music_playing,  "()i"),
};

bool wasm_fpc_register(void)
{
  return wasm_runtime_register_natives("env", g_natives,
                                       sizeof(g_natives) /
                                       sizeof(g_natives[0]));
}
