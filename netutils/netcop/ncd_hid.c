/****************************************************************************
 * apps/netutils/netcop/ncd_hid.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * netcopd: the coprocessor's BLE keyboards, mice and game pads into NuttX
 * input devices (docs/netcop.md 5.9 and 7.8), through /dev/vinput
 * (fpc/drivers/netcop):
 *
 *   - keys: the HID usages as the USB HID keyboard driver turns them into
 *     keyboard events (drivers/usbhost/usbhost_hidkbd.c, its US tables,
 *     CONFIG_HIDKBD_REPORT_MODIFIERS): a character (Shift and Caps Lock
 *     applied, Ctrl making it a control character), else a special key;
 *     the modifier keys themselves as special keys; the consumer page's
 *     volume keys.  A key's release repeats what its press said, and the
 *     keys a device holds are released when it disconnects;
 *   - the mouse: its buttons and movement (/dev/mouseble);
 *   - game pads: /dev/ajoy0, the stick (or the hat, which wins) and eight
 *     buttons: bit 0 A, 1 B, 2 X, 3 Y, 4 L, 5 R, 6 Select, 7 Start, from
 *     the usual HID gamepad numbering (A 1, B 2, X 4, Y 5, L 7, R 8,
 *     Select 11, Start 12, the sticks' clicks 14 and 15).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/input/ajoystick.h>
#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/mouse.h>

#include "fpc_vinput.h"
#include "ncd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HID_CAPSLOCK   0x39
#define HID_LCTRL      0xe0
#define HID_RGUI       0xe7

#define MOD_CTRL       0x11      /* HID modifier byte: LCtrl | RCtrl */
#define MOD_SHIFT      0x22      /* LShift | RShift */

#define NHELD          16

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct held_s
{
  uint8_t  dev;
  uint8_t  page;
  uint16_t usage;
  uint32_t kind;                 /* What the press was */
  uint32_t code;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_vfd = -1;
static bool g_caps;
static struct held_s g_held[NHELD];
static uint8_t g_nheld;

/* Usage 0x00-0x67 as a character, without and with Shift (the USB
 * keyboard driver's tables)
 */

static const uint8_t g_lc[0x68] =
{
  0x00, 0x00, 0x00, 0x00, 0x61, 0x62, 0x63, 0x64,  /* 00 */
  0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,  /* 08 */
  0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74,  /* 10 */
  0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x31, 0x32,  /* 18 */
  0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,  /* 20 */
  0x0a, 0x1b, 0x7f, 0x09, 0x20, 0x2d, 0x3d, 0x5b,  /* 28 */
  0x5d, 0x5c, 0x00, 0x3b, 0x27, 0x60, 0x2c, 0x2e,  /* 30 */
  0x2f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 38 */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 40 */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 48 */
  0x00, 0x00, 0x00, 0x00, 0x2f, 0x2a, 0x2d, 0x2b,  /* 50 */
  0x0a, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,  /* 58 */
  0x38, 0x39, 0x30, 0x2e, 0x00, 0x00, 0x00, 0x3d,  /* 60 */
};

static const uint8_t g_uc[0x68] =
{
  0x00, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44,  /* 00 */
  0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c,  /* 08 */
  0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54,  /* 10 */
  0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x21, 0x40,  /* 18 */
  0x23, 0x24, 0x25, 0x5e, 0x26, 0x2a, 0x28, 0x29,  /* 20 */
  0x0a, 0x1b, 0x7f, 0x09, 0x20, 0x5f, 0x2b, 0x7b,  /* 28 */
  0x7d, 0x7c, 0x00, 0x3a, 0x22, 0x7e, 0x3c, 0x3e,  /* 30 */
  0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 38 */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 40 */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 48 */
  0x00, 0x00, 0x00, 0x00, 0x2f, 0x2a, 0x2d, 0x2b,  /* 50 */
  0x0a, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,  /* 58 */
  0x38, 0x39, 0x30, 0x2e, 0x00, 0x00, 0x00, 0x3d,  /* 60 */
};

/* Usages 0x28-0x66 that are special keys */

static const uint8_t g_special[0x67 - 0x28] =
{
  KEYCODE_ENTER, 0, KEYCODE_FWDDEL, KEYCODE_BACKDEL, 0, 0, 0, 0,  /* 28 */
  0, 0, 0, 0, 0, 0, 0, 0,                                         /* 30 */
  0, KEYCODE_CAPSLOCK, KEYCODE_F1, KEYCODE_F2, KEYCODE_F3,        /* 38 */
  KEYCODE_F4, KEYCODE_F5, KEYCODE_F6,
  KEYCODE_F7, KEYCODE_F8, KEYCODE_F9, KEYCODE_F10, KEYCODE_F11,   /* 40 */
  KEYCODE_F12, KEYCODE_PRTSCRN, KEYCODE_SCROLLLOCK,
  KEYCODE_PAUSE, KEYCODE_INSERT, KEYCODE_HOME, KEYCODE_PAGEUP,    /* 48 */
  KEYCODE_FWDDEL, KEYCODE_END, KEYCODE_PAGEDOWN, KEYCODE_RIGHT,
  KEYCODE_LEFT, KEYCODE_DOWN, KEYCODE_UP, KEYCODE_NUMLOCK,        /* 50 */
  0, 0, 0, 0,
  KEYCODE_ENTER, 0, 0, 0, 0, 0, 0, 0,                             /* 58 */
  0, 0, 0, 0, 0, 0, KEYCODE_POWER                                 /* 60 */
};

static const uint8_t g_modkey[8] =
{
  KEYCODE_LCTRL, KEYCODE_LSHIFT, KEYCODE_LALT, KEYCODE_LGUI,
  KEYCODE_RCTRL, KEYCODE_RSHIFT, KEYCODE_RALT, KEYCODE_RGUI
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void vput(FAR const struct vinput_s *ev)
{
  if (g_vfd < 0)
    {
      g_vfd = open(VINPUT_PATH, O_WRONLY);
      if (g_vfd < 0)
        {
          return;
        }
    }

  write(g_vfd, ev, sizeof(*ev));
}

static void key_event(uint32_t kind, uint32_t code)
{
  struct vinput_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = VINPUT_KEY;
  ev.kind = kind;
  ev.code = code;
  vput(&ev);
}

/* A key's press as a keyboard event: false if it is none */

static bool key_map(uint8_t mods, uint8_t page, uint16_t usage,
                    FAR uint32_t *kind, FAR uint32_t *code)
{
  bool shift;
  uint8_t ch;

  if (page == NC_HID_PAGE_CONSUMER)
    {
      *kind = KEYBOARD_SPECPRESS;
      *code = usage == 0xe9 ? KEYCODE_VOLUP :
              usage == 0xea ? KEYCODE_VOLDOWN :
              usage == 0xe2 ? KEYCODE_MUTE : 0;
      return *code != 0;
    }

  if (page != NC_HID_PAGE_KEYBOARD)
    {
      return false;
    }

  if (usage >= HID_LCTRL && usage <= HID_RGUI)
    {
      *kind = KEYBOARD_SPECPRESS;
      *code = g_modkey[usage - HID_LCTRL];
      return true;
    }

  if (usage < sizeof(g_lc))
    {
      shift = (mods & MOD_SHIFT) != 0 || g_caps;
      ch = shift ? g_uc[usage] : g_lc[usage];
      if (ch != 0)
        {
          *kind = KEYBOARD_PRESS;
          *code = (mods & MOD_CTRL) != 0 ? ch & 0x1f : ch;
          return true;
        }
    }

  if (usage >= 0x28 && usage < 0x67 && g_special[usage - 0x28] != 0)
    {
      *kind = KEYBOARD_SPECPRESS;
      *code = g_special[usage - 0x28];
      return true;
    }

  return false;
}

static void hid_key(FAR const struct nc_ble_key *k)
{
  bool down = (k->flags & NC_BLE_KEY_DOWN) != 0;
  uint32_t kind;
  uint32_t code;
  int i;

  /* Caps Lock is a mode, not a key (as the USB driver has it) */

  if (k->page == NC_HID_PAGE_KEYBOARD && k->usage == HID_CAPSLOCK)
    {
      g_caps ^= down;
      return;
    }

  for (i = 0; i < g_nheld; i++)
    {
      if (g_held[i].dev == k->dev && g_held[i].page == k->page &&
          g_held[i].usage == k->usage)
        {
          break;
        }
    }

  if (!down)
    {
      if (i < g_nheld)
        {
          key_event(g_held[i].kind == KEYBOARD_PRESS ? KEYBOARD_RELEASE :
                    KEYBOARD_SPECREL, g_held[i].code);
          g_held[i] = g_held[--g_nheld];
        }

      return;
    }

  if (!key_map(k->mods, k->page, k->usage, &kind, &code))
    {
      return;
    }

  if (i == g_nheld && g_nheld < NHELD)
    {
      g_held[g_nheld].dev   = k->dev;
      g_held[g_nheld].page  = k->page;
      g_held[g_nheld].usage = k->usage;
      g_nheld++;
    }

  if (i < g_nheld)
    {
      g_held[i].kind = kind;
      g_held[i].code = code;
    }

  key_event(kind, code);
}

static void hid_mouse(FAR const struct nc_ble_mouse *m)
{
  struct vinput_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type    = VINPUT_MOUSE;
  ev.buttons = (m->buttons & 1 ? MOUSE_BUTTON_1 : 0) |
               (m->buttons & 2 ? MOUSE_BUTTON_2 : 0) |
               (m->buttons & 4 ? MOUSE_BUTTON_3 : 0);
  ev.x       = m->dx;
  ev.y       = m->dy;
  ev.wheel   = m->wheel;
  vput(&ev);
}

static void hid_pad(FAR const struct nc_ble_gamepad *p)
{
  static const int16_t hx[8] =
  {
    0, 32767, 32767, 32767, 0, -32767, -32767, -32767
  };

  static const int16_t hy[8] =
  {
    -32767, -32767, 0, 32767, 32767, 32767, 0, -32767
  };

  struct vinput_s ev;
  uint32_t b = p->buttons;

  memset(&ev, 0, sizeof(ev));
  ev.type = VINPUT_PAD;
  if (p->hat < 8)
    {
      ev.x = hx[p->hat];
      ev.y = hy[p->hat];
    }
  else
    {
      ev.x = (p->axis_mask & (1 << NC_PAD_X)) ? p->axes[NC_PAD_X] : 0;
      ev.y = (p->axis_mask & (1 << NC_PAD_Y)) ? p->axes[NC_PAD_Y] : 0;
    }

  ev.buttons = (b & 0x0001 ? AJOY_BUTTON_1_BIT : 0) |             /* A */
               (b & 0x0002 ? AJOY_BUTTON_2_BIT : 0) |             /* B */
               (b & 0x000c ? AJOY_BUTTON_3_BIT : 0) |             /* X */
               (b & 0x0010 ? AJOY_BUTTON_4_BIT : 0) |             /* Y */
               (b & 0x2040 ? AJOY_BUTTON_5_BIT : 0) |             /* L */
               (b & 0x40a0 ? AJOY_BUTTON_6_BIT : 0) |             /* R */
               (b & 0x0400 ? AJOY_BUTTON_7_BIT : 0) |        /* Select */
               (b & 0x0800 ? AJOY_BUTTON_8_BIT : 0);          /* Start */
  vput(&ev);
}

/* A device went: let go of what it held */

static void hid_release(uint8_t dev)
{
  int i = 0;

  while (i < g_nheld)
    {
      if (g_held[i].dev == dev)
        {
          key_event(g_held[i].kind == KEYBOARD_PRESS ? KEYBOARD_RELEASE :
                    KEYBOARD_SPECREL, g_held[i].code);
          g_held[i] = g_held[--g_nheld];
        }
      else
        {
          i++;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ncd_hid_init(void)
{
  g_vfd = open(VINPUT_PATH, O_WRONLY);
}

void ncd_hid_event(uint16_t code, FAR const uint8_t *p, size_t n)
{
  struct nc_rd rd;

  nc_rd_init(&rd, p, n);
  switch (code)
    {
      case NC_EV_BLE_KEY:
        {
          struct nc_ble_key k;

          if (nc_dec_ble_key(&rd, &k) == 0)
            {
              hid_key(&k);
            }
        }
        break;

      case NC_EV_BLE_MOUSE:
        {
          struct nc_ble_mouse m;

          if (nc_dec_ble_mouse(&rd, &m) == 0)
            {
              hid_mouse(&m);
            }
        }
        break;

      case NC_EV_BLE_GAMEPAD:
        {
          struct nc_ble_gamepad g;

          if (nc_dec_ble_gamepad(&rd, &g) == 0)
            {
              hid_pad(&g);
            }
        }
        break;

      case NC_EV_BLE_CONN:
        {
          struct nc_ble_dev d;

          if (nc_dec_ble_dev(&rd, &d) == 0 &&
              d.state != NC_BLE_ST_CONNECTED)
            {
              hid_release(d.id);
            }
        }
        break;

      default:
        break;
    }
}
