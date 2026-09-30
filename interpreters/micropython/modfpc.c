/****************************************************************************
 * apps/interpreters/micropython/modfpc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fpc module: windows on the FPC desktop for Python programs, through
 * the desktop's client interface (apps/include/graphics/deskwin.h): the
 * desktop draws and lays out, this task only queues commands and reads
 * events, so a program that raises, loops or is killed never holds the
 * desktop up.
 *
 *   import fpc
 *   w = fpc.Window("Hello", 360, 200)
 *   w.clear()
 *   w.rect(20, 20, 120, 60, fpc.rgb(40, 120, 220), radius=12)
 *   w.text(30, 40, "Hello, world", "text", font=fpc.HEADING)
 *   b = w.button("Quit", 250, 150, on_click=lambda b: w.close())
 *   w.run()
 *
 * Colours are RGB565 numbers (fpc.rgb()), (r, g, b) tuples, or the
 * theme's roles by name ("text", "accent", "window_bg" ...).  Waiting for
 * events (event(), run()) watches for Ctrl-C.  Windows still open when
 * the interpreter ends are closed (mp_fpc_deinit()).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxkeys.h>

#include <graphics/deskwin.h>

#include "py/mphal.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include "py/runtime.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_WINDOWS       16               /* A program's, at once */
#define SLICE_MS          20               /* Waiting, between Ctrl-C checks */

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct fpc_window_s
{
  mp_obj_base_t base;
  struct deskwin_s *d;                     /* NULL once closed */
  mp_obj_t widgets[DESKWIN_WIDGETS + 1];   /* By id */
  mp_obj_t images[DESKWIN_IMAGES];
  bool gone;                               /* CLOSE seen */
} fpc_window_t;

typedef struct fpc_widget_s
{
  mp_obj_base_t base;
  fpc_window_t *win;
  uint8_t id;
  uint8_t type;
  mp_obj_t on_click;
  mp_obj_t on_change;
  mp_obj_t on_activate;
} fpc_widget_t;

typedef struct fpc_image_s
{
  mp_obj_base_t base;
  fpc_window_t *win;
  int num;                                 /* -1 once freed */
  int w;
  int h;
} fpc_image_t;

/* This interpreter's open windows (the port's state: several
 * interpreters run at once)
 */

struct fpc_state_s
{
  fpc_window_t *win[MAX_WINDOWS];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const mp_obj_type_t fpc_window_type;
static const mp_obj_type_t fpc_widget_type;
static const mp_obj_type_t fpc_image_type;

static const qstr g_event_fields[] =
{
  MP_QSTR_type, MP_QSTR_x, MP_QSTR_y, MP_QSTR_w, MP_QSTR_h,
  MP_QSTR_buttons, MP_QSTR_key, MP_QSTR_char, MP_QSTR_mods,
  MP_QSTR_widget, MP_QSTR_kind, MP_QSTR_value
};

/****************************************************************************
 * Private Functions: helpers
 ****************************************************************************/

static struct fpc_state_s *state(bool make)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();

  if (p->fpc == NULL && make)
    {
      p->fpc = calloc(1, sizeof(struct fpc_state_s));
      if (p->fpc == NULL)
        {
          mp_raise_type(&mp_type_MemoryError);
        }
    }

  return p->fpc;
}

static void registry(fpc_window_t *w, bool add)
{
  struct fpc_state_s *st = state(add);
  int i;

  for (i = 0; st != NULL && i < MAX_WINDOWS; i++)
    {
      if (add && st->win[i] == NULL)
        {
          st->win[i] = w;
          return;
        }
      else if (!add && st->win[i] == w)
        {
          st->win[i] = NULL;
          return;
        }
    }

  if (add)
    {
      mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("too many windows"));
    }
}

/* A colour: RGB565, (r, g, b), or a theme role's name */

static uint16_t colour(mp_obj_t o)
{
  if (mp_obj_is_str(o))
    {
      int c = deskwin_colour(mp_obj_str_get_str(o));

      if (c < 0)
        {
          mp_raise_ValueError(MP_ERROR_TEXT("no such colour"));
        }

      return c;
    }

  if (mp_obj_is_type(o, &mp_type_tuple) || mp_obj_is_type(o, &mp_type_list))
    {
      size_t n;
      mp_obj_t *v;
      int r;
      int g;
      int b;

      mp_obj_get_array(o, &n, &v);
      if (n != 3)
        {
          mp_raise_ValueError(MP_ERROR_TEXT("a colour is (r, g, b)"));
        }

      r = mp_obj_get_int(v[0]) & 255;
      g = mp_obj_get_int(v[1]) & 255;
      b = mp_obj_get_int(v[2]) & 255;
      return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

  return mp_obj_get_int(o) & 0xffff;
}

static int clamp(mp_int_t a)
{
  return a < 0 ? 0 : a > 255 ? 255 : a;
}

static struct deskwin_s *open_d(mp_obj_t self_in)
{
  fpc_window_t *self = MP_OBJ_TO_PTR(self_in);

  if (self->d == NULL)
    {
      mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the window is closed"));
    }

  return self->d;
}

static void win_close(fpc_window_t *self)
{
  if (self->d != NULL)
    {
      deskwin_close(self->d);
      self->d = NULL;
      registry(self, false);
    }
}

/* Hand the commands over, watching for Ctrl-C while the desktop is busy */

static void flush(struct deskwin_s *d)
{
  while (deskwin_flush(d, SLICE_MS) == -ETIMEDOUT)
    {
      mp_event_handle_nowait();
    }
}

/* A key's name: the character it types, or what it is */

static mp_obj_t key_name(int code)
{
  static const struct
  {
    uint8_t code;
    const char *name;
  }

  names[] =
  {
    { KEYCODE_UP, "Up" }, { KEYCODE_DOWN, "Down" },
    { KEYCODE_LEFT, "Left" }, { KEYCODE_RIGHT, "Right" },
    { KEYCODE_HOME, "Home" }, { KEYCODE_END, "End" },
    { KEYCODE_PAGEUP, "PageUp" }, { KEYCODE_PAGEDOWN, "PageDown" },
    { KEYCODE_INSERT, "Insert" }, { KEYCODE_FWDDEL, "Delete" },
    { KEYCODE_BACKDEL, "Backspace" }, { KEYCODE_ENTER, "Enter" },
    { KEYCODE_LCTRL, "Ctrl" }, { KEYCODE_RCTRL, "Ctrl" },
    { KEYCODE_LSHIFT, "Shift" }, { KEYCODE_RSHIFT, "Shift" },
    { KEYCODE_LALT, "Alt" }, { KEYCODE_RALT, "Alt" },
    { KEYCODE_LGUI, "Gui" }, { KEYCODE_RGUI, "Gui" },
  };

  char buf[8];
  size_t i;

  switch (code)
    {
      case NX_KEY_BACKSPACE:
        return MP_OBJ_NEW_QSTR(MP_QSTR_Backspace);

      case NX_KEY_TAB:
        return MP_OBJ_NEW_QSTR(MP_QSTR_Tab);

      case NX_KEY_ENTER:
        return MP_OBJ_NEW_QSTR(MP_QSTR_Enter);

      case NX_KEY_ESCAPE:
        return MP_OBJ_NEW_QSTR(MP_QSTR_Escape);

      default:
        break;
    }

  if (!NX_KEY_ISSPECIAL(code))
    {
      buf[0] = code >= 0x20 && code < 0x7f ? code : '?';
      return mp_obj_new_str(buf, 1);
    }

  code = NX_KEY_KEYCODE(code);
  if (code >= KEYCODE_F1 && code <= KEYCODE_F24)
    {
      snprintf(buf, sizeof(buf), "F%d", code - KEYCODE_F1 + 1);
      return mp_obj_new_str_from_cstr(buf);
    }

  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
      if (names[i].code == code)
        {
          return mp_obj_new_str_from_cstr(names[i].name);
        }
    }

  return MP_OBJ_NEW_QSTR(MP_QSTR_);
}

static mp_obj_t make_event(fpc_window_t *self, const struct deskwin_event_s *e)
{
  mp_obj_t items[MP_ARRAY_SIZE(g_event_fields)];
  mp_obj_t widget = mp_const_none;
  char ch[1];

  if (e->type == DESKWIN_EV_WIDGET && e->id >= 1 &&
      e->id <= DESKWIN_WIDGETS && self->widgets[e->id] != MP_OBJ_NULL)
    {
      widget = self->widgets[e->id];
    }

  ch[0] = e->ch;
  items[0]  = MP_OBJ_NEW_SMALL_INT(e->type);
  items[1]  = MP_OBJ_NEW_SMALL_INT(e->x);
  items[2]  = MP_OBJ_NEW_SMALL_INT(e->y);
  items[3]  = MP_OBJ_NEW_SMALL_INT(e->w);
  items[4]  = MP_OBJ_NEW_SMALL_INT(e->h);
  items[5]  = MP_OBJ_NEW_SMALL_INT(e->buttons);
  items[6]  = e->type == DESKWIN_EV_KEY || e->type == DESKWIN_EV_KEYUP ?
              key_name(e->key) : mp_const_none;
  items[7]  = e->ch != 0 && e->ch < 0x80 ? mp_obj_new_str(ch, 1) :
              MP_OBJ_NEW_QSTR(MP_QSTR_);
  items[8]  = MP_OBJ_NEW_SMALL_INT(e->mods);
  items[9]  = widget;
  items[10] = MP_OBJ_NEW_SMALL_INT(e->kind);
  items[11] = mp_obj_new_int(e->value);
  return mp_obj_new_attrtuple(g_event_fields, MP_ARRAY_SIZE(g_event_fields),
                              items);
}

/* The next event within timeout ms (-1: until one comes), or None;
 * Ctrl-C raises meanwhile.  A closed window gives CLOSE.
 */

static mp_obj_t next_event(fpc_window_t *self, mp_int_t timeout)
{
  struct deskwin_event_s e;
  mp_uint_t start = mp_hal_ticks_ms();

  if (self->d == NULL || self->gone)
    {
      memset(&e, 0, sizeof(e));
      e.type = DESKWIN_EV_CLOSE;
      return make_event(self, &e);
    }

  flush(self->d);
  for (; ; )
    {
      int slice = SLICE_MS;
      int ret;

      mp_event_handle_nowait();
      if (timeout >= 0)
        {
          mp_int_t left = timeout - (mp_int_t)(mp_hal_ticks_ms() - start);

          if (left <= 0)
            {
              slice = 0;
            }
          else if (left < slice)
            {
              slice = left;
            }
        }

      ret = deskwin_event(self->d, &e, slice);
      if (ret > 0)
        {
          if (e.type == DESKWIN_EV_CLOSE)
            {
              self->gone = true;
            }

          return make_event(self, &e);
        }

      if (ret < 0)
        {
          self->gone = true;
          memset(&e, 0, sizeof(e));
          e.type = DESKWIN_EV_CLOSE;
          return make_event(self, &e);
        }

      if (slice == 0 && timeout >= 0)
        {
          return mp_const_none;
        }
    }
}

/****************************************************************************
 * Private Functions: Window
 ****************************************************************************/

static mp_obj_t window_make_new(const mp_obj_type_t *type, size_t n_args,
                                size_t n_kw, const mp_obj_t *all_args)
{
  enum
  {
    ARG_title, ARG_width, ARG_height, ARG_frames
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_title, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_Python)} },
    { MP_QSTR_width, MP_ARG_INT, {.u_int = 400} },
    { MP_QSTR_height, MP_ARG_INT, {.u_int = 300} },
    { MP_QSTR_frames, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
  };

  mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
  fpc_window_t *self;
  mp_map_t kw;
  int i;

  mp_map_init_fixed_table(&kw, n_kw, all_args + n_args);
  mp_arg_parse_all(n_args, all_args, &kw, MP_ARRAY_SIZE(allowed), allowed,
                   args);

  self = mp_obj_malloc_with_finaliser(fpc_window_t, type);
  self->d    = NULL;
  self->gone = false;
  for (i = 0; i <= DESKWIN_WIDGETS; i++)
    {
      self->widgets[i] = MP_OBJ_NULL;
    }

  for (i = 0; i < DESKWIN_IMAGES; i++)
    {
      self->images[i] = MP_OBJ_NULL;
    }

  registry(self, true);
  self->d = deskwin_open(mp_obj_str_get_str(args[ARG_title].u_obj),
                         args[ARG_width].u_int, args[ARG_height].u_int);
  if (self->d == NULL)
    {
      int err = errno;

      registry(self, false);
      if (err == ENODEV)
        {
          mp_raise_msg(&mp_type_OSError,
                       MP_ERROR_TEXT("no desktop is running"));
        }

      mp_raise_OSError(err);
    }

  if (args[ARG_frames].u_bool)
    {
      deskwin_frames(self->d, true);
    }

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t window_close(mp_obj_t self_in)
{
  win_close(MP_OBJ_TO_PTR(self_in));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(window_close_obj, window_close);

static mp_obj_t window_flush(mp_obj_t self_in)
{
  flush(open_d(self_in));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(window_flush_obj, window_flush);

static mp_obj_t window_size(mp_obj_t self_in)
{
  mp_obj_t t[2];
  int w;
  int h;

  deskwin_size(open_d(self_in), &w, &h);
  t[0] = MP_OBJ_NEW_SMALL_INT(w);
  t[1] = MP_OBJ_NEW_SMALL_INT(h);
  return mp_obj_new_tuple(2, t);
}

static MP_DEFINE_CONST_FUN_OBJ_1(window_size_obj, window_size);

static mp_obj_t window_title(mp_obj_t self_in, mp_obj_t title)
{
  deskwin_title(open_d(self_in), mp_obj_str_get_str(title));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(window_title_obj, window_title);

static mp_obj_t window_frames(size_t n_args, const mp_obj_t *args)
{
  deskwin_frames(open_d(args[0]), n_args < 2 || mp_obj_is_true(args[1]));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_frames_obj, 1, 2,
                                           window_frames);

/* Drawing */

static mp_obj_t window_clear(size_t n_args, const mp_obj_t *args)
{
  deskwin_clear(open_d(args[0]), n_args > 1 && args[1] != mp_const_none ?
                colour(args[1]) : deskwin_colour("window_bg"));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_clear_obj, 1, 2,
                                           window_clear);

static mp_obj_t window_fill(size_t n_args, const mp_obj_t *args)
{
  deskwin_fill(open_d(args[0]), mp_obj_get_int(args[1]),
               mp_obj_get_int(args[2]), mp_obj_get_int(args[3]),
               mp_obj_get_int(args[4]), colour(args[5]));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_fill_obj, 6, 6,
                                           window_fill);

static mp_obj_t window_rect(size_t n_args, const mp_obj_t *pos,
                            mp_map_t *kw)
{
  enum
  {
    ARG_x, ARG_y, ARG_w, ARG_h, ARG_c, ARG_alpha, ARG_radius
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_colour, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_alpha, MP_ARG_INT, {.u_int = 255} },
    { MP_QSTR_radius, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];

  mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(allowed), allowed,
                   a);
  deskwin_rect(open_d(pos[0]), a[ARG_x].u_int, a[ARG_y].u_int,
               a[ARG_w].u_int, a[ARG_h].u_int, colour(a[ARG_c].u_obj),
               clamp(a[ARG_alpha].u_int),
               a[ARG_radius].u_int);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_KW(window_rect_obj, 6, window_rect);

static mp_obj_t window_circle(size_t n_args, const mp_obj_t *pos,
                              mp_map_t *kw)
{
  enum
  {
    ARG_cx, ARG_cy, ARG_r, ARG_c, ARG_alpha
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_cx, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_cy, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_r, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_colour, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_alpha, MP_ARG_INT, {.u_int = 255} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  mp_int_t r;

  mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(allowed), allowed,
                   a);
  r = a[ARG_r].u_int;
  deskwin_rect(open_d(pos[0]), a[ARG_cx].u_int - r, a[ARG_cy].u_int - r,
               2 * r, 2 * r, colour(a[ARG_c].u_obj), clamp(a[ARG_alpha].u_int),
               r);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_KW(window_circle_obj, 5, window_circle);

static mp_obj_t window_gradient(size_t n_args, const mp_obj_t *pos,
                                mp_map_t *kw)
{
  enum
  {
    ARG_x, ARG_y, ARG_w, ARG_h, ARG_c0, ARG_c1, ARG_vertical, ARG_alpha
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_top, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_bottom, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_vertical, MP_ARG_BOOL, {.u_bool = true} },
    { MP_QSTR_alpha, MP_ARG_INT, {.u_int = 255} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];

  mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(allowed), allowed,
                   a);
  deskwin_gradient(open_d(pos[0]), a[ARG_x].u_int, a[ARG_y].u_int,
                   a[ARG_w].u_int, a[ARG_h].u_int, colour(a[ARG_c0].u_obj),
                   colour(a[ARG_c1].u_obj),
                   clamp(a[ARG_alpha].u_int),
                   a[ARG_vertical].u_bool);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_KW(window_gradient_obj, 7, window_gradient);

static mp_obj_t window_line(size_t n_args, const mp_obj_t *pos,
                            mp_map_t *kw)
{
  enum
  {
    ARG_x0, ARG_y0, ARG_x1, ARG_y1, ARG_c, ARG_alpha
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_x0, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_y0, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_x1, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_y1, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_colour, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_alpha, MP_ARG_INT, {.u_int = 255} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];

  mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(allowed), allowed,
                   a);
  deskwin_line(open_d(pos[0]), a[ARG_x0].u_int, a[ARG_y0].u_int,
               a[ARG_x1].u_int, a[ARG_y1].u_int, colour(a[ARG_c].u_obj),
               clamp(a[ARG_alpha].u_int));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_KW(window_line_obj, 6, window_line);

static mp_obj_t window_text(size_t n_args, const mp_obj_t *pos,
                            mp_map_t *kw)
{
  enum
  {
    ARG_x, ARG_y, ARG_s, ARG_c, ARG_font, ARG_alpha
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_colour, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_font, MP_ARG_INT, {.u_int = DESKWIN_FONT_BODY} },
    { MP_QSTR_alpha, MP_ARG_INT, {.u_int = 255} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  const char *s;
  int font;

  mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(allowed), allowed,
                   a);
  font = a[ARG_font].u_int;
  if (font < 0 || font >= DESKWIN_FONTS)
    {
      mp_raise_ValueError(MP_ERROR_TEXT("no such font"));
    }

  s = mp_obj_str_get_str(mp_obj_is_str(a[ARG_s].u_obj) ? a[ARG_s].u_obj :
                         mp_obj_str_make_new(&mp_type_str, 1, 0,
                                             &a[ARG_s].u_obj));
  deskwin_text(open_d(pos[0]), a[ARG_x].u_int, a[ARG_y].u_int, font,
               a[ARG_c].u_obj != MP_OBJ_NULL && a[ARG_c].u_obj !=
               mp_const_none ? colour(a[ARG_c].u_obj) :
               deskwin_colour("text"),
               clamp(a[ARG_alpha].u_int), s);
  return MP_OBJ_NEW_SMALL_INT(deskwin_text_width(font, s));
}

static MP_DEFINE_CONST_FUN_OBJ_KW(window_text_obj, 4, window_text);

static mp_obj_t window_scroll(size_t n_args, const mp_obj_t *args)
{
  deskwin_copy(open_d(args[0]), mp_obj_get_int(args[1]),
               mp_obj_get_int(args[2]), mp_obj_get_int(args[3]),
               mp_obj_get_int(args[4]), mp_obj_get_int(args[5]),
               mp_obj_get_int(args[6]));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_scroll_obj, 7, 7,
                                           window_scroll);

static mp_obj_t window_clip(size_t n_args, const mp_obj_t *args)
{
  if (n_args == 1 || args[1] == mp_const_none)
    {
      deskwin_clip(open_d(args[0]), 0, 0, 0, 0);
    }
  else if (n_args == 5)
    {
      deskwin_clip(open_d(args[0]), mp_obj_get_int(args[1]),
                   mp_obj_get_int(args[2]), mp_obj_get_int(args[3]),
                   mp_obj_get_int(args[4]));
    }
  else
    {
      mp_raise_TypeError(MP_ERROR_TEXT("clip(x, y, w, h) or clip()"));
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_clip_obj, 1, 5,
                                           window_clip);

/* Images */

static mp_obj_t window_load_image(size_t n_args, const mp_obj_t *args)
{
  fpc_window_t *self = MP_OBJ_TO_PTR(args[0]);
  struct deskwin_s *d = open_d(args[0]);
  fpc_image_t *img;
  char err[80];
  int w;
  int h;
  int n;

  n = deskwin_image_load(d, mp_obj_str_get_str(args[1]),
                         n_args > 2 ? mp_obj_get_int(args[2]) : 0,
                         n_args > 3 ? mp_obj_get_int(args[3]) : 0, &w, &h,
                         err, sizeof(err));
  if (n < 0)
    {
      mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s: %s"),
                        mp_obj_str_get_str(args[1]),
                        err[0] != '\0' ? err : strerror(-n));
    }

  img = mp_obj_malloc(fpc_image_t, &fpc_image_type);
  img->win = self;
  img->num = n;
  img->w   = w;
  img->h   = h;
  self->images[n] = MP_OBJ_FROM_PTR(img);
  return MP_OBJ_FROM_PTR(img);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_load_image_obj, 2, 4,
                                           window_load_image);

static mp_obj_t window_image(size_t n_args, const mp_obj_t *pos,
                             mp_map_t *kw)
{
  enum
  {
    ARG_img, ARG_x, ARG_y, ARG_sx, ARG_sy, ARG_w, ARG_h, ARG_alpha
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_image, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_sx, MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_sy, MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_w, MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_h, MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_alpha, MP_ARG_INT, {.u_int = 255} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  fpc_image_t *img;

  mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(allowed), allowed,
                   a);
  if (!mp_obj_is_type(a[ARG_img].u_obj, &fpc_image_type))
    {
      mp_raise_TypeError(MP_ERROR_TEXT("not an image"));
    }

  img = MP_OBJ_TO_PTR(a[ARG_img].u_obj);
  if (img->num < 0 || img->win != MP_OBJ_TO_PTR(pos[0]))
    {
      mp_raise_ValueError(MP_ERROR_TEXT("not this window's image"));
    }

  deskwin_image(open_d(pos[0]), img->num, a[ARG_sx].u_int, a[ARG_sy].u_int,
                a[ARG_w].u_int, a[ARG_h].u_int, a[ARG_x].u_int,
                a[ARG_y].u_int, clamp(a[ARG_alpha].u_int));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_KW(window_image_obj, 4, window_image);

/* Events */

static mp_obj_t window_event(size_t n_args, const mp_obj_t *args)
{
  return next_event(MP_OBJ_TO_PTR(args[0]),
                    n_args > 1 && args[1] != mp_const_none ?
                    mp_obj_get_int(args[1]) : -1);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_event_obj, 1, 2,
                                           window_event);

/* A widget's callback for an event of it */

static void dispatch(fpc_window_t *self, mp_obj_t ev)
{
  mp_obj_t *items;
  size_t n;
  fpc_widget_t *wg;
  mp_obj_t fn = mp_const_none;

  mp_obj_tuple_get(ev, &n, &items);
  if (mp_obj_get_int(items[0]) != DESKWIN_EV_WIDGET ||
      items[9] == mp_const_none)
    {
      return;
    }

  wg = MP_OBJ_TO_PTR(items[9]);
  switch (mp_obj_get_int(items[10]))
    {
      case DESKWIN_CLICK:
        fn = wg->on_click;
        break;

      case DESKWIN_CHANGE:
        fn = wg->on_change;
        break;

      case DESKWIN_ACTIVATE:
        fn = wg->on_activate;
        break;
    }

  if (fn != mp_const_none)
    {
      mp_call_function_1(fn, MP_OBJ_FROM_PTR(wg));
    }
}

/* run([handler]): events until the window is closed: widgets' callbacks,
 * then handler(event) for each (returning False ends it)
 */

static mp_obj_t window_run(size_t n_args, const mp_obj_t *args)
{
  fpc_window_t *self = MP_OBJ_TO_PTR(args[0]);
  mp_obj_t handler = n_args > 1 ? args[1] : mp_const_none;

  for (; ; )
    {
      mp_obj_t ev = next_event(self, -1);
      mp_obj_t *items;
      size_t n;

      if (ev == mp_const_none)
        {
          continue;
        }

      dispatch(self, ev);
      if (handler != mp_const_none)
        {
          mp_obj_t r = mp_call_function_1(handler, ev);

          if (r == mp_const_false)
            {
              break;
            }
        }

      mp_obj_tuple_get(ev, &n, &items);
      if (mp_obj_get_int(items[0]) == DESKWIN_EV_CLOSE || self->d == NULL)
        {
          break;
        }
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_run_obj, 1, 2,
                                           window_run);

/* Widgets: made with a free id */

static mp_obj_t new_widget(mp_obj_t self_in, int type, mp_obj_t text,
                           const char *items, int x, int y, int w, int h,
                           int value, int min, int max, int style,
                           mp_obj_t on_click, mp_obj_t on_change,
                           mp_obj_t on_activate)
{
  fpc_window_t *self = MP_OBJ_TO_PTR(self_in);
  struct deskwin_s *d = open_d(self_in);
  fpc_widget_t *wg;
  int id;

  for (id = 1; id <= DESKWIN_WIDGETS && self->widgets[id] != MP_OBJ_NULL;
       id++)
    {
    }

  if (id > DESKWIN_WIDGETS)
    {
      mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("too many widgets"));
    }

  wg = mp_obj_malloc(fpc_widget_t, &fpc_widget_type);
  wg->win         = self;
  wg->id          = id;
  wg->type        = type;
  wg->on_click    = on_click;
  wg->on_change   = on_change;
  wg->on_activate = on_activate;
  self->widgets[id] = MP_OBJ_FROM_PTR(wg);
  deskwin_widget(d, id, type, x, y, w, h,
                 text != mp_const_none ? mp_obj_str_get_str(text) : "",
                 items, value, min, max, style);
  return MP_OBJ_FROM_PTR(wg);
}

/* Lines of a sequence, "\n" between */

static const char *join_items(mp_obj_t seq)
{
  vstr_t v;
  mp_obj_t *items;
  size_t n;
  size_t i;

  if (seq == mp_const_none)
    {
      return "";
    }

  mp_obj_get_array(seq, &n, &items);
  vstr_init(&v, 64);
  for (i = 0; i < n; i++)
    {
      mp_obj_t s = mp_obj_is_str(items[i]) ? items[i] :
                   mp_obj_str_make_new(&mp_type_str, 1, 0, &items[i]);

      if (i > 0)
        {
          vstr_add_char(&v, '\n');
        }

      vstr_add_str(&v, mp_obj_str_get_str(s));
    }

  return vstr_null_terminated_str(&v);
}

enum
{
  W_TEXT, W_X, W_Y, W_W, W_H, W_VALUE, W_MIN, W_MAX, W_STYLE, W_ITEMS,
  W_PLACEHOLDER, W_FONT, W_ON_CLICK, W_ON_CHANGE, W_ON_ACTIVATE, W_ON_SELECT,
  W_N
};

/* All the widgets take these (keywords past the positions each has) */

static void widget_args(size_t n_args, const mp_obj_t *pos, mp_map_t *kw,
                        const qstr *order, size_t norder, mp_arg_val_t *a)
{
  static const qstr names[W_N] =
  {
    MP_QSTR_text, MP_QSTR_x, MP_QSTR_y, MP_QSTR_w, MP_QSTR_h,
    MP_QSTR_value, MP_QSTR_min, MP_QSTR_max, MP_QSTR_style, MP_QSTR_items,
    MP_QSTR_placeholder, MP_QSTR_font, MP_QSTR_on_click, MP_QSTR_on_change,
    MP_QSTR_on_activate, MP_QSTR_on_select
  };

  mp_arg_t allowed[W_N];
  mp_arg_val_t vals[W_N];
  size_t i;
  size_t j;

  /* The positional ones first, in order, then the rest */

  for (i = 0; i < norder; i++)
    {
      allowed[i].qst   = order[i];
      allowed[i].flags = MP_ARG_OBJ;
      allowed[i].defval.u_obj = mp_const_none;
    }

  for (j = 0; j < W_N; j++)
    {
      bool placed = false;
      size_t k;

      for (k = 0; k < norder; k++)
        {
          placed |= order[k] == names[j];
        }

      if (!placed)
        {
          allowed[i].qst   = names[j];
          allowed[i].flags = MP_ARG_KW_ONLY | MP_ARG_OBJ;
          allowed[i].defval.u_obj = mp_const_none;
          i++;
        }
    }

  mp_arg_parse_all(n_args, pos, kw, W_N, allowed, vals);
  for (i = 0; i < W_N; i++)
    {
      for (j = 0; j < W_N; j++)
        {
          if (allowed[i].qst == names[j])
            {
              a[j] = vals[i];
            }
        }
    }
}

static int opt_int(mp_arg_val_t *a, int i, int def)
{
  return a[i].u_obj == mp_const_none ? def : mp_obj_get_int(a[i].u_obj);
}

static mp_obj_t make_widget(size_t n_args, const mp_obj_t *pos,
                            mp_map_t *kw, int type)
{
  static const qstr text_xy[] =
  {
    MP_QSTR_text, MP_QSTR_x, MP_QSTR_y, MP_QSTR_w, MP_QSTR_h
  };

  static const qstr xy[] =
  {
    MP_QSTR_x, MP_QSTR_y, MP_QSTR_w, MP_QSTR_h
  };

  static const qstr list_xy[] =
  {
    MP_QSTR_x, MP_QSTR_y, MP_QSTR_w, MP_QSTR_h, MP_QSTR_items
  };

  static const qstr dropdown_xy[] =
  {
    MP_QSTR_x, MP_QSTR_y, MP_QSTR_items, MP_QSTR_value, MP_QSTR_w
  };

  mp_arg_val_t a[W_N];
  mp_obj_t text;
  mp_obj_t on_change;
  const char *items = "";
  int value;
  int style = 0;
  bool hastext = type == DESKWIN_BUTTON || type == DESKWIN_LABEL ||
                 type == DESKWIN_CHECK;

  if (hastext)
    {
      widget_args(n_args - 1, pos + 1, kw, text_xy, 5, a);
    }
  else if (type == DESKWIN_LIST)
    {
      widget_args(n_args - 1, pos + 1, kw, list_xy, 5, a);
    }
  else if (type == DESKWIN_DROPDOWN)
    {
      widget_args(n_args - 1, pos + 1, kw, dropdown_xy, 5, a);
    }
  else
    {
      widget_args(n_args - 1, pos + 1, kw, xy, 4, a);
    }

  text = a[W_TEXT].u_obj;
  if (text != mp_const_none && !mp_obj_is_str(text))
    {
      text = mp_obj_str_make_new(&mp_type_str, 1, 0, &text);
    }

  value = a[W_VALUE].u_obj == mp_const_true ? 1 :
          a[W_VALUE].u_obj == mp_const_false ? 0 : opt_int(a, W_VALUE, 0);
  if (type == DESKWIN_LABEL)
    {
      style = opt_int(a, W_FONT, 0) | opt_int(a, W_STYLE, 0);
    }
  else
    {
      style = opt_int(a, W_STYLE, 0);
    }

  if (type == DESKWIN_LIST || type == DESKWIN_DROPDOWN)
    {
      items = join_items(a[W_ITEMS].u_obj);
    }
  else if (type == DESKWIN_FIELD && a[W_PLACEHOLDER].u_obj != mp_const_none)
    {
      items = mp_obj_str_get_str(a[W_PLACEHOLDER].u_obj);
    }

  if (type == DESKWIN_LIST && a[W_VALUE].u_obj == mp_const_none)
    {
      value = -1;
    }

  on_change = a[W_ON_CHANGE].u_obj != mp_const_none ? a[W_ON_CHANGE].u_obj :
              a[W_ON_SELECT].u_obj;
  return new_widget(pos[0], type, text, items, opt_int(a, W_X, 0),
                    opt_int(a, W_Y, 0), opt_int(a, W_W, 0),
                    opt_int(a, W_H, 0), value, opt_int(a, W_MIN, 0),
                    opt_int(a, W_MAX, 100), style, a[W_ON_CLICK].u_obj,
                    on_change, a[W_ON_ACTIVATE].u_obj);
}

#define WIDGET_MAKER(name, type, min) \
  static mp_obj_t window_##name(size_t n_args, const mp_obj_t *pos, \
                                mp_map_t *kw) \
  { \
    return make_widget(n_args, pos, kw, type); \
  } \
  static MP_DEFINE_CONST_FUN_OBJ_KW(window_##name##_obj, min, \
                                    window_##name);

WIDGET_MAKER(button, DESKWIN_BUTTON, 4)
WIDGET_MAKER(label, DESKWIN_LABEL, 4)
WIDGET_MAKER(field, DESKWIN_FIELD, 3)
WIDGET_MAKER(slider, DESKWIN_SLIDER, 3)
WIDGET_MAKER(check, DESKWIN_CHECK, 4)
WIDGET_MAKER(switch_, DESKWIN_SWITCH, 3)
WIDGET_MAKER(progress, DESKWIN_PROGRESS, 3)
WIDGET_MAKER(list, DESKWIN_LIST, 3)
WIDGET_MAKER(dropdown, DESKWIN_DROPDOWN, 3)

static mp_obj_t window_del(mp_obj_t self_in)
{
  win_close(MP_OBJ_TO_PTR(self_in));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(window_del_obj, window_del);

static mp_obj_t window_enter(mp_obj_t self_in)
{
  return self_in;
}

static MP_DEFINE_CONST_FUN_OBJ_1(window_enter_obj, window_enter);

static mp_obj_t window_exit(size_t n_args, const mp_obj_t *args)
{
  win_close(MP_OBJ_TO_PTR(args[0]));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(window_exit_obj, 4, 4,
                                           window_exit);

static void window_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
  fpc_window_t *self = MP_OBJ_TO_PTR(self_in);

  if (dest[0] == MP_OBJ_NULL)
    {
      int w = 0;
      int h = 0;

      if (attr == MP_QSTR_closed)
        {
          dest[0] = mp_obj_new_bool(self->d == NULL || self->gone ||
                                    !deskwin_alive(self->d));
          return;
        }

      if (attr == MP_QSTR_width || attr == MP_QSTR_height)
        {
          if (self->d != NULL)
            {
              deskwin_size(self->d, &w, &h);
            }

          dest[0] = MP_OBJ_NEW_SMALL_INT(attr == MP_QSTR_width ? w : h);
          return;
        }
    }

  dest[1] = MP_OBJ_SENTINEL;               /* The methods */
}

static void window_print(const mp_print_t *print, mp_obj_t self_in,
                         mp_print_kind_t kind)
{
  fpc_window_t *self = MP_OBJ_TO_PTR(self_in);

  mp_printf(print, "<Window%s>", self->d == NULL ? " closed" : "");
}

static const mp_rom_map_elem_t window_locals_table[] =
{
  { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&window_close_obj) },
  { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&window_flush_obj) },
  { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&window_size_obj) },
  { MP_ROM_QSTR(MP_QSTR_title), MP_ROM_PTR(&window_title_obj) },
  { MP_ROM_QSTR(MP_QSTR_frames), MP_ROM_PTR(&window_frames_obj) },
  { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&window_clear_obj) },
  { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&window_fill_obj) },
  { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&window_rect_obj) },
  { MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&window_circle_obj) },
  { MP_ROM_QSTR(MP_QSTR_gradient), MP_ROM_PTR(&window_gradient_obj) },
  { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&window_line_obj) },
  { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&window_text_obj) },
  { MP_ROM_QSTR(MP_QSTR_scroll), MP_ROM_PTR(&window_scroll_obj) },
  { MP_ROM_QSTR(MP_QSTR_clip), MP_ROM_PTR(&window_clip_obj) },
  { MP_ROM_QSTR(MP_QSTR_load_image), MP_ROM_PTR(&window_load_image_obj) },
  { MP_ROM_QSTR(MP_QSTR_image), MP_ROM_PTR(&window_image_obj) },
  { MP_ROM_QSTR(MP_QSTR_event), MP_ROM_PTR(&window_event_obj) },
  { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&window_run_obj) },
  { MP_ROM_QSTR(MP_QSTR_button), MP_ROM_PTR(&window_button_obj) },
  { MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&window_label_obj) },
  { MP_ROM_QSTR(MP_QSTR_field), MP_ROM_PTR(&window_field_obj) },
  { MP_ROM_QSTR(MP_QSTR_slider), MP_ROM_PTR(&window_slider_obj) },
  { MP_ROM_QSTR(MP_QSTR_check), MP_ROM_PTR(&window_check_obj) },
  { MP_ROM_QSTR(MP_QSTR_switch), MP_ROM_PTR(&window_switch__obj) },
  { MP_ROM_QSTR(MP_QSTR_progress), MP_ROM_PTR(&window_progress_obj) },
  { MP_ROM_QSTR(MP_QSTR_list), MP_ROM_PTR(&window_list_obj) },
  { MP_ROM_QSTR(MP_QSTR_dropdown), MP_ROM_PTR(&window_dropdown_obj) },
  { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&window_del_obj) },
  { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&window_enter_obj) },
  { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&window_exit_obj) },
};

static MP_DEFINE_CONST_DICT(window_locals, window_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
  fpc_window_type, MP_QSTR_Window, MP_TYPE_FLAG_NONE,
  make_new, window_make_new,
  print, window_print,
  attr, window_attr,
  locals_dict, &window_locals);

/****************************************************************************
 * Private Functions: Widget
 ****************************************************************************/

static struct deskwin_s *widget_d(fpc_widget_t *wg)
{
  if (wg->id == 0 || wg->win->d == NULL)
    {
      mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the widget is gone"));
    }

  return wg->win->d;
}

static mp_obj_t widget_move(size_t n_args, const mp_obj_t *args)
{
  fpc_widget_t *wg = MP_OBJ_TO_PTR(args[0]);

  deskwin_widget_move(widget_d(wg), wg->id, mp_obj_get_int(args[1]),
                      mp_obj_get_int(args[2]),
                      n_args > 3 ? mp_obj_get_int(args[3]) : 0,
                      n_args > 4 ? mp_obj_get_int(args[4]) : 0);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(widget_move_obj, 3, 5,
                                           widget_move);

static mp_obj_t widget_remove(mp_obj_t self_in)
{
  fpc_widget_t *wg = MP_OBJ_TO_PTR(self_in);

  if (wg->id != 0)
    {
      if (wg->win->d != NULL)
        {
          deskwin_widget_remove(wg->win->d, wg->id);
        }

      wg->win->widgets[wg->id] = MP_OBJ_NULL;
      wg->id = 0;
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(widget_remove_obj, widget_remove);

static mp_obj_t widget_focus(mp_obj_t self_in)
{
  fpc_widget_t *wg = MP_OBJ_TO_PTR(self_in);

  deskwin_widget_set(widget_d(wg), wg->id, DESKWIN_SET_FOCUS, 0, NULL);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(widget_focus_obj, widget_focus);

static mp_obj_t widget_size(mp_obj_t self_in)
{
  fpc_widget_t *wg = MP_OBJ_TO_PTR(self_in);
  mp_obj_t t[2];
  int w;
  int h;

  deskwin_widget_size(widget_d(wg), wg->id, &w, &h);
  t[0] = MP_OBJ_NEW_SMALL_INT(w);
  t[1] = MP_OBJ_NEW_SMALL_INT(h);
  return mp_obj_new_tuple(2, t);
}

static MP_DEFINE_CONST_FUN_OBJ_1(widget_size_obj, widget_size);

static void widget_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
  fpc_widget_t *wg = MP_OBJ_TO_PTR(self_in);
  mp_obj_t *cb = attr == MP_QSTR_on_click ? &wg->on_click :
                 attr == MP_QSTR_on_change ? &wg->on_change :
                 attr == MP_QSTR_on_select ? &wg->on_change :
                 attr == MP_QSTR_on_activate ? &wg->on_activate : NULL;

  if (dest[0] == MP_OBJ_NULL)
    {
      /* Load */

      if (cb != NULL)
        {
          dest[0] = *cb;
        }
      else if (attr == MP_QSTR_id)
        {
          dest[0] = MP_OBJ_NEW_SMALL_INT(wg->id);
        }
      else if (attr == MP_QSTR_value || attr == MP_QSTR_text)
        {
          char text[256];
          int value = 0;

          if (!deskwin_widget_get(widget_d(wg), wg->id, &value, text,
                                  sizeof(text)))
            {
              dest[0] = mp_const_none;
            }
          else if (attr == MP_QSTR_text)
            {
              dest[0] = mp_obj_new_str_from_cstr(text);
            }
          else if (wg->type == DESKWIN_CHECK || wg->type == DESKWIN_SWITCH ||
                   (wg->type == DESKWIN_BUTTON))
            {
              dest[0] = mp_obj_new_bool(value != 0);
            }
          else if (wg->type == DESKWIN_LIST && value < 0)
            {
              dest[0] = mp_const_none;
            }
          else if (wg->type == DESKWIN_FIELD || wg->type == DESKWIN_LABEL)
            {
              dest[0] = mp_obj_new_str_from_cstr(text);
            }
          else
            {
              dest[0] = mp_obj_new_int(value);
            }
        }
      else
        {
          dest[1] = MP_OBJ_SENTINEL;
        }

      return;
    }

  if (dest[1] == MP_OBJ_NULL)
    {
      return;                                /* No deleting */
    }

  /* Store */

  if (cb != NULL)
    {
      *cb = dest[1];
    }
  else if (attr == MP_QSTR_value)
    {
      int v;

      if ((wg->type == DESKWIN_FIELD || wg->type == DESKWIN_LABEL) &&
          mp_obj_is_str(dest[1]))
        {
          deskwin_widget_set(widget_d(wg), wg->id, DESKWIN_SET_TEXT, 0,
                             mp_obj_str_get_str(dest[1]));
          dest[0] = MP_OBJ_NULL;
          return;
        }

      v = dest[1] == mp_const_none ? -1 :
          mp_obj_is_bool(dest[1]) ? mp_obj_is_true(dest[1]) :
          mp_obj_get_int(dest[1]);
      deskwin_widget_set(widget_d(wg), wg->id, DESKWIN_SET_VALUE, v, NULL);
    }
  else if (attr == MP_QSTR_text)
    {
      mp_obj_t s = mp_obj_is_str(dest[1]) ? dest[1] :
                   mp_obj_str_make_new(&mp_type_str, 1, 0, &dest[1]);

      deskwin_widget_set(widget_d(wg), wg->id, DESKWIN_SET_TEXT, 0,
                         mp_obj_str_get_str(s));
    }
  else if (attr == MP_QSTR_items)
    {
      deskwin_widget_set(widget_d(wg), wg->id, DESKWIN_SET_ITEMS, 0,
                         join_items(dest[1]));
    }
  else if (attr == MP_QSTR_enabled || attr == MP_QSTR_visible)
    {
      deskwin_widget_set(widget_d(wg), wg->id,
                         attr == MP_QSTR_enabled ? DESKWIN_SET_ENABLED :
                         DESKWIN_SET_VISIBLE, mp_obj_is_true(dest[1]),
                         NULL);
    }
  else if (attr == MP_QSTR_range)
    {
      mp_obj_t *mm;
      size_t n;
      char buf[32];

      mp_obj_get_array(dest[1], &n, &mm);
      if (n != 2)
        {
          mp_raise_ValueError(MP_ERROR_TEXT("range is (min, max)"));
        }

      snprintf(buf, sizeof(buf), "%d %d", (int)mp_obj_get_int(mm[0]),
               (int)mp_obj_get_int(mm[1]));
      deskwin_widget_set(widget_d(wg), wg->id, DESKWIN_SET_RANGE, 0, buf);
    }
  else
    {
      return;
    }

  dest[0] = MP_OBJ_NULL;                     /* Done */
}

static void widget_print(const mp_print_t *print, mp_obj_t self_in,
                         mp_print_kind_t kind)
{
  fpc_widget_t *wg = MP_OBJ_TO_PTR(self_in);

  mp_printf(print, "<Widget %d>", wg->id);
}

static const mp_rom_map_elem_t widget_locals_table[] =
{
  { MP_ROM_QSTR(MP_QSTR_move), MP_ROM_PTR(&widget_move_obj) },
  { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&widget_remove_obj) },
  { MP_ROM_QSTR(MP_QSTR_focus), MP_ROM_PTR(&widget_focus_obj) },
  { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&widget_size_obj) },
};

static MP_DEFINE_CONST_DICT(widget_locals, widget_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
  fpc_widget_type, MP_QSTR_Widget, MP_TYPE_FLAG_NONE,
  print, widget_print,
  attr, widget_attr,
  locals_dict, &widget_locals);

/****************************************************************************
 * Private Functions: Image
 ****************************************************************************/

static mp_obj_t image_free(mp_obj_t self_in)
{
  fpc_image_t *img = MP_OBJ_TO_PTR(self_in);

  if (img->num >= 0)
    {
      if (img->win->d != NULL)
        {
          deskwin_image_free(img->win->d, img->num);
        }

      img->win->images[img->num] = MP_OBJ_NULL;
      img->num = -1;
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(image_free_obj, image_free);

static void image_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
  fpc_image_t *img = MP_OBJ_TO_PTR(self_in);

  if (dest[0] == MP_OBJ_NULL &&
      (attr == MP_QSTR_width || attr == MP_QSTR_height))
    {
      dest[0] = MP_OBJ_NEW_SMALL_INT(attr == MP_QSTR_width ? img->w :
                                     img->h);
      return;
    }

  dest[1] = MP_OBJ_SENTINEL;
}

static const mp_rom_map_elem_t image_locals_table[] =
{
  { MP_ROM_QSTR(MP_QSTR_free), MP_ROM_PTR(&image_free_obj) },
};

static MP_DEFINE_CONST_DICT(image_locals, image_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
  fpc_image_type, MP_QSTR_Image, MP_TYPE_FLAG_NONE,
  attr, image_attr,
  locals_dict, &image_locals);

/****************************************************************************
 * Private Functions: the module
 ****************************************************************************/

static mp_obj_t fpc_rgb(mp_obj_t r, mp_obj_t g, mp_obj_t b)
{
  int rr = mp_obj_get_int(r) & 255;
  int gg = mp_obj_get_int(g) & 255;
  int bb = mp_obj_get_int(b) & 255;

  return MP_OBJ_NEW_SMALL_INT(((rr >> 3) << 11) | ((gg >> 2) << 5) |
                              (bb >> 3));
}

static MP_DEFINE_CONST_FUN_OBJ_3(fpc_rgb_obj, fpc_rgb);

static mp_obj_t fpc_colour(mp_obj_t role)
{
  return MP_OBJ_NEW_SMALL_INT(colour(role));
}

static MP_DEFINE_CONST_FUN_OBJ_1(fpc_colour_obj, fpc_colour);

static mp_obj_t fpc_text_width(size_t n_args, const mp_obj_t *args)
{
  return MP_OBJ_NEW_SMALL_INT(deskwin_text_width(
           n_args > 1 ? mp_obj_get_int(args[1]) : DESKWIN_FONT_BODY,
           mp_obj_str_get_str(args[0])));
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fpc_text_width_obj, 1, 2,
                                           fpc_text_width);

static mp_obj_t fpc_text_height(size_t n_args, const mp_obj_t *args)
{
  return MP_OBJ_NEW_SMALL_INT(deskwin_text_height(
           n_args > 0 ? mp_obj_get_int(args[0]) : DESKWIN_FONT_BODY));
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fpc_text_height_obj, 0, 1,
                                           fpc_text_height);

static const mp_rom_map_elem_t fpc_globals_table[] =
{
  { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fpc) },
  { MP_ROM_QSTR(MP_QSTR_Window), MP_ROM_PTR(&fpc_window_type) },
  { MP_ROM_QSTR(MP_QSTR_rgb), MP_ROM_PTR(&fpc_rgb_obj) },
  { MP_ROM_QSTR(MP_QSTR_colour), MP_ROM_PTR(&fpc_colour_obj) },
  { MP_ROM_QSTR(MP_QSTR_text_width), MP_ROM_PTR(&fpc_text_width_obj) },
  { MP_ROM_QSTR(MP_QSTR_text_height), MP_ROM_PTR(&fpc_text_height_obj) },

  /* Fonts */

  { MP_ROM_QSTR(MP_QSTR_BODY), MP_ROM_INT(DESKWIN_FONT_BODY) },
  { MP_ROM_QSTR(MP_QSTR_BOLD), MP_ROM_INT(DESKWIN_FONT_BOLD) },
  { MP_ROM_QSTR(MP_QSTR_SMALL), MP_ROM_INT(DESKWIN_FONT_SMALL) },
  { MP_ROM_QSTR(MP_QSTR_HEADING), MP_ROM_INT(DESKWIN_FONT_HEADING) },
  { MP_ROM_QSTR(MP_QSTR_DISPLAY), MP_ROM_INT(DESKWIN_FONT_DISPLAY) },
  { MP_ROM_QSTR(MP_QSTR_MONO), MP_ROM_INT(DESKWIN_FONT_MONO) },
  { MP_ROM_QSTR(MP_QSTR_MONO_BOLD), MP_ROM_INT(DESKWIN_FONT_MONOBOLD) },

  /* Labels: or'ed with a font */

  { MP_ROM_QSTR(MP_QSTR_DIM), MP_ROM_INT(8) },
  { MP_ROM_QSTR(MP_QSTR_CENTRE), MP_ROM_INT(16) },
  { MP_ROM_QSTR(MP_QSTR_RIGHT_ALIGN), MP_ROM_INT(32) },

  /* Buttons */

  { MP_ROM_QSTR(MP_QSTR_NORMAL), MP_ROM_INT(0) },
  { MP_ROM_QSTR(MP_QSTR_DEFAULT), MP_ROM_INT(1) },
  { MP_ROM_QSTR(MP_QSTR_DANGER), MP_ROM_INT(2) },
  { MP_ROM_QSTR(MP_QSTR_FLAT), MP_ROM_INT(3) },
  { MP_ROM_QSTR(MP_QSTR_TOGGLE), MP_ROM_INT(4) },

  /* Events */

  { MP_ROM_QSTR(MP_QSTR_MOUSE), MP_ROM_INT(DESKWIN_EV_MOUSE) },
  { MP_ROM_QSTR(MP_QSTR_KEY), MP_ROM_INT(DESKWIN_EV_KEY) },
  { MP_ROM_QSTR(MP_QSTR_KEYUP), MP_ROM_INT(DESKWIN_EV_KEYUP) },
  { MP_ROM_QSTR(MP_QSTR_RESIZE), MP_ROM_INT(DESKWIN_EV_RESIZE) },
  { MP_ROM_QSTR(MP_QSTR_CLOSE), MP_ROM_INT(DESKWIN_EV_CLOSE) },
  { MP_ROM_QSTR(MP_QSTR_EXPOSE), MP_ROM_INT(DESKWIN_EV_EXPOSE) },
  { MP_ROM_QSTR(MP_QSTR_FRAME), MP_ROM_INT(DESKWIN_EV_FRAME) },
  { MP_ROM_QSTR(MP_QSTR_TICK), MP_ROM_INT(DESKWIN_EV_TICK) },
  { MP_ROM_QSTR(MP_QSTR_FOCUS), MP_ROM_INT(DESKWIN_EV_FOCUS) },
  { MP_ROM_QSTR(MP_QSTR_WIDGET), MP_ROM_INT(DESKWIN_EV_WIDGET) },
  { MP_ROM_QSTR(MP_QSTR_CLICK), MP_ROM_INT(DESKWIN_CLICK) },
  { MP_ROM_QSTR(MP_QSTR_CHANGE), MP_ROM_INT(DESKWIN_CHANGE) },
  { MP_ROM_QSTR(MP_QSTR_ACTIVATE), MP_ROM_INT(DESKWIN_ACTIVATE) },

  /* Buttons of the mouse, modifiers of keys */

  { MP_ROM_QSTR(MP_QSTR_LEFT), MP_ROM_INT(DESKWIN_LEFT) },
  { MP_ROM_QSTR(MP_QSTR_MIDDLE), MP_ROM_INT(DESKWIN_CENTRE) },
  { MP_ROM_QSTR(MP_QSTR_RIGHT), MP_ROM_INT(DESKWIN_RIGHT) },
  { MP_ROM_QSTR(MP_QSTR_SHIFT), MP_ROM_INT(NX_KEY_SHIFT) },
  { MP_ROM_QSTR(MP_QSTR_CTRL), MP_ROM_INT(NX_KEY_CTRL) },
  { MP_ROM_QSTR(MP_QSTR_ALT), MP_ROM_INT(NX_KEY_ALT) },
};

static MP_DEFINE_CONST_DICT(fpc_globals, fpc_globals_table);

const mp_obj_module_t mp_module_fpc =
{
  .base    = { &mp_type_module },
  .globals = (mp_obj_dict_t *)&fpc_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fpc, mp_module_fpc);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The interpreter ends: its windows close */

void mp_fpc_deinit(void)
{
  struct fpc_state_s *st = state(false);
  int i;

  if (st == NULL)
    {
      return;
    }

  for (i = 0; i < MAX_WINDOWS; i++)
    {
      fpc_window_t *w = st->win[i];

      if (w != NULL)
        {
          st->win[i] = NULL;
          if (w->d != NULL)
            {
              deskwin_close(w->d);
              w->d = NULL;
            }
        }
    }

  mp_nuttx_port()->fpc = NULL;
  free(st);
}
