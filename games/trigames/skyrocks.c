// Skyrocks: fly the ship, shoot rocks. Layers, sprites, blends, text, FM sound.
// Designed for the square-pixel 16:9 canvas (424x240); the playfield spans it.
#include <string.h>
#include <stdlib.h>
#include "tri_sdk.h"

// VRAM plan. VDP1: sheet at 0. VDP2: tiles 0, maps 0x10000/0x12000, text font 0x18000, text map 0x1A000.
#define SHEET_W 64
#define SHEET_H 32
#define N_ROCKS 16
#define N_SHOTS 8

static struct { int16_t x, y, dx, dy; uint8_t big, alive; } rock[N_ROCKS];
static struct { int16_t x, y; bool live; } shot[N_SHOTS];
static int ship_x, ship_y, score, lives, hit_flash;
static uint32_t t;

static const tri_fm_patch_t laser = { .alg = 0, .feedback = 2,
	.op = { { 3, 0, 0, 200, 255, 120, 0, 200 }, { 1, 0, 0, 255, 255, 60, 0, 120 } } };
static const tri_fm_patch_t boom = { .alg = 0, .feedback = 7,
	.op = { { 1, 0, 3, 255, 255, 40, 0, 60 }, { 0, 0, 0, 255, 255, 30, 0, 40 } } };

static void load(void) {
	// ---- VDP1: procedural sheet: ship 16x16 at (0,0), rock 16x16 at (16,0), shot 8x8 at (32,0), big rock 32x16 at (0,16)
	static uint8_t sheet[SHEET_W * SHEET_H];
	memset(sheet, 0, sizeof sheet);
	for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
		int dy = y - 8, r2 = (x - 8) * (x - 8) + dy * dy;
		sheet[y * SHEET_W + x] = (x < 14 - abs(dy) * 2 && x > 1) ? (x > 10 ? 1 : 2) : 0;
		sheet[y * SHEET_W + 16 + x] = r2 < 56 ? ((x * 7 + y * 3) % 5 ? 3 : 4) : 0;
	}
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) sheet[y * SHEET_W + 32 + x] = (x > 1 && x < 6 && y > 2 && y < 5) ? 5 : 0;
	for (int y = 0; y < 16; y++) for (int x = 0; x < 32; x++) {
		int dx = (x - 16) / 2, dy = y - 8;
		sheet[(16 + y) * SHEET_W + x] = dx * dx + dy * dy < 56 ? ((x * 5 + y * 7) % 6 ? 3 : 4) : 0;
	}
	tri_vram_upload(TRI_VDP1, 0, sheet, sizeof sheet);
	tri_sheet_define(0, 0, SHEET_W, SHEET_H);
	static const uint16_t pal1[6] = { 0, TRI_RGB555(31, 31, 31), TRI_RGB555(10, 18, 31), TRI_RGB555(14, 12, 10), TRI_RGB555(20, 18, 16), TRI_RGB555(31, 28, 8) };
	tri_palette_set(TRI_VDP1, 0, 0, pal1, 6);

	// ---- VDP2: landscape (layer 0) and clouds (layer 1), text on layer 2
	static uint8_t tiles[8 * 64];
	memset(tiles, 0, sizeof tiles);
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
		tiles[1 * 64 + y * 8 + x] = ((x + y) & 3) == 0 ? 2 : 1;
		tiles[2 * 64 + y * 8 + x] = y < 2 ? 3 : ((x + y) & 3) == 0 ? 2 : 1;
		tiles[3 * 64 + y * 8 + x] = ((x * 3 + y * 5) % 7) < 3 ? 4 : 5;
		tiles[4 * 64 + y * 8 + x] = 6;
		tiles[5 * 64 + y * 8 + x] = x >= 7 - y ? 6 : 0;
		tiles[6 * 64 + y * 8 + x] = x <= y ? 6 : 0;
		tiles[7 * 64 + y * 8 + x] = (x == 3 && y == 3) ? 7 : 0;
	}
	tri_vram_upload(TRI_VDP2, 0, tiles, sizeof tiles);
	tri_tileset_define(0, 0, 8);
	static uint16_t map0[64 * 32], map1[64 * 32];
	for (int x = 0; x < 64; x++) {
		int h = 20 + ((x * 7) % 5) - (((x / 4) * 3) % 4);
		for (int y = 0; y < 32; y++) {
			uint16_t e = 0;
			if (y == h) e = TRI_TILE(2, x & 1, 0, 0);
			else if (y > h) e = TRI_TILE(((x * 13 + y * 7) % 11) == 0 ? 3 : 1, 0, 0, 0);
			else if (((x * 31 + y * 17) % 23) == 0) e = TRI_TILE(7, 0, 0, 0);
			map0[y * 64 + x] = e;
		}
	}
	memset(map1, 0, sizeof map1);
	for (int c = 0; c < 12; c++) {
		int cx = (c * 17) % 60, cy = 2 + (c * 5) % 12, w = 2 + c % 3;
		map1[cy * 64 + cx] = TRI_TILE(5, 0, 0, 0);
		for (int i = 1; i <= w; i++) map1[cy * 64 + cx + i] = TRI_TILE(4, 0, 0, 0);
		map1[cy * 64 + cx + w + 1] = TRI_TILE(6, 0, 0, 0);
	}
	tri_vram_upload(TRI_VDP2, 0x10000, map0, sizeof map0); tri_tilemap_define(0, 0x10000, 6, 5);
	tri_vram_upload(TRI_VDP2, 0x12000, map1, sizeof map1); tri_tilemap_define(1, 0x12000, 6, 5);
	static const uint16_t pal2[8] = { 0, TRI_RGB555(10, 18, 6), TRI_RGB555(6, 26, 8), TRI_RGB555(14, 12, 10),
	                                  TRI_RGB555(20, 20, 24), TRI_RGB555(24, 24, 28), TRI_RGB555(30, 30, 31), TRI_RGB555(31, 31, 20) };
	tri_palette_set(TRI_VDP2, 0, 0, pal2, 8);
	tri_backdrop_set(TRI_RGB555(3, 5, 14));
	tri_layer_set(0, 0, 0, 0, TRI_LAYER_BELOW);
	tri_layer_set(1, 0, 1, 0, TRI_LAYER_ABOVE);
	tri_text_init(2, TRI_LAYER_ABOVE);
	tri_fm_patch_set(0, &laser);
	tri_fm_patch_set(1, &boom);
}

static void spawn_rock(int i) {
	rock[i].x = tri_screen_width() + tri_random_range(0, 64); rock[i].y = tri_random_range(8, 200);
	rock[i].dx = -1 - tri_random_range(0, 2); rock[i].dy = tri_random_range(-1, 1);
	rock[i].big = (i % 4 == 0); rock[i].alive = 1;
}

static void init(void) {
	ship_x = 40; ship_y = 120; score = 0; lives = 3; hit_flash = 0; t = 0;
	memset(shot, 0, sizeof shot);
	for (int i = 0; i < N_ROCKS; i++) { spawn_rock(i); rock[i].x = (int16_t)tri_random_range(160, tri_screen_width()); }
	tri_text_clear();
}

static void update(void) {
	ship_x += tri_pad_axis_x() / 32; ship_y -= tri_pad_axis_y() / 32;
	if (ship_x < 0) ship_x = 0;
	if (ship_x > tri_screen_width() - 16) ship_x = tri_screen_width() - 16;
	if (ship_y < 0) ship_y = 0;
	if (ship_y > 224) ship_y = 224;
	if ((tri_pad_pressed() & TRI_PAD_A) || ((tri_pad_buttons() & TRI_PAD_A) && t % 8 == 0))
		for (int i = 0; i < N_SHOTS; i++) if (!shot[i].live) {
			shot[i].live = true; shot[i].x = ship_x + 14; shot[i].y = ship_y + 4;
			tri_fm_note_on(i & 3, 0, tri_audio_note_freq(84 + (int)(t % 3)), 120, 120);
			break;
		}
	for (int i = 0; i < N_SHOTS; i++) if (shot[i].live) { shot[i].x += 6; if (shot[i].x > tri_screen_width()) shot[i].live = false; }
	for (int i = 0; i < N_ROCKS; i++) {
		if (!rock[i].alive) { if (t % 30 == 0) spawn_rock(i); continue; }
		rock[i].x += rock[i].dx; rock[i].y += rock[i].dy;
		int w = rock[i].big ? 32 : 16;
		if (rock[i].x < -w) spawn_rock(i);
		if (rock[i].y < -16) rock[i].y = 200; else if (rock[i].y > 200) rock[i].y = -16;
		for (int s = 0; s < N_SHOTS; s++)
			if (shot[s].live && shot[s].x + 6 > rock[i].x && shot[s].x + 2 < rock[i].x + w && shot[s].y + 5 > rock[i].y && shot[s].y + 3 < rock[i].y + 16) {
				shot[s].live = false; rock[i].alive = 0; score += rock[i].big ? 50 : 20;
				tri_fm_note_on(4 + (i & 3), 1, tri_audio_note_freq(rock[i].big ? 36 : 48), 200, 200);
			}
		if (ship_x + 12 > rock[i].x + 2 && ship_x + 2 < rock[i].x + w - 2 && ship_y + 12 > rock[i].y + 2 && ship_y + 2 < rock[i].y + 14 && !hit_flash) {
			hit_flash = 30; lives--; rock[i].alive = 0;
			tri_fm_note_on(8, 1, tri_audio_note_freq(30), 255, 255);
			if (lives <= 0) { score = 0; lives = 3; }
		}
	}
	if (hit_flash) hit_flash--;
	t++;
}

static void draw(void) {
	tri_layer_scroll(0, (int)(t / 2), 4);
	tri_layer_scroll(1, (int)(t / 4), (int)((t / 32) % 3));
	for (int i = 0; i < N_SHOTS; i++) if (shot[i].live) tri_sprite_draw(shot[i].x, shot[i].y, 0, 32, 0, 8, 8, 0, TRI_SPRITE_BLEND_ADD);
	for (int i = 0; i < N_ROCKS; i++) if (rock[i].alive) {
		if (rock[i].big) tri_sprite_draw_rotated(rock[i].x + 16, rock[i].y + 8, 0, 0, 16, 32, 16, 0, 0, (int)(t * 8 + i * 300) & 4095, 256);
		else tri_sprite_draw(rock[i].x, rock[i].y, 0, 16, 0, 16, 16, 0, ((t >> 3) & i & 1) ? TRI_SPRITE_HFLIP : 0);
	}
	if (!(hit_flash & 4)) tri_sprite_draw(ship_x, ship_y, 0, 0, 0, 16, 16, 0, 0);
	if (t % 6 == 0) tri_text_print(1, 1, "SCORE %6d   SHIPS %d", score, lives);
}

TRI_GAME("Skyrocks", TRI_VIDEO_SQUARE, load, init, update, draw);
