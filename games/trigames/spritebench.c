// Sprite bench: hundreds of objects, flips, blends and affine transforms, with the numbers on screen.
// Runs in whatever video mode the console is set to (TRI_VIDEO_ANY) and fills the canvas it gets.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tri_sdk.h"

#define MAX_OBJ 400
static struct { int16_t x, y, dx, dy; uint8_t cell, pal, kind; } obj[MAX_OBJ];
static int n_obj = 200;
static uint32_t t;
static int sheet_src;                        // 0 SRAM, 1 PSRAM, 2 pulled from PSRAM into SRAM by tri_vram_copy; changes every 5 s
static char psram_line[48];                  // CPU-side PSRAM check result
TRI_PSRAM static uint32_t big[1 << 18];      // 1 MB in the CPU board's PSRAM

static void load(void) {
	static uint8_t sheet[128 * 16];
	for (int c = 0; c < 8; c++) for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
		int dx = x - 8, dy = y - 8, r2 = dx * dx + dy * dy; uint8_t p = 0;
		switch (c) {
		case 0: p = r2 < 49 ? (r2 < 20 ? 2 : 1) : 0; break;
		case 1: p = (x > 1 && x < 14 && y > 1 && y < 14) ? ((x + y) & 1 ? 3 : 1) : 0; break;
		case 2: p = (abs(dx) + abs(dy) < 8) ? 4 : 0; break;
		case 3: p = (x == y || x == 15 - y) ? 2 : (r2 < 64 && r2 > 36 ? 1 : 0); break;
		case 4: p = (y > 12 - abs(dx)) ? 3 : 0; break;
		case 5: p = ((x / 4 + y / 4) & 1) ? 4 : 2; break;
		case 6: p = (r2 < 64) ? (uint8_t)(1 + (r2 / 16)) : 0; break;
		default: p = (x == 0 || y == 0 || x == 15 || y == 15) ? 3 : 0; break;
		}
		sheet[y * 128 + c * 16 + x] = p;
	}
	tri_vram_upload(TRI_VDP1, 0, sheet, sizeof sheet);                 // sheet 0: VDP1 SRAM
	tri_sheet_define(0, 0, 128, 16);
	tri_vram_upload(TRI_VDP1, TRI_VRAM_PSRAM, sheet, sizeof sheet);     // sheet 1: the same pixels in VDP1's PSRAM
	tri_sheet_define(1, TRI_VRAM_PSRAM, 128, 16);
	tri_vram_copy(TRI_VDP1, 4096, TRI_VRAM_PSRAM, sizeof sheet);         // sheet 2: pulled back into SRAM on the VDP
	tri_sheet_define(2, 4096, 128, 16);
	for (int pal = 0; pal < 8; pal++) {
		int h = pal * 4;
		uint16_t e[6] = { 0, TRI_RGB555(31 - h, 8 + h, h * 2), TRI_RGB555(31, 31, 31), TRI_RGB555(h, 31, 31 - h), TRI_RGB555(31 - h, 31 - h, 31), TRI_RGB555(31, 16 + h / 2, 4) };
		tri_palette_set(TRI_VDP1, pal, 0, e, 6);
	}
	tri_backdrop_set(TRI_RGB555(2, 2, 6));
	tri_text_init(2, TRI_LAYER_ABOVE);
}

static void init(void) {
	for (int i = 0; i < MAX_OBJ; i++) {
		obj[i].x = (int16_t)tri_random_range(0, tri_screen_width() - 16); obj[i].y = (int16_t)tri_random_range(0, 224);
		obj[i].dx = (int16_t)(tri_random_range(1, 3) * (tri_random() & 1 ? 1 : -1));
		obj[i].dy = (int16_t)(tri_random_range(1, 3) * (tri_random() & 1 ? 1 : -1));
		obj[i].cell = (uint8_t)(i & 7); obj[i].pal = (uint8_t)((i / 8) & 7);
		obj[i].kind = (uint8_t)(i % 4);           // 0 plain, 1 flipped, 2 blended, 3 affine
	}
	t = 0;
	tri_text_clear();
	// CPU-side PSRAM: the TRI_PSRAM array and an allocation, pattern-checked once
	size_t sz = tri_psram_size();
	if (!sz) { snprintf(psram_line, sizeof psram_line, "cpu psram: none"); return; }
	uint32_t bad = 0;
	for (uint32_t i = 0; i < (1u << 18); i++) big[i] = i * 0x9E3779B1u;
	for (uint32_t i = 0; i < (1u << 18); i++) if (big[i] != i * 0x9E3779B1u) bad++;
	uint8_t *blk = tri_psram_alloc(1u << 20);
	if (blk) { memset(blk, 0x5A, 1u << 20); for (uint32_t i = 0; i < (1u << 20); i += 4093) if (blk[i] != 0x5A) bad++; }
	snprintf(psram_line, sizeof psram_line, "cpu psram %lu KB %s, free %lu KB", (unsigned long)(sz >> 10), bad || !blk ? "BAD" : "ok", (unsigned long)(tri_psram_free() >> 10));
}

static void update(void) {
	uint32_t p = tri_pad_pressed();
	if ((p & TRI_PAD_A) && n_obj < MAX_OBJ) n_obj += 32;
	if ((p & TRI_PAD_B) && n_obj > 32) n_obj -= 32;
	if (n_obj > MAX_OBJ) n_obj = MAX_OBJ;
	for (int i = 0; i < n_obj; i++) {
		obj[i].x += obj[i].dx; obj[i].y += obj[i].dy;
		if (obj[i].x < -8 || obj[i].x > tri_screen_width() - 8) obj[i].dx = -obj[i].dx;
		if (obj[i].y < -8 || obj[i].y > 232) obj[i].dy = -obj[i].dy;
	}
	t++;
	sheet_src = (int)((t / 300) % 3);
}

static void draw(void) {
	int sh = sheet_src;
	for (int i = 0; i < n_obj; i++) {
		int sx = obj[i].cell * 16;
		switch (obj[i].kind) {
		case 0: tri_sprite_draw(obj[i].x, obj[i].y, sh, sx, 0, 16, 16, obj[i].pal, 0); break;
		case 1: tri_sprite_draw(obj[i].x, obj[i].y, sh, sx, 0, 16, 16, obj[i].pal, ((t >> 4) & 1) ? TRI_SPRITE_HFLIP : TRI_SPRITE_VFLIP); break;
		case 2: tri_sprite_draw(obj[i].x, obj[i].y, sh, sx, 0, 16, 16, obj[i].pal, (i & 8) ? TRI_SPRITE_BLEND_HALF : TRI_SPRITE_BLEND_ADD); break;
		default: tri_sprite_draw_rotated(obj[i].x + 8, obj[i].y + 8, sh, sx, 0, 16, 16, obj[i].pal, 0, (int)(t * 16 + i * 64) & 4095, 128 + ((tri_sin((int)(t * 8 + i * 32) & 4095) + 256) >> 1)); break;
		}
	}
	if (t % 10 == 0) {
		const tri_stats_t *s = tri_stats();
		tri_text_print(1, 1, "objects %3d (A/B +-32) 1/4 affine  %s", n_obj, tri_video_mode_name(tri_video_mode()));
		tri_text_print(1, 2, "cpu frame %3lu us  max %3lu us", (unsigned long)s->frame_time_us, (unsigned long)s->frame_time_max_us);
		tri_text_print(1, 3, "sheet: %s (cycles every 5 s)   ", sheet_src == 0 ? "VDP1 SRAM " : sheet_src == 1 ? "VDP1 PSRAM" : "PSRAM->SRAM copy");
		tri_text_print(1, 4, "%s", psram_line);
	}
}

TRI_GAME("Sprite bench", TRI_VIDEO_ANY, load, init, update, draw);
