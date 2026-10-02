// SDK text: the 8x8 system font as a tileset on a game-owned tile layer.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "tri_sdk_internal.h"
#include "font8x8_basic.h"

#define TEXT_MAP_LOG2W 6     // 64 x 32 characters: covers the widest canvas (53 x 30)
#define TEXT_MAP_LOG2H 5
#define TEXT_MAX 64

static int text_map = -1, text_pal;

void tri_text_init(int layer, int prio) {
	tri_text_init_at(TRI_TEXT_FONT_VRAM, TRI_TEXT_MAP_VRAM, TRI_TEXT_TILESET, TRI_TEXT_TILEMAP, TRI_TEXT_PALETTE, layer, prio);
}

void tri_text_init_at(uint32_t font_vram, uint32_t map_vram, int tileset, int tilemap, int pal, int layer, int prio) {
	if (!tri_check(layer >= 0 && layer < 4, "tri_text_init", "layer must be 0..3 (4 is the overlay's)")) return;
	if (!tri_check(pal >= 0 && pal < 15, "tri_text_init", "palette out of range (15 is the overlay's)")) return;
	// tile 0 empty, tiles 1..95 = glyphs 0x20..0x7E, foreground index 1, background transparent
	uint8_t tiles[8 * 64];
	memset(tiles, 0, 64);
	tri_vram_upload(TRI_VDP2, font_vram, tiles, 64);
	for (int c = 0x20; c < 0x7F; c += 8) {
		int n = 0;
		for (; n < 8 && c + n < 0x7F; n++)
			for (int y = 0; y < 8; y++) {
				uint8_t bits = (uint8_t)font8x8_basic[c + n][y];
				for (int x = 0; x < 8; x++) tiles[n * 64 + y * 8 + x] = (bits >> x) & 1;
			}
		tri_vram_upload(TRI_VDP2, font_vram + (uint32_t)(c - 0x20 + 1) * 64, tiles, (uint32_t)n * 64);
	}
	tri_tileset_define(tileset, font_vram, 96);
	tri_tilemap_define(tilemap, map_vram, TEXT_MAP_LOG2W, TEXT_MAP_LOG2H);
	static const uint16_t pal_entries[2] = { 0, TRI_RGB555(31, 31, 31) };
	tri_palette_set(TRI_VDP2, pal, 0, pal_entries, 2);
	tri_layer_set(layer, tileset, tilemap, pal, prio);
	text_map = tilemap; text_pal = pal;
	tri_text_clear();
}

void tri_text_reset(void) { text_map = -1; }

static bool text_ready(const char *fn) { return tri_check(text_map >= 0, fn, "tri_text_init not called"); }

void tri_text_color(uint16_t rgb555) {
	if (!text_ready("tri_text_color")) return;
	tri_palette_entry(TRI_VDP2, text_pal, 1, rgb555);
}

void tri_text_clear(void) {
	if (!text_ready("tri_text_clear")) return;
	tri_tilemap_fill(text_map, 0, 0, 1 << TEXT_MAP_LOG2W, 1 << TEXT_MAP_LOG2H, 0);
}

static int put(int col, int row, const char *s) {
	uint16_t e[TEXT_MAX];
	int n = 0;
	for (; s[n] && n < TEXT_MAX; n++) {
		unsigned c = (unsigned char)s[n];
		e[n] = (c >= 0x20 && c < 0x7F) ? (uint16_t)(c - 0x20 + 1) : 1;   // unknown characters print as '!'
	}
	tri_tilemap_write(text_map, col, row, e, n);
	return n;
}

void tri_text_print(int col, int row, const char *fmt, ...) {
	if (!text_ready("tri_text_print")) return;
	char buf[TEXT_MAX + 1];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	put(col, row, buf);
}

void tri_text_center(int row, const char *fmt, ...) {
	if (!text_ready("tri_text_center")) return;
	char buf[TEXT_MAX + 1];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	int n = (int)strlen(buf), col = (tri_text_columns() - n) / 2;
	put(col < 0 ? 0 : col, row, buf);
}
