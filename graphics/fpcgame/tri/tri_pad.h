// Gamepad state as the SDK sees it: one 32-bit word, a bit per input, plus the
// analogue stick. The CPU firmware fills it from whatever pad is attached.
#pragma once
#include <stdint.h>

#define TRI_PAD_UP     (1u << 0)
#define TRI_PAD_DOWN   (1u << 1)
#define TRI_PAD_LEFT   (1u << 2)
#define TRI_PAD_RIGHT  (1u << 3)
#define TRI_PAD_A      (1u << 4)
#define TRI_PAD_B      (1u << 5)
#define TRI_PAD_C      (1u << 6)
#define TRI_PAD_D      (1u << 7)
#define TRI_PAD_STICK  (1u << 8)     // stick click
#define TRI_PAD_PRESENT (1u << 31)   // a pad answered this frame

typedef struct {
	uint32_t buttons;    // TRI_PAD_* bits
	int8_t x, y;         // stick, -127..127, y positive = up
} tri_pad_t;
