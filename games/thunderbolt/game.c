// Thunderbolt: WW2 side-scrolling shooter. Fly a P-47 over occupied Europe,
// guns (A), cannon (B), rockets (C) and bombs (stick click or D) against
// Bf 109s, Stukas, He 111s and the flak, tanks, trucks, bunkers and depots
// on the ground; an armoured train ends each mission.
// Designed for the square-pixel 16:9 canvas (424x240). Art from PixelLab
// (assets/raw), packed by assets/build_assets.py into three VDP1 sheets and
// one VDP2 tileset; the ground is a wang-tiled heightfield streamed into a
// 64x32 wrapping tilemap as the level scrolls.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "tri_sdk.h"
// FPC: the FPC's 3D extension of the SDK (tri_fpc3d.h: TRI_FPC3D), where
// there is one; on stock TRI the game builds and plays without the effects
#if defined(__has_include)
#if __has_include("tri_fpc3d.h")
#include "tri_fpc3d.h"
#endif
#endif
#include "assets.h"
#include "sprites.h"
#include "tiles.h"

#define H 240
#define FX(v) ((int32_t)(v) << 8)      // 8.8 fixed point positions and speeds
#define TOI(v) ((int)((v) >> 8))
#define SCROLL_SPEED 320               // 1.25 px per frame
#define MAP_W 64
#define MAP_H 32
#define LEVEL_CELLS 720                // 16 px cells; the train waits at the end
#define BASE_ROW 26                    // ground surface row (8 px) on flat land: y = 208; hills rise to row 22, dips to 28

// ---- level data (generated per mission) ----------------------------------------------------------------------
static uint8_t lh[LEVEL_CELLS + 64];   // surface row (8 px units) at each 16-px vertex column; neighbours differ by at most 1 (a ramp)
typedef struct { uint16_t cell; uint8_t kind; } lobj_t;
static lobj_t ldec[160];  static int n_dec;      // trees and houses (tiles)
static lobj_t lobj[160];  static int n_obj;      // ground targets (sprites)
static int next_obj;

enum { D_TREE = 1, D_HOUSE };
enum { E_NONE, E_BF109, E_BF109G, E_HE111, E_STUKA, E_TANK, E_FLAK, E_TRUCK, E_BUNKER, E_DEPOT, E_TRAIN };

// ---- entities ------------------------------------------------------------------------------------------------
#define N_ENT 40
#define N_SHOT 64
#define N_ESHOT 48
#define N_FX 48
typedef struct { uint8_t type, state, flash, ground; int32_t x, y, vx, vy; int16_t hp, t, aux, base_y; } ent_t;
typedef struct { uint8_t kind; int32_t x, y, vx, vy; int16_t t; } shot_t;
typedef struct { uint8_t kind, frame; int32_t x, y, vx, vy; int16_t t; int16_t scale; } fx_t;
static ent_t ent[N_ENT];
static shot_t shot[N_SHOT], eshot[N_ESHOT];
static fx_t fx[N_FX];

enum { S_GUN = 1, S_CANNON, S_ROCKET, S_BOMB };
enum { K_BULLET = 1, K_SHELL, K_FLAK };
enum { F_BOOM = 1, F_BOOMSMALL, F_SMOKE, F_FLASH, F_FLAKBURST, F_MUZZLE };

// ---- player and game state -------------------------------------------------------------------------------------
typedef struct { int32_t x, y, vx, vy; int hp, inv, gun_t, cannon_t, rocket_t, bomb_t, alt; bool alive; } player_t;
static player_t P;
static int32_t scroll;                 // 8.8 world x of the left screen edge
static int next_col;                   // next world 8-px column to stream into the level map
static int score, lives, rockets, bombs, cannon, level, hi_score;
static int spawn_t, ent_air;
static uint32_t t;
enum { GS_TITLE, GS_PLAY, GS_DEAD, GS_OVER, GS_CLEAR };
static int gs, gs_t;
static bool boss_spawned, boss_dead;
static int boss_hp, boss_hp_max;
static int msg_t; static char msg[40];

// ---- audio -----------------------------------------------------------------------------------------------------
enum { V_ENGINE = 0, V_GUN0 = 1, V_GUN1, V_GUN2, V_ROCKET0 = 4, V_ROCKET1, V_BOOM0 = 6, V_BOOM1, V_BOOM2, V_BOOM3, V_HIT = 10, V_FLAK = 11, V_MUS0 = 12, V_MUS1, V_MUS2, V_MUS3 };
static int gun_v, boom_v, rocket_v;
static void sfx(int voice, int sample, int vol, int x) { tri_sfx_play_pan(voice, sample, 65536, vol, (x - 212) / 2); }
static void sfx_boom(int sample, int vol, int x) { sfx(V_BOOM0 + (boom_v++ & 3), sample, vol, x); }

static const tri_fm_patch_t pat_bass = { .alg = 0, .feedback = 3,
	.op = { { 1, 0, 3, 150, 255, 90, 0, 150 }, { 1, 0, 0, 255, 255, 80, 90, 120 } } };
static const tri_fm_patch_t pat_snare = { .alg = 1, .feedback = 7,
	.op = { { 9, 3, 3, 200, 255, 160, 0, 200 }, { 1, 0, 0, 120, 255, 200, 0, 200 } } };
static const tri_fm_patch_t pat_lead = { .alg = 0, .feedback = 1,
	.op = { { 2, 0, 0, 90, 255, 60, 120, 120 }, { 1, 0, 1, 220, 240, 40, 160, 80 } } };
// Martial 4/4 loop, 120 bpm: 8 frames a step, 16 steps a bar, 4 bars.
#define STEP 8
#define NOTE(f, v, p, n, vel, len) { (f) * STEP, v, p, n, vel, (len) * STEP }
static const tri_note_t song_notes[] = {
	// bar 1: D minor
	NOTE(0, V_MUS0, 0, 38, 150, 2), NOTE(4, V_MUS0, 0, 38, 130, 2), NOTE(8, V_MUS0, 0, 45, 150, 2), NOTE(12, V_MUS0, 0, 38, 130, 2),
	NOTE(4, V_MUS1, 1, 60, 90, 1), NOTE(12, V_MUS1, 1, 60, 90, 1), NOTE(14, V_MUS1, 1, 60, 60, 1),
	NOTE(0, V_MUS2, 2, 62, 100, 3), NOTE(4, V_MUS2, 2, 65, 100, 3), NOTE(8, V_MUS2, 2, 69, 110, 6),
	// bar 2
	NOTE(16, V_MUS0, 0, 38, 150, 2), NOTE(20, V_MUS0, 0, 38, 130, 2), NOTE(24, V_MUS0, 0, 45, 150, 2), NOTE(28, V_MUS0, 0, 41, 130, 2),
	NOTE(20, V_MUS1, 1, 60, 90, 1), NOTE(28, V_MUS1, 1, 60, 90, 1), NOTE(30, V_MUS1, 1, 60, 60, 1), NOTE(31, V_MUS1, 1, 60, 60, 1),
	NOTE(16, V_MUS2, 2, 67, 100, 3), NOTE(20, V_MUS2, 2, 65, 100, 3), NOTE(24, V_MUS2, 2, 62, 110, 6),
	// bar 3: Bb
	NOTE(32, V_MUS0, 0, 34, 150, 2), NOTE(36, V_MUS0, 0, 34, 130, 2), NOTE(40, V_MUS0, 0, 41, 150, 2), NOTE(44, V_MUS0, 0, 34, 130, 2),
	NOTE(36, V_MUS1, 1, 60, 90, 1), NOTE(44, V_MUS1, 1, 60, 90, 1), NOTE(46, V_MUS1, 1, 60, 60, 1),
	NOTE(32, V_MUS2, 2, 70, 100, 3), NOTE(36, V_MUS2, 2, 69, 100, 3), NOTE(40, V_MUS2, 2, 65, 110, 6),
	// bar 4: A
	NOTE(48, V_MUS0, 0, 33, 150, 2), NOTE(52, V_MUS0, 0, 33, 130, 2), NOTE(56, V_MUS0, 0, 40, 150, 2), NOTE(60, V_MUS0, 0, 33, 130, 1), NOTE(62, V_MUS0, 0, 36, 130, 1),
	NOTE(52, V_MUS1, 1, 60, 90, 1), NOTE(60, V_MUS1, 1, 60, 90, 1), NOTE(61, V_MUS1, 1, 60, 60, 1), NOTE(62, V_MUS1, 1, 60, 70, 1), NOTE(63, V_MUS1, 1, 60, 80, 1),
	NOTE(48, V_MUS2, 2, 64, 100, 3), NOTE(52, V_MUS2, 2, 67, 100, 3), NOTE(56, V_MUS2, 2, 69, 110, 5), NOTE(62, V_MUS2, 2, 61, 100, 2),
};
#define N_NOTES (int)(sizeof song_notes / sizeof song_notes[0])
static tri_note_t song_sorted[N_NOTES];
static const tri_song_t song = { song_sorted, N_NOTES, 64 * STEP, true };
static void song_sort(void) {
	memcpy(song_sorted, song_notes, sizeof song_notes);
	for (int i = 1; i < N_NOTES; i++) { tri_note_t n = song_sorted[i]; int j = i; while (j > 0 && song_sorted[j - 1].frame > n.frame) { song_sorted[j] = song_sorted[j - 1]; j--; } song_sorted[j] = n; }
}

// ---- helpers ---------------------------------------------------------------------------------------------------
static uint32_t hash(uint32_t v) { v ^= v >> 16; v *= 0x7feb352d; v ^= v >> 15; v *= 0x846ca68b; v ^= v >> 16; return v; }
static int W(void) { return tri_screen_width(); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int iabs(int v) { return v < 0 ? -v : v; }
static int level_row(int vx) { if (vx < 0) vx = 0; if (vx >= LEVEL_CELLS + 64) vx = LEVEL_CELLS + 63; return lh[vx]; }
// ground surface (screen y, pixels) under screen x
static int ground_y(int sx) { int wx = TOI(scroll) + sx; if (wx < 0) wx = 0; int a = level_row(wx >> 4), b = level_row((wx >> 4) + 1); return (a < b ? a : b) * 8 + GROUND_SURFACE; }

static void spr(int x, int y, int id, unsigned flags) {
	const spr_t *s = &SPR[id];
	tri_sprite_draw(x, y, s->sheet, s->x, s->y, s->w, s->h, s->pal, flags);
}
static void spr_rot(int cx, int cy, int id, unsigned flags, int angle, int scale) {
	const spr_t *s = &SPR[id];
	tri_sprite_draw_rotated(cx, cy, s->sheet, s->x, s->y, s->w, s->h, s->pal, flags, angle & 4095, scale);
}
static bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
	return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static fx_t *fx_add(int kind, int x, int y) {
	for (int i = 0; i < N_FX; i++) if (!fx[i].kind) {
		fx[i] = (fx_t){ .kind = (uint8_t)kind, .x = FX(x), .y = FX(y), .scale = 256 };
		return &fx[i];
	}
	return NULL;
}
static void boom(int x, int y, bool big) {
	fx_t *f = fx_add(big ? F_BOOM : F_BOOMSMALL, x, y);
	if (f) f->scale = big ? 256 + tri_random_range(0, 64) : 128 + tri_random_range(0, 48);
	sfx_boom(big ? ASSET_BOOM_SAMPLE : ASSET_BOOMSMALL_SAMPLE, big ? 220 : 150, x);
}

static ent_t *ent_add(int type, int x, int y) {
	for (int i = 0; i < N_ENT; i++) if (!ent[i].type) {
		ent[i] = (ent_t){ .type = (uint8_t)type, .x = FX(x), .y = FX(y) };
		return &ent[i];
	}
	return NULL;
}
static const uint8_t ent_spr[] = { 0, SPR_BF109, SPR_BF109G, SPR_HE111, SPR_STUKA, SPR_TANK, SPR_FLAK, SPR_TRUCK, SPR_BUNKER, SPR_DEPOT, SPR_TRAIN };
static const int16_t ent_hp[] = { 0, 3, 4, 26, 5, 12, 4, 2, 22, 8, 260 };
static const int16_t ent_score[] = { 0, 100, 150, 500, 200, 300, 150, 100, 400, 400, 5000 };

static void eshot_add(int kind, int x, int y, int vx, int vy, int life) {
	for (int i = 0; i < N_ESHOT; i++) if (!eshot[i].kind) {
		eshot[i] = (shot_t){ .kind = (uint8_t)kind, .x = FX(x), .y = FX(y), .vx = vx, .vy = vy, .t = (int16_t)life };
		return;
	}
}
static void shot_add(int kind, int x, int y, int vx, int vy) {
	for (int i = 0; i < N_SHOT; i++) if (!shot[i].kind) {
		shot[i] = (shot_t){ .kind = (uint8_t)kind, .x = FX(x), .y = FX(y), .vx = vx, .vy = vy, .t = 0 };
		return;
	}
}
static void say(const char *s) { strncpy(msg, s, sizeof msg - 1); msg_t = 150; }

#ifdef TRI_FPC3D
// ---- FPC: 3D effects (tri_fpc3d.h) ----------------------------------------------------------------------------
// Only what is seen changes: nothing here takes a random number, touches an
// entity or takes part in a collision, so the game plays out as without them.
#include "font8x8_basic.h"

// Shot-down aircraft tumble in 3D as they fall, burning and trailing smoke,
// and blow up where they hit the ground.
#define N_WRECK 8
#define N_WSMOKE 32
typedef struct { uint8_t spr; int32_t x, y, vx, vy; int16_t t, yaw, pitch, roll, dyaw, dpitch, droll; } wreck_t;
static wreck_t wreck[N_WRECK];
static fx_t wsmoke[N_WSMOKE];
static void wreck_add(int id, int cx, int cy, int32_t vx, bool faces_right) {
	if (cy > ground_y(cx) - 12) return;                          // already down
	for (int i = 0; i < N_WRECK; i++) if (!wreck[i].t) {
		uint32_t h = hash(t * 131u + (uint32_t)i * 977u + (uint32_t)cx * 7u + (uint32_t)cy);
		int nose = faces_right ? 1 : -1;                         // the roll that puts its nose down
		wreck[i] = (wreck_t){ .spr = (uint8_t)id, .x = FX(cx), .y = FX(cy), .vx = vx, .vy = -FX(1) / 2 - (int32_t)(h & 63), .t = 1,
			.dyaw = (int16_t)((h >> 8 & 1 ? 1 : -1) * (24 + (int)(h >> 9 & 31))),
			.dpitch = (int16_t)((int)(h >> 14 & 15) - 8),
			.droll = (int16_t)(nose * (14 + (int)(h >> 18 & 15))) };
		return;
	}
}
static void wreck_update(void) {
	for (int i = 0; i < N_WRECK; i++) if (wreck[i].t) {
		wreck_t *w = &wreck[i];
		w->t++;
		w->vy += 14; if (w->vy > FX(4)) w->vy = FX(4);           // gravity
		w->vx += (-SCROLL_SPEED - w->vx) / 48;                   // its way slowed to the ground's
		w->x += w->vx; w->y += w->vy;
		w->yaw = (int16_t)((w->yaw + w->dyaw) & 4095);
		w->pitch = (int16_t)((w->pitch + w->dpitch) & 4095);
		w->roll = (int16_t)((w->roll + w->droll) & 4095);
		int x = TOI(w->x), y = TOI(w->y);
		if (w->t % 3 == 0)
			for (int j = 0; j < N_WSMOKE; j++) if (!wsmoke[j].kind) { wsmoke[j] = (fx_t){ .kind = F_SMOKE, .x = w->x, .y = w->y, .vx = -SCROLL_SPEED / 2, .scale = 70 }; break; }
		int gy = ground_y(x);
		if (y > gy - 6) {
			fx_t *f = fx_add(F_BOOM, x, gy - 10); if (f) f->scale = SPR[w->spr].w > 60 ? 380 : 290;
			f = fx_add(F_BOOMSMALL, x + 12, gy - 4); if (f) { f->scale = 170; f->t = -6; }
			sfx_boom(ASSET_BOOM_SAMPLE, 190, x);
			w->t = 0;
		} else if (x < -90 || x > W() + 90 || y > H + 40) w->t = 0;
	}
	for (int j = 0; j < N_WSMOKE; j++) if (wsmoke[j].kind) {
		fx_t *f = &wsmoke[j];
		f->x += f->vx; f->y -= 20; f->scale += 5;
		if (++f->t > 34) f->kind = 0;
	}
}
static void wreck_draw(void) {
	for (int j = 0; j < N_WSMOKE; j++) if (wsmoke[j].kind)
		spr_rot(TOI(wsmoke[j].x), TOI(wsmoke[j].y), SPR_SMOKE, TRI_SPRITE_BLEND_HALF, (int)(t * 8 + j * 300), wsmoke[j].scale);
	for (int i = 0; i < N_WRECK; i++) if (wreck[i].t) {
		const wreck_t *w = &wreck[i]; const spr_t *s = &SPR[w->spr];
		int x = TOI(w->x), y = TOI(w->y);
		int scale = 256 - (w->t > 120 ? 60 : w->t / 2);         // falling away from us
		tri_sprite_draw_3d(x, y, s->sheet, s->x, s->y, s->w, s->h, s->pal, TRI_SPRITE_BACK_MIRROR, w->yaw, w->pitch, w->roll, scale);
		spr_rot(x, y, SPR_FIREBALL, TRI_SPRITE_BLEND_ADD, (int)(t * 300 + i * 900), 40 + (int)(hash(t * 7u + (uint32_t)i) & 15));
	}
}

// Banners turned in 3D: the mission's start, its end, the game's end. The
// font drawn into a sheet of their own (sheet 5, palette 5, VDP1 bulk memory).
enum { BN_NONE, BN_START, BN_CLEAR, BN_OVER };
#define BANNER_SHEET 5
#define BANNER_PAL 5
#define BANNER_VRAM (TRI_VRAM_PSRAM + 0x6000)
#define BANNER_MAXW 324
#define BANNER_MAXH 48
#define HAZE_SHEET 6                                             // 8 x 8 of palette 5's haze
#define HAZE_VRAM (TRI_VRAM_PSRAM + 0xA000)
static const uint16_t banner_pal[] = { 0, TRI_RGB555(2, 3, 7), TRI_RGB555(4, 6, 12), TRI_RGB555(30, 24, 8), TRI_RGB555(16, 11, 3),
	TRI_RGB555(31, 31, 31), TRI_RGB555(31, 27, 15), TRI_RGB555(1, 1, 3), TRI_RGB555(22, 25, 31), TRI_RGB555(25, 28, 31) };
static int banner, banner_t, banner_w, banner_h;
static uint8_t banner_px[BANNER_MAXW * BANNER_MAXH];
static void banner_text(int x0, int y0, const char *s, int sc, int c_top, int c_bot) {
	for (; *s; s++, x0 += 8 * sc) {
		const char *g = font8x8_basic[(unsigned char)*s & 127];
		for (int y = 0; y < 8 * sc; y++) for (int x = 0; x < 8 * sc; x++) {
			int px = x0 + x, py = y0 + y;
			if (((g[y / sc] >> (x / sc)) & 1) && px >= 0 && px < banner_w && py >= 0 && py < banner_h)
				banner_px[py * banner_w + px] = (uint8_t)(y < 4 * sc ? c_top : c_bot);
		}
	}
}
static void banner_show(int kind, const char *big, const char *small) {
	int nb = (int)strlen(big), ns = small ? (int)strlen(small) : 0;
	banner_w = clampi((nb * 16 > ns * 8 ? nb * 16 : ns * 8) + 36, 8, BANNER_MAXW);
	banner_h = small ? 48 : 36;
	// a gold rim, its corners cut, in a margin of 2 see-through pixels (a
	// perspective row's last texels can fall a little short of the edge)
	for (int y = 0; y < banner_h; y++) for (int x = 0; x < banner_w; x++) {
		int ex = x < banner_w - 1 - x ? x : banner_w - 1 - x, ey = y < banner_h - 1 - y ? y : banner_h - 1 - y;
		int d = (ex < ey ? ex : ey) - 2;
		uint8_t c = d < 0 || ex + ey < 6 ? 0 : d == 0 || d == 2 ? 4 : d == 1 ? 3 : y < banner_h / 2 ? 2 : 1;
		banner_px[y * banner_w + x] = c;
	}
	int bx = (banner_w - nb * 16) / 2;
	banner_text(bx + 2, 12, big, 2, 7, 7);                       // a shadow, then the letters
	banner_text(bx, 10, big, 2, 5, 6);
	if (small) {
		int sx = (banner_w - ns * 8) / 2;
		banner_text(sx + 1, 31, small, 1, 7, 7);
		banner_text(sx, 30, small, 1, 8, 8);
	}
	tri_vram_upload(TRI_VDP1, BANNER_VRAM, banner_px, (uint32_t)(banner_w * banner_h));
	tri_sheet_define(BANNER_SHEET, BANNER_VRAM, banner_w, banner_h);
	banner = kind; banner_t = 0;
}
static void banner_draw(void) {
	int tt = banner_t, yaw = 0, pitch = 0, scale = 256, cy = 64;
	unsigned back = TRI_SPRITE_BACK_PITCH;
	switch (banner) {
	case BN_START:                                               // swings down into place, then tips away
		if (tt >= 150) { banner = BN_NONE; return; }
		if (tt < 40) pitch = 1024 * (40 - tt) * (40 - tt) / 1600 * tri_cos(tt * 160) / 256;
		else if (tt > 128) pitch = -1024 * (tt - 128) * (tt - 128) / 484;
		break;
	case BN_CLEAR: {                                             // spins in from afar, like a medal
		cy = 108; back = TRI_SPRITE_BACK_YAW;
		if (tt < 60) { int q = 256 - tt * 256 / 60, e = 256 - q * q / 256 * q / 256; yaw = 8192 * (256 - e) / 256; scale = 40 + 216 * e / 256; }
		else yaw = 100 * tri_sin(tt * 12) / 256;
		break; }
	case BN_OVER:                                                // falls into place and settles
		cy = 108;
		if (tt < 80) pitch = 1024 * (80 - tt) * (80 - tt) / 6400 * tri_cos(tt * 100) / 256;
		break;
	default:
		return;
	}
	tri_sprite_draw_3d(W() / 2, cy, BANNER_SHEET, 0, 0, banner_w, banner_h, BANNER_PAL, back, yaw, pitch, 0, scale);
}

// The title: a flight over the fields of Normandy (a Mode 7 floor, layer 1:
// a 256 x 256 patchwork made here, tileset 1, map 3, VDP2 palette 4, which
// repeats as the layer wraps), the far hills beyond it, the logo flying in.
#define FIELDS_TILESET 1
#define FIELDS_TILEMAP 3
#define FIELDS_PAL 4
#define FIELDS_VRAM (TRI_VRAM_PSRAM + 0x10000)
#define FIELDS_MAP_VRAM (TRI_VRAM_PSRAM + 0x21000)
#define FIELDS_N 32                                              // tiles across and down
#define HORIZON 158
enum { C_HEDGE = 1, C_HEDGE2, C_TREE, C_PASTURE, C_PASTURE2, C_MEADOW, C_MEADOW2, C_WHEAT, C_WHEAT2, C_PLOUGH, C_PLOUGH2,
	C_CROP, C_CROP2, C_ROAD, C_ROADEDGE, C_RIVER, C_RIVER2, C_BANK, C_ROOF, C_ROOF2, C_SHADOW, C_CRATER, C_RIM, C_FLOWER, C_COLOURS };
static const uint16_t fields_pal[C_COLOURS] = { 0,
	TRI_RGB555(5, 12, 7), TRI_RGB555(7, 15, 8), TRI_RGB555(11, 19, 9), TRI_RGB555(11, 20, 9), TRI_RGB555(12, 22, 10),
	TRI_RGB555(16, 24, 12), TRI_RGB555(18, 25, 13), TRI_RGB555(27, 24, 13), TRI_RGB555(25, 21, 11), TRI_RGB555(17, 12, 8),
	TRI_RGB555(14, 10, 7), TRI_RGB555(8, 17, 8), TRI_RGB555(10, 19, 9), TRI_RGB555(24, 21, 16), TRI_RGB555(18, 16, 12),
	TRI_RGB555(8, 14, 23), TRI_RGB555(12, 19, 27), TRI_RGB555(13, 16, 10), TRI_RGB555(22, 8, 6), TRI_RGB555(15, 6, 5),
	TRI_RGB555(9, 9, 8), TRI_RGB555(9, 8, 7), TRI_RGB555(17, 15, 12), TRI_RGB555(29, 28, 20) };
static const uint8_t field_cols[] = { 0, 44, 96, 138, 188, 224 };     // the fields' columns, each with its rows staggered
static const uint8_t field_rows[] = { 0, 40, 76, 118, 162, 208 };
typedef struct { uint8_t x, y, w, h; } box_t;
static const box_t houses[] = { { 143, 100, 6, 5 }, { 151, 102, 5, 5 }, { 143, 108, 7, 5 }, { 128, 100, 5, 6 }, { 127, 87, 6, 5 },
	{ 145, 86, 5, 5 }, { 153, 89, 6, 4 } };
static const box_t craters[] = { { 205, 40, 5, 0 }, { 30, 185, 4, 0 }, { 172, 226, 6, 0 }, { 108, 30, 4, 0 }, { 238, 140, 4, 0 } };
static int river_x(int y) { return 72 + (tri_sin(y * 16) * 10 >> 8) + (tri_sin(y * 48 + 700) * 3 >> 8); }
static int field_px(int x, int y, int rx) {
	uint32_t hp = hash((uint32_t)(y * 256 + x) ^ 0x9e3779b9u);
	int iabs_r = iabs(x - rx);
	if ((y >= 94 && y < 99) || (x >= 135 && x < 140)) {          // two roads, crossing; a bridge over the river
		bool edge = (y >= 94 && y < 99) ? (y == 94 || y == 98) && !(x > 135 && x < 139) : (x == 135 || x == 139) && !(y > 94 && y < 98);
		return edge ? C_ROADEDGE : C_ROAD;
	}
	if (iabs_r <= 3) return iabs_r == 3 ? C_BANK : (hp & 7) == 0 || (x + y) % 9 == 0 ? C_RIVER2 : C_RIVER;
	for (unsigned i = 0; i < sizeof houses / sizeof houses[0]; i++) {
		const box_t *b = &houses[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return y == b->y + b->h - 1 || x == b->x + b->w - 1 ? C_ROOF2 : C_ROOF;
		if (x >= b->x + 1 && x <= b->x + b->w && y >= b->y + 1 && y <= b->y + b->h) return C_SHADOW;
	}
	for (unsigned i = 0; i < sizeof craters / sizeof craters[0]; i++) {
		int dx = x - craters[i].x, dy = y - craters[i].y, r = craters[i].w;
		if (dx * dx + dy * dy <= r * r) return dx * dx + dy * dy >= (r - 1) * (r - 1) ? C_RIM : C_CRATER;
	}
	int c = 0; while (c + 1 < (int)sizeof field_cols && x >= field_cols[c + 1]) c++;
	int x0 = field_cols[c], x1 = c + 1 < (int)sizeof field_cols ? field_cols[c + 1] : 256;
	int yy = (y + (int)(hash((uint32_t)c * 7u + 1u) % 48u)) & 255;
	int r = 0; while (r + 1 < (int)sizeof field_rows && yy >= field_rows[r + 1]) r++;
	int y0 = field_rows[r], y1 = r + 1 < (int)sizeof field_rows ? field_rows[r + 1] : 256;
	int dx = x - x0 < x1 - 1 - x ? x - x0 : x1 - 1 - x, dy = yy - y0 < y1 - 1 - yy ? yy - y0 : y1 - 1 - yy;
	if (dx < 2 || dy < 2) return (hp & 3) == 0 ? C_HEDGE : (hp & 15) == 1 ? C_TREE : C_HEDGE2;   // hedgerows: the bocage
	if ((dx < 4 || dy < 4) && (hash((uint32_t)((x >> 2) * 64 + (yy >> 2))) & 7) == 0) return (hp & 1) ? C_TREE : C_HEDGE;   // trees by them
	uint32_t f = hash((uint32_t)(c * 31 + r * 17 + 5));
	int along = (f >> 8) & 1 ? x - x0 : yy - y0;                // its furrows' or rows' way
	switch (f % 6) {
	case 0: return (hp & 15) < 5 ? C_PASTURE2 : C_PASTURE;
	case 1: return (along >> 1) & 1 ? C_WHEAT : C_WHEAT2;
	case 2: return along % 4 < 2 ? C_PLOUGH : C_PLOUGH2;
	case 3: return along % 5 < 2 ? C_CROP : C_CROP2;
	case 4: return (hp & 31) == 0 ? C_FLOWER : (hp & 7) < 3 ? C_MEADOW2 : C_MEADOW;
	default: { int ox = (x - x0) % 7, oy = (yy - y0) % 7;     // an orchard
		return ox < 3 && oy < 3 ? (ox == 0 && oy == 0 ? C_TREE : C_HEDGE2) : (hp & 15) < 5 ? C_PASTURE2 : C_PASTURE; }
	}
}
static void make_fields(void) {
	static uint8_t tiles[FIELDS_N * 64];                        // a row of tiles at a time
	static uint16_t map[FIELDS_N * FIELDS_N];
	int rx[8];
	tri_palette_set(TRI_VDP2, FIELDS_PAL, 0, fields_pal, C_COLOURS);
	memset(tiles, 0, 64);
	tri_vram_upload(TRI_VDP2, FIELDS_VRAM, tiles, 64);           // tile 0: empty
	for (int ty = 0; ty < FIELDS_N; ty++) {
		for (int py = 0; py < 8; py++) rx[py] = river_x(ty * 8 + py);
		for (int tx = 0; tx < FIELDS_N; tx++)
			for (int py = 0; py < 8; py++) for (int px = 0; px < 8; px++)
				tiles[tx * 64 + py * 8 + px] = (uint8_t)field_px(tx * 8 + px, ty * 8 + py, rx[py]);
		tri_vram_upload(TRI_VDP2, FIELDS_VRAM + (uint32_t)(1 + ty * FIELDS_N) * 64, tiles, sizeof tiles);
	}
	for (int i = 0; i < FIELDS_N * FIELDS_N; i++) map[i] = TRI_TILE(1 + i, 0, 0, 0);
	tri_tileset_define(FIELDS_TILESET, FIELDS_VRAM, 1 + FIELDS_N * FIELDS_N);
	tri_tilemap_define(FIELDS_TILEMAP, FIELDS_MAP_VRAM, 5, 5);
	tri_vram_upload(TRI_VDP2, FIELDS_MAP_VRAM, map, sizeof map);
	tri_layer_set(1, FIELDS_TILESET, FIELDS_TILEMAP, FIELDS_PAL, TRI_LAYER_BELOW);
	tri_layer_enable(1, false);
	// the banners' palette, and the haze's cell
	uint8_t haze[64];
	memset(haze, 9, sizeof haze);
	tri_palette_set(TRI_VDP1, BANNER_PAL, 0, banner_pal, (int)(sizeof banner_pal / sizeof banner_pal[0]));
	tri_vram_upload(TRI_VDP1, HAZE_VRAM, haze, sizeof haze);
	tri_sheet_define(HAZE_SHEET, HAZE_VRAM, 8, 8);
}
// The camera's flight: forward up the map in gentle S-turns, the far hills
// turning with it, and haze where the fields meet them
static void title_scene(void) {
	int a = 90 * tri_sin((int)t * 5) / 256;
	int32_t camx = FX(128) - 28 * tri_cos((int)t * 5);
	int32_t camy = FX(200) - (int32_t)(t % 4096) * 400;          // (4096 frames: 25 times round the map, seamless)
	tri_layer_mode7(1, HORIZON + 2, H, camx, camy, a & 4095, 34 + (tri_sin((int)t * 3) * 3 >> 8), HORIZON, 180);
	tri_layer_scroll(0, TOI(scroll) / 8 + a / 4, 0);
	for (int k = 0; k < 3; k++)                                  // haze: 3 bands halved over each other, thicker near the horizon
		tri_sprite_draw_matrix(W() / 2, HORIZON - 1 + (24 >> k) / 2, HAZE_SHEET, 0, 0, 8, 8, BANNER_PAL, TRI_SPRITE_BLEND_HALF, 4, 0, 0, 8 * 256 / (24 >> k));
}
static void logo_draw(void) {
	const spr_t *s = &SPR[SPR_LOGO];
	int tt = (int)t, rest = 28 + s->h / 2, cy = rest, yaw, pitch, scale = 256;
	if (tt < 90) {                                               // flies in from afar, spinning, and comes to rest
		int q = 256 - tt * 256 / 90, e = 256 - q * q / 256 * q / 256;
		yaw = 2 * 4096 * (256 - e) / 256;
		pitch = 240 * (256 - e) / 256;
		scale = 24 + 232 * e / 256;
		cy = 120 + (rest - 120) * e / 256;
	} else {                                                     // then sways, and now and then turns once more
		int k = (tt - 90) % 720;
		yaw = 150 * tri_sin(tt * 9) / 256;
		pitch = 70 * tri_sin(tt * 6 + 1024) / 256;
		if (k >= 600 && k < 660) yaw += 4096 * ((256 - tri_cos((k - 600) * 2048 / 60)) / 2) / 256;
	}
	tri_sprite_draw_3d(W() / 2, cy, s->sheet, s->x, s->y, s->w, s->h, s->pal, TRI_SPRITE_BACK_YAW, yaw, pitch, 0, scale);
}
// The armoured train comes round a bend: until it reaches its place it is
// a plane turned away about its front end (the track at eye level, so its
// wheels stay on the rails), straightening as it comes
static void train_draw(const ent_t *e, unsigned fl) {
	const spr_t *s = &SPR[SPR_TRAIN];
	int x = TOI(e->x), y = TOI(e->y), from = W() - s->w - 24 + 12;   // (it sways 12 either side of its place)
	if (x <= from) { spr(x, y, SPR_TRAIN, fl); return; }
	int a = clampi((x - from) * 1000 / (W() + 64 - from), 0, 1000);
	int z = s->w * tri_sin(a) / 256, f = 256 + z;                 // the far end's depth, and its divisor
	int xr = x * 16 + s->w * tri_cos(a) / 16 * 256 / f;          // (1/16 pixel)
	int hr = s->h * 16 * 256 / f, yb = (y + s->h) * 16;
	int qx[4] = { x * 16, xr, xr, x * 16 }, qy[4] = { y * 16, yb - hr, yb, yb };
	tri_sprite_draw_quad(qx, qy, s->sheet, s->x, s->y, s->w, s->h, s->pal, 0);
}
#endif

// ---- level generation ----------------------------------------------------------------------------------------
// Heights are per vertex column (16 px). A segment picks a target row; the ground
// ramps toward it one row per column (the wang tiles draw those as slopes), then
// runs flat. Targets and decor only go on the flat runs, so nothing floats or sinks.
static const uint8_t obj_cells[] = { 0, 0, 0, 0, 0, 3, 2, 2, 4, 4, 11 };   // cells a ground target needs (by E_ type)
static bool flat(int c0, int c1) { for (int i = c0; i < c1; i++) if (lh[i] != lh[c0]) return false; return true; }
static void gen_level(int lv) {
	uint32_t seed = 0x5EED0000u + (uint32_t)lv * 7919u;
	n_dec = n_obj = 0;
	for (int i = 0; i < LEVEL_CELLS + 64; i++) lh[i] = BASE_ROW;
	int cell = 0, cur = BASE_ROW;
	while (cell < LEVEL_CELLS - 48) {
		uint32_t r = hash(seed + (uint32_t)cell);
		int len = 10 + (int)(r % 16);
		int target = BASE_ROW;
		if (cell > 10) { int k = (int)((r >> 8) % 8); target = k < 3 ? BASE_ROW : k < 5 ? BASE_ROW - 2 : k < 7 ? BASE_ROW - 4 : BASE_ROW + 2; }
		int i = cell;
		while (cur != target && i < LEVEL_CELLS) { cur += cur < target ? 1 : -1; lh[i++] = (uint8_t)cur; }
		int flat_start = i, flat_end = cell + len;
		for (; i < flat_end && i < LEVEL_CELLS; i++) lh[i] = (uint8_t)cur;
		if (cell > 12 && flat_end - flat_start >= 5) {
			uint32_t k = hash(r ^ 0xA5A5);
			int p = flat_start + 1;
			if ((k % 100) >= 22 && n_obj < 150) {                     // 78% of runs carry targets
				int pick = (int)((k >> 8) % 100);
				int type = pick < 22 ? E_TRUCK : pick < 40 ? E_TANK : pick < 62 ? E_FLAK : pick < 76 ? E_BUNKER : E_DEPOT;
				int n = type == E_TRUCK ? 2 + (lv > 1) : 1;
				for (int j = 0; j < n && p + obj_cells[type] < flat_end; j++) { lobj[n_obj++] = (lobj_t){ (uint16_t)p, (uint8_t)type }; p += obj_cells[type] + 1; }
				if (type == E_FLAK && (k >> 20) % 2 && p + obj_cells[E_TANK] < flat_end) { lobj[n_obj++] = (lobj_t){ (uint16_t)p, E_TANK }; p += obj_cells[E_TANK] + 1; }
			}
			if ((k >> 12) % 3 != 0 && n_dec < 150) {
				int dk = ((k >> 16) % 4 == 0) ? D_HOUSE : D_TREE;
				int w = dk == D_HOUSE ? (HOUSE_COLS + 1) / 2 : (TREE_COLS + 1) / 2;
				for (int j = 0; j < 2 && p + w < flat_end && n_dec < 150; j++) { ldec[n_dec++] = (lobj_t){ (uint16_t)p, (uint8_t)dk }; p += w + 1 + (int)((k >> (24 + j)) % 2); dk = D_TREE; w = (TREE_COLS + 1) / 2; }
			}
		}
		cell = flat_end;
	}
	// ramp back to the base and stay flat: the train needs level track
	int i = cell;
	while (cur != BASE_ROW && i < LEVEL_CELLS) { cur += cur < BASE_ROW ? 1 : -1; lh[i++] = (uint8_t)cur; }
	for (; i < LEVEL_CELLS + 64; i++) lh[i] = BASE_ROW;
	lobj[n_obj++] = (lobj_t){ LEVEL_CELLS - 24, E_TRAIN };
	(void)flat;
	next_obj = 0;
}

// one world 8-px column of the ground layer: wang tiles from the heightfield, decor in the air cells above
static void build_column(int wc, uint16_t *col) {
	int cx = wc >> 1, half = wc & 1;
	int l = level_row(cx), r = level_row(cx + 1);
	memset(col, 0, MAP_H * sizeof *col);
	int top;                                               // first earth row below the surface tiles
	if (l == r) {                                          // flat: grass tile over earth
		if (l < MAP_H) col[l] = GRASS[0][half];
		if (l + 1 < MAP_H) col[l + 1] = GRASS[1][half];
		top = l + 2;
	} else if (r == l - 1) {                               // rising to the right: the ramp block, rows l-1 .. l+1
		for (int i = 0; i < 3; i++) if (l - 1 + i >= 0 && l - 1 + i < MAP_H) col[l - 1 + i] = RAMP[i][half];
		top = l + 2;
	} else {                                               // descending: the ramp mirrored, rows r-1 .. r+1
		for (int i = 0; i < 3; i++) if (r - 1 + i >= 0 && r - 1 + i < MAP_H) col[r - 1 + i] = RAMP[i][1 - half] | 1 << 12;
		top = r + 2;
	}
	for (int y8 = top; y8 < MAP_H; y8++) col[y8] = EARTH[y8 & 1][half];
	for (int i = 0; i < n_dec; i++) {
		int cs = ldec[i].cell, cols = ldec[i].kind == D_TREE ? TREE_COLS : HOUSE_COLS, rows = ldec[i].kind == D_TREE ? TREE_ROWS : HOUSE_ROWS;
		int c = wc - cs * 2;
		if (c < 0 || c >= cols) continue;
		int dtop = level_row(cs) + 1 - rows;                   // its last row is the grass-top row, pre-composited
		for (int rr = 0; rr < rows; rr++) {
			int y8 = dtop + rr;
			if (y8 < 0 || y8 >= MAP_H) continue;
			uint16_t v = ldec[i].kind == D_TREE ? TREE_MAP[rr][c] : HOUSE_MAP[rr][c];
			if (v && (col[y8] == 0 || rr == rows - 1)) col[y8] = v;
		}
	}
}
// The whole mission's ground lives in CPU PSRAM (92 KB), built once per mission;
// streaming a column into the wrapping VDP2 map is then a copy. Without the chip
// (tri_psram_size() == 0) columns are built on the fly instead.
#define LEVEL_COLS ((LEVEL_CELLS + 64) * 2)
TRI_PSRAM static uint16_t level_map[LEVEL_COLS][MAP_H];
static bool have_psram;
static void build_level_map(void) {
	if (!have_psram) return;
	for (int wc = 0; wc < LEVEL_COLS; wc++) build_column(wc, level_map[wc]);
}
static void stream_column(int wc) {
	uint16_t col[MAP_H];
	if (wc >= LEVEL_COLS) wc = LEVEL_COLS - 1;
	if (have_psram) memcpy(col, level_map[wc], sizeof col); else build_column(wc, col);
	int mx = wc & (MAP_W - 1);
	for (int y8 = 0; y8 < MAP_H; y8++) tri_tilemap_set(ASSET_LEVEL_TILEMAP, mx, y8, col[y8]);
}

// ---- spawning --------------------------------------------------------------------------------------------------
static void spawn_ground(void) {
	int edge_cell = (TOI(scroll) + W() + 64) >> 4;
	while (next_obj < n_obj && lobj[next_obj].cell <= edge_cell) {
		lobj_t o = lobj[next_obj++];
		int sx = o.cell * 16 - TOI(scroll);
		ent_t *e = ent_add(o.kind, sx, 0);
		if (!e) continue;
		e->ground = 1;
		e->hp = ent_hp[o.kind] + (o.kind == E_TRAIN ? level * 40 : level * 2);
		e->vx = o.kind == E_TRUCK ? -FX(1) / 2 : o.kind == E_TANK ? -FX(1) / 5 : 0;
		e->t = tri_random_range(30, 120);
		if (o.kind == E_TRAIN) { boss_spawned = true; boss_hp = boss_hp_max = e->hp; say("ARMOURED TRAIN AHEAD"); }
	}
}
static void spawn_air(int type, int y, int dx) {
	ent_t *e = ent_add(type, W() + 40 + dx, y);
	if (!e) return;
	e->hp = ent_hp[type] + level - 1;
	e->t = tri_random_range(20, 60);
	e->base_y = (int16_t)y;
	switch (type) {
	case E_BF109: e->vx = -FX(2) - level * 20; break;
	case E_BF109G: e->vx = -FX(3); break;
	case E_HE111: e->vx = -FX(1) - 40; break;
	case E_STUKA: e->vx = -FX(1) - 100; break;
	}
}
static void spawn_wave(void) {
	int r = tri_random_range(0, 99);
	int y = tri_random_range(24, 150);
	if (r < 38) { int n = 2 + (level > 1) + tri_random_range(0, 1); for (int i = 0; i < n; i++) spawn_air(E_BF109, y + i * 14, i * 40); }
	else if (r < 55) { spawn_air(E_BF109G, y, 0); spawn_air(E_BF109G, y + 22, 50); }
	else if (r < 75) { spawn_air(E_STUKA, tri_random_range(10, 40), 0); if (level > 1) spawn_air(E_STUKA, tri_random_range(10, 40), 60); }
	else if (r < 92) { spawn_air(E_HE111, tri_random_range(30, 110), 0); if (level > 2) spawn_air(E_BF109, y, 90); }
}

// ---- damage ----------------------------------------------------------------------------------------------------
static void give_ammo(int r, int b, int c) {
	rockets = clampi(rockets + r, 0, 99); bombs = clampi(bombs + b, 0, 99); cannon = clampi(cannon + c, 0, 999);
	sfx(V_HIT, ASSET_PICKUP_SAMPLE, 160, TOI(P.x));
	say("AMMO RESUPPLIED");
}
static void ent_kill(ent_t *e) {
	int x = TOI(e->x), y = TOI(e->y), w = SPR[ent_spr[e->type]].w, h = SPR[ent_spr[e->type]].h;
	score += ent_score[e->type] * level;
	switch (e->type) {
	case E_DEPOT:
		for (int i = 0; i < 4; i++) { fx_t *f = fx_add(F_BOOM, x + tri_random_range(0, w), y + tri_random_range(0, h)); if (f) { f->t = (int16_t)(-i * 8); f->scale = 300; } }
		sfx_boom(ASSET_BOOM_SAMPLE, 255, x);
		give_ammo(6, 3, 60);
		break;
	case E_TRAIN:
		for (int i = 0; i < 12; i++) { fx_t *f = fx_add(F_BOOM, x + tri_random_range(0, w), y + tri_random_range(-10, h)); if (f) { f->t = (int16_t)(-i * 10); f->scale = 320; } }
		sfx_boom(ASSET_BOOM_SAMPLE, 255, x);
		boss_dead = true; gs = GS_CLEAR; gs_t = 0;
		break;
	case E_HE111:
		boom(x + w / 2, y + h / 2, true); boom(x + w / 4, y + h / 3, false);
		break;
	default:
		boom(x + w / 2, y + h / 2, e->type == E_TANK || e->type == E_BUNKER || e->type == E_FLAK);
	}
#ifdef TRI_FPC3D
	if (!e->ground) wreck_add(ent_spr[e->type], x + w / 2, y + h / 2, e->vx / 2, false);
#endif
	e->type = E_NONE;
}
static bool ent_hit(ent_t *e, int dmg, int hx, int hy) {
	e->hp = (int16_t)(e->hp - dmg); e->flash = 4;
	fx_t *f = fx_add(F_FLASH, hx, hy); if (f) f->scale = 60 + dmg * 12;
	if (e->type == E_TRAIN) boss_hp = e->hp;
	if (e->hp <= 0) { ent_kill(e); return true; }
	return false;
}
// area damage (bombs, rockets) on every entity within r of (x, y)
static void blast(int x, int y, int r, int dmg) {
	for (int i = 0; i < N_ENT; i++) if (ent[i].type) {
		ent_t *e = &ent[i]; const spr_t *s = &SPR[ent_spr[e->type]];
		int cx = TOI(e->x) + s->w / 2, cy = TOI(e->y) + s->h / 2;
		int dx = iabs(cx - x), dy = iabs(cy - y);
		if (dx < r + s->w / 2 && dy < r + s->h / 2) ent_hit(e, dmg, cx, cy);
	}
}
static void player_hit(int dmg) {
	if (!P.alive || P.inv > 0) return;
	P.hp -= dmg; P.inv = 40;
	sfx(V_HIT, ASSET_HIT_SAMPLE, 200, TOI(P.x));
	fx_add(F_SMOKE, TOI(P.x) + 10, TOI(P.y) + 8);
	if (P.hp <= 0) {
		P.alive = false; gs = GS_DEAD; gs_t = 0;
		boom(TOI(P.x) + 22, TOI(P.y) + 11, true);
#ifdef TRI_FPC3D
		wreck_add(SPR_P47, TOI(P.x) + SPR[SPR_P47].w / 2, TOI(P.y) + SPR[SPR_P47].h / 2, FX(1) + P.vx / 2, true);
#endif
		tri_sfx_stop(V_ENGINE);
	}
}

// ---- player --------------------------------------------------------------------------------------------------
static void player_reset(void) {
	P = (player_t){ .x = FX(60), .y = FX(100), .hp = 4, .inv = 120, .alive = true, .alt = 0 };
	tri_sfx_play(V_ENGINE, ASSET_ENGINE_SAMPLE, 65536, 70, 70);
}
static void player_update(void) {
	int ax = tri_pad_axis_x(), ay = tri_pad_axis_y();
	int32_t tvx = ax * 6, tvy = -ay * 6;
	P.vx += (tvx - P.vx) / 5; P.vy += (tvy - P.vy) / 5;
	P.x += P.vx; P.y += P.vy;
	int w = SPR[SPR_P47].w, h = SPR[SPR_P47].h;
	P.x = clampi(P.x, FX(0), FX(W() - w));
	P.y = clampi(P.y, FX(4), FX(H - h));
	if (P.inv > 0) P.inv--;
	// terrain
	int gy = ground_y(TOI(P.x) + w / 2);
	if (TOI(P.y) + h - 4 > gy) { P.inv = 0; P.hp = 1; player_hit(1); return; }
	uint32_t b = tri_pad_buttons(), pr = tri_pad_pressed();
	if (P.gun_t) P.gun_t--;
	if (P.cannon_t) P.cannon_t--;
	if (P.rocket_t) P.rocket_t--;
	if (P.bomb_t) P.bomb_t--;
	int nx = TOI(P.x) + w, ny = TOI(P.y) + h / 2;
	if ((b & TRI_PAD_A) && !P.gun_t) {
		P.gun_t = 5; P.alt ^= 1;
		shot_add(S_GUN, nx - 6, ny - 3 + P.alt * 6, FX(7) + P.vx, P.vy / 4 + tri_random_range(-20, 20));
		sfx(V_GUN0 + (gun_v++ % 3), ASSET_GUN_SAMPLE, 110, nx);
		fx_t *f = fx_add(F_MUZZLE, nx - 4, ny - 2 + P.alt * 6); if (f) f->scale = 70;
	}
	if ((b & TRI_PAD_B) && !P.cannon_t && cannon <= 0 && (tri_pad_pressed() & TRI_PAD_B)) say("NO CANNON ROUNDS");
	if ((b & TRI_PAD_B) && !P.cannon_t && cannon > 0) {
		P.cannon_t = 12; cannon--;
		shot_add(S_CANNON, nx - 8, ny + 2, FX(5) + P.vx, P.vy / 6);
		sfx(V_GUN0 + (gun_v++ % 3), ASSET_CANNON_SAMPLE, 170, nx);
		fx_t *f = fx_add(F_MUZZLE, nx - 6, ny + 2); if (f) f->scale = 110;
	}
	if ((pr & TRI_PAD_C) && !P.rocket_t) {
		if (rockets > 0) {
			rockets--; P.rocket_t = 18;
			shot_add(S_ROCKET, TOI(P.x) + 10, TOI(P.y) + h - 4, FX(2) + P.vx, FX(0));
			sfx(V_ROCKET0 + (rocket_v++ & 1), ASSET_ROCKET_SAMPLE, 200, nx);
		} else say("NO ROCKETS");
	}
	if ((pr & (TRI_PAD_STICK | TRI_PAD_D)) && !P.bomb_t) {
		if (bombs > 0) {
			bombs--; P.bomb_t = 25;
			shot_add(S_BOMB, TOI(P.x) + 14, TOI(P.y) + h - 2, FX(1) + P.vx, FX(1) / 2);
			sfx(V_ROCKET0 + (rocket_v++ & 1), ASSET_WHISTLE_SAMPLE, 120, nx);
		} else say("NO BOMBS");
	}
}

// ---- shots -----------------------------------------------------------------------------------------------------
static void shots_update(void) {
	int pw = SPR[SPR_P47].w, ph = SPR[SPR_P47].h;
	for (int i = 0; i < N_SHOT; i++) if (shot[i].kind) {
		shot_t *s = &shot[i];
		s->t++;
		if (s->kind == S_ROCKET) { if (s->vx < FX(9)) s->vx += 40; if (s->t % 3 == 0) { fx_t *f = fx_add(F_SMOKE, TOI(s->x) - 2, TOI(s->y) - 6); if (f) f->scale = 90; } }
		if (s->kind == S_BOMB) s->vy += 30;
		s->x += s->vx; s->y += s->vy;
		int x = TOI(s->x), y = TOI(s->y);
		int sw = s->kind == S_GUN ? 6 : s->kind == S_CANNON ? 8 : s->kind == S_ROCKET ? 24 : 14;
		int sh = s->kind == S_BOMB ? 10 : s->kind == S_ROCKET ? 8 : 3;
		int dmg = s->kind == S_GUN ? 1 : s->kind == S_CANNON ? 3 : s->kind == S_ROCKET ? 10 : 25;
		if (x > W() + 8 || x < -60 || y < -20 || y > H + 20) { s->kind = 0; continue; }
		int gy = ground_y(x + sw / 2);
		if (y + sh > gy + 1) {
			if (s->kind == S_BOMB) { boom(x + sw / 2, gy - 6, true); blast(x + sw / 2, gy, 30, dmg); }
			else if (s->kind == S_ROCKET) { boom(x + sw, gy - 4, false); blast(x + sw, gy, 18, dmg); }
			else { fx_t *f = fx_add(F_FLASH, x + sw, y); if (f) f->scale = 50; }
			s->kind = 0; continue;
		}
		for (int j = 0; j < N_ENT && s->kind; j++) if (ent[j].type) {
			ent_t *e = &ent[j]; const spr_t *sp = &SPR[ent_spr[e->type]];
			int ex = TOI(e->x) + 2, ey = TOI(e->y) + 2, ew = sp->w - 4, eh = sp->h - 4;
			if (overlap(x, y, sw, sh, ex, ey, ew, eh)) {
				if (s->kind == S_BOMB) { boom(x + sw / 2, y + sh / 2, true); blast(x + sw / 2, y + sh / 2, 30, dmg); }
				else if (s->kind == S_ROCKET) { boom(x + sw, y + sh / 2, false); blast(x + sw, y + sh / 2, 18, dmg); }
				else ent_hit(e, dmg, x + sw, y + sh / 2);
				s->kind = 0;
			}
		}
	}
	for (int i = 0; i < N_ESHOT; i++) if (eshot[i].kind) {
		shot_t *s = &eshot[i];
		if (s->kind == K_SHELL) s->vy += 12;
		s->x += s->vx; s->y += s->vy;
		int x = TOI(s->x), y = TOI(s->y);
		if (s->kind == K_FLAK) {
			if (--s->t <= 0) {
				fx_add(F_FLAKBURST, x - 10, y - 10);
				sfx(V_FLAK, ASSET_FLAK_SAMPLE, 140, x);
				if (P.alive && iabs(TOI(P.x) + pw / 2 - x) < 26 && iabs(TOI(P.y) + ph / 2 - y) < 22) player_hit(1);
				s->kind = 0; continue;
			}
		} else if (--s->t <= 0 || y > ground_y(x) - 1) {
			if (s->kind == K_SHELL) { fx_t *f = fx_add(F_FLASH, x, y - 4); if (f) f->scale = 90; }
			s->kind = 0; continue;
		}
		if (x < -8 || x > W() + 8 || y < -8 || y > H + 8) { s->kind = 0; continue; }
		if (P.alive && overlap(x - 1, y - 1, 5, 4, TOI(P.x) + 6, TOI(P.y) + 4, pw - 12, ph - 8)) { player_hit(1); s->kind = 0; }
	}
}

// ---- enemies ---------------------------------------------------------------------------------------------------
static void aim(int x, int y, int speed, int32_t *vx, int32_t *vy) {   // rough aim at the player, 8.8 velocity
	int dx = TOI(P.x) + 20 - x, dy = TOI(P.y) + 10 - y;
	int m = iabs(dx) + iabs(dy); if (m < 1) m = 1;
	*vx = (int32_t)dx * speed / m; *vy = (int32_t)dy * speed / m;
}
static void ents_update(void) {
	ent_air = 0;
	int pw = SPR[SPR_P47].w, ph = SPR[SPR_P47].h;
	for (int i = 0; i < N_ENT; i++) if (ent[i].type) {
		ent_t *e = &ent[i]; const spr_t *sp = &SPR[ent_spr[e->type]];
		if (e->flash) e->flash--;
		e->t--;
		int x = TOI(e->x), y = TOI(e->y);
		int px = TOI(P.x), py = TOI(P.y);
		if (e->ground) {
			e->x += e->vx - SCROLL_SPEED;
			if (e->type == E_TRAIN) {
				int hold = W() - sp->w - 24;
				if (x <= hold) { e->x = FX(hold) + ((int32_t)tri_sin((int)(t * 6) & 4095) * 12); }
			}
			// settle on the ground under the front and the back of the sprite
			int g1 = ground_y(x + 4), g2 = ground_y(x + sp->w - 4);
			int target = (g1 < g2 ? g1 : g2) - sp->h;
			if (e->base_y == 0 || e->type == E_FLAK || e->type == E_BUNKER || e->type == E_DEPOT) { e->y = FX(target); e->base_y = 1; }
			else { int cy = TOI(e->y); if (cy < target) e->y += FX(1); else if (cy > target) e->y -= FX(1); }
			x = TOI(e->x); y = TOI(e->y);
			if (x + sp->w < -40) { e->type = E_NONE; continue; }
			if (!P.alive) continue;
			switch (e->type) {
			case E_TANK:
				if (e->t <= 0 && px < x && x < W() - 8) {
					e->t = (int16_t)(150 - level * 10);
					int32_t vx, vy; aim(x + 6, y + 6, FX(4), &vx, &vy);
					eshot_add(K_SHELL, x + 4, y + 6, vx, vy - FX(1), 90);
					sfx(V_FLAK, ASSET_CANNON_SAMPLE, 130, x);
					fx_t *f = fx_add(F_MUZZLE, x - 4, y + 2); if (f) f->scale = 120;
				}
				break;
			case E_FLAK:
				if (e->t <= 0 && x < W() - 8 && x > 8) {
					e->t = (int16_t)(100 - level * 8);
					int32_t vx, vy; int gx = x + sp->w / 2, gy = y + 4;
					aim(gx, gy, FX(3), &vx, &vy);
					int dist = iabs(px - gx) + iabs(py - gy);
					eshot_add(K_FLAK, gx, gy, vx, vy, dist / 3 + tri_random_range(-6, 6));
					sfx(V_FLAK, ASSET_CANNON_SAMPLE, 110, x);
					fx_t *f = fx_add(F_MUZZLE, gx + 8, gy - 12); if (f) f->scale = 100;
				}
				break;
			case E_BUNKER:
				if (e->t <= 0 && px < x + 40 && x < W() - 8) {
					e->t = (int16_t)(e->aux ? 6 : 120 - level * 8);
					e->aux = (int16_t)((e->aux + 1) % 6);
					int32_t vx, vy; aim(x + 10, y + 12, FX(6), &vx, &vy);
					eshot_add(K_BULLET, x + 10, y + 12, vx, vy, 80);
					if (e->aux == 1) sfx(V_GUN0 + (gun_v++ % 3), ASSET_GUN_SAMPLE, 90, x);
				}
				break;
			case E_TRAIN:
				if (e->t <= 0) {
					e->t = (int16_t)(45 - level * 3);
					int32_t vx, vy; int gx = x + 24, gy = y - 4;
					aim(gx, gy, FX(3), &vx, &vy);
					int dist = iabs(px - gx) + iabs(py - gy);
					eshot_add(K_FLAK, gx, gy, vx, vy, dist / 3 + tri_random_range(-8, 8));
					sfx(V_FLAK, ASSET_CANNON_SAMPLE, 140, x);
					fx_t *f = fx_add(F_MUZZLE, gx, gy - 8); if (f) f->scale = 120;
				}
				if (++e->aux >= 900) e->aux = 0;
				if (e->aux % 300 == 150 && ent_air < 4) spawn_air(E_BF109, tri_random_range(30, 120), 0);
				if (e->aux % 90 == 0) { fx_t *f = fx_add(F_SMOKE, x + sp->w - 30, y - 6); if (f) f->scale = 160; }
				break;
			}
			continue;
		}
		// aircraft
		ent_air++;
		switch (e->type) {
		case E_BF109:
			e->y = FX(e->base_y) + (int32_t)tri_sin((int)(t * 16 + i * 500) & 4095) * 18;
			e->x += e->vx;
			if (e->t <= 0 && px < x - 30 && iabs(py - y) < 24 && x < W()) {
				e->t = (int16_t)(70 - level * 5);
				eshot_add(K_BULLET, x - 2, y + 10, -FX(6) + e->vx, 0, 70);
				sfx(V_GUN0 + (gun_v++ % 3), ASSET_GUN_SAMPLE, 80, x);
			}
			break;
		case E_BF109G:
			// swoops at the player's altitude, then breaks away upward
			if (e->state == 0) { if (y < py) e->y += FX(1); else e->y -= FX(1); if (x < px + 120) e->state = 1; }
			else e->y -= FX(2);
			e->x += e->vx;
			if (e->t <= 0 && px < x - 30 && iabs(py - y) < 20) {
				e->t = (int16_t)(40 - level * 4);
				eshot_add(K_BULLET, x - 2, y + 8, -FX(7), 0, 70);
				eshot_add(K_BULLET, x - 2, y + 12, -FX(7), 0, 70);
				sfx(V_GUN0 + (gun_v++ % 3), ASSET_CANNON_SAMPLE, 90, x);
			}
			break;
		case E_HE111:
			e->x += e->vx;
			e->y = FX(e->base_y) + (int32_t)tri_sin((int)(t * 4 + i * 700) & 4095) * 6;
			if (e->t <= 0 && x < W() - 20 && x + sp->w > 0) {
				e->t = (int16_t)(80 - level * 6);
				int32_t vx, vy; aim(x + sp->w / 2, y + 6, FX(4), &vx, &vy);
				eshot_add(K_BULLET, x + sp->w / 2, y + 6, vx, vy, 100);
				sfx(V_GUN0 + (gun_v++ % 3), ASSET_GUN_SAMPLE, 80, x);
			}
			if (e->hp < ent_hp[E_HE111] / 3 && t % 6 == 0) { fx_t *f = fx_add(F_SMOKE, x + 30, y + 8); if (f) f->scale = 120; }
			break;
		case E_STUKA:
			if (e->state == 0) { e->x += e->vx; if (x < px + 160 && x > px + 60) { e->state = 1; e->vy = FX(3); e->vx = -FX(2); } }
			else if (e->state == 1) { e->x += e->vx; e->y += e->vy; if (y > py - 10 || y > 150) { e->state = 2; e->vy = -FX(2); }
				if (e->t <= 0) { e->t = 12; eshot_add(K_BULLET, x + 4, y + 16, e->vx - FX(2), e->vy + FX(2), 60); } }
			else { e->x += e->vx; e->y += e->vy; if (e->vy > -FX(3)) e->vy -= 20; }
			break;
		}
		x = TOI(e->x); y = TOI(e->y);
		if (x + sp->w < -40 || y < -60 || y > H + 20) { e->type = E_NONE; continue; }
		if (y + sp->h > ground_y(x + sp->w / 2)) { ent_kill(e); continue; }
		if (P.alive && overlap(x + 6, y + 4, sp->w - 12, sp->h - 8, px + 6, py + 4, pw - 12, ph - 8)) {
			player_hit(2);
			ent_hit(e, 6, x + sp->w / 2, y + sp->h / 2);
		}
	}
}

// ---- effects ---------------------------------------------------------------------------------------------------
static void fx_update(void) {
	for (int i = 0; i < N_FX; i++) if (fx[i].kind) {
		fx_t *f = &fx[i];
		f->t++;
		if (f->t < 0) continue;
		f->x += f->vx; f->y += f->vy;
		switch (f->kind) {
		case F_BOOM: case F_BOOMSMALL: f->frame = (uint8_t)(f->t / 4); if (f->frame >= 9) f->kind = 0; break;
		case F_SMOKE: f->y -= 24; f->scale += 4; if (f->t > 30) f->kind = 0; break;
		case F_FLASH: if (f->t > 5) f->kind = 0; break;
		case F_MUZZLE: if (f->t > 3) f->kind = 0; break;
		case F_FLAKBURST: f->scale += 6; if (f->t > 14) f->kind = 0; break;
		}
	}
}

// ---- game flow -------------------------------------------------------------------------------------------------
static void start_mission(int lv) {
	level = lv;
	gen_level(lv);
	build_level_map();
	memset(ent, 0, sizeof ent); memset(shot, 0, sizeof shot); memset(eshot, 0, sizeof eshot); memset(fx, 0, sizeof fx);
	scroll = 0; next_col = 0; boss_spawned = boss_dead = false; boss_hp = 0;
	spawn_t = 240; ent_air = 0;
	rockets = 16; bombs = 8; cannon = 150;
	for (int c = 0; c < MAP_W; c++) stream_column(c);
	next_col = MAP_W;
	player_reset();
	gs = GS_PLAY; gs_t = 0;
	static char m[32]; snprintf(m, sizeof m, "MISSION %d", lv);
#ifdef TRI_FPC3D
	memset(wreck, 0, sizeof wreck); memset(wsmoke, 0, sizeof wsmoke);
	banner_show(BN_START, m, "DESTROY THE ARMOURED TRAIN");
#else
	say(m);
#endif
	tri_music_play(&song);
}

static void load(void) {
	tri_assets_load(game_assets, ASSET_COUNT);
	tri_backdrop_set(TRI_RGB555(17, 24, 31));
	tri_layer_set(0, ASSET_TILES_TILESET, ASSET_BG_TILEMAP, ASSET_TILES_PAL, TRI_LAYER_BELOW);      // far hills and clouds
	tri_layer_enable(1, false);                                                                    // (treeline strip, off for now)
	tri_layer_set(2, ASSET_TILES_TILESET, ASSET_LEVEL_TILEMAP, ASSET_TILES_PAL, TRI_LAYER_BELOW);   // the ground
	tri_text_init(3, TRI_LAYER_ABOVE);
#ifdef TRI_FPC3D
	make_fields();                                                                                 // layer 1: the title's fields
#endif
	tri_fm_patch_set(0, &pat_bass);
	tri_fm_patch_set(1, &pat_snare);
	tri_fm_patch_set(2, &pat_lead);
	tri_audio_master_volume(200, 200);
	// after a reconnect the streamed ground is gone: rebuild the visible window
	if (next_col) {
		int first = (TOI(scroll) >> 3) - 1; if (first < 0) first = 0;
		for (int c = first; c < first + MAP_W; c++) stream_column(c);
		if (P.alive && gs != GS_TITLE) tri_sfx_play(V_ENGINE, ASSET_ENGINE_SAMPLE, 65536, 70, 70);
	}
}

static void init(void) {
	score = 0; lives = 3; level = 1; t = 0;
	song_sort();
	gs = GS_TITLE; gs_t = 0; msg_t = 0;
#ifdef TRI_FPC3D
	banner = BN_NONE; memset(wreck, 0, sizeof wreck); memset(wsmoke, 0, sizeof wsmoke);
#endif
	gen_level(1);
	scroll = 0;
	for (int c = 0; c < MAP_W; c++) stream_column(c);
	next_col = MAP_W;
	tri_text_clear();
}

static void update(void) {
	t++;
	if (msg_t > 0) msg_t--;
#ifdef TRI_FPC3D
	banner_t++;
#endif
	if (gs == GS_TITLE) {
		scroll += SCROLL_SPEED / 2;
		if (TOI(scroll) > 3000) { scroll = 0; for (int c = 0; c < MAP_W; c++) stream_column(c); next_col = MAP_W; }   // attract loop over the first stretch
		while (next_col * 8 < TOI(scroll) + W() + 24) stream_column(next_col++);
		if (tri_pad_pressed() & (TRI_PAD_A | TRI_PAD_B | TRI_PAD_C)) { score = 0; lives = 3; tri_text_clear(); start_mission(1); }
		return;
	}
	if (gs == GS_OVER) {
		if (++gs_t > 90 && (tri_pad_pressed() & (TRI_PAD_A | TRI_PAD_B))) { init(); }
		fx_update();
#ifdef TRI_FPC3D
		wreck_update();
#endif
		return;
	}
	// the world keeps moving while the player is dead, on the clear screen and in play
	scroll += SCROLL_SPEED;
	while (next_col * 8 < TOI(scroll) + W() + 24) stream_column(next_col++);
	spawn_ground();
	if (gs == GS_PLAY && !boss_spawned) {
		if (--spawn_t <= 0 && ent_air < 7) { spawn_wave(); spawn_t = 200 - level * 20 + tri_random_range(0, 60); if (spawn_t < 60) spawn_t = 60; }
	}
	if (gs == GS_PLAY || gs == GS_CLEAR) player_update();
	ents_update();
	shots_update();
	fx_update();
#ifdef TRI_FPC3D
	wreck_update();
#endif
	if (gs == GS_DEAD) {
		if (++gs_t == 100) {
			if (--lives <= 0) { gs = GS_OVER; gs_t = 0; if (score > hi_score) hi_score = score; tri_music_stop();
#ifdef TRI_FPC3D
				banner_show(BN_OVER, "MISSION FAILED", "PRESS A");
#endif
			}
			else { player_reset(); gs = GS_PLAY; }
		}
	} else if (gs == GS_CLEAR) {
		gs_t++;                                         // (FPC: TRI's never counted, so the next mission never came)
		if (P.alive && P.inv < 60) P.inv = 60;          // untouchable while the train burns
#ifdef TRI_FPC3D
		if (gs_t == 61) { static char m[32]; snprintf(m, sizeof m, "MISSION %d COMPLETE", level); banner_show(BN_CLEAR, m, "TARGET DESTROYED"); }
#endif
		if (gs_t == 240) { score += 1000 * level; tri_text_clear(); start_mission(level + 1); }
	}
}

static void draw_hud(void) {
	static int last = -1;
	int cols = tri_text_columns();
	if (gs == GS_TITLE) {
#ifdef TRI_FPC3D
		if (t % 4 == 0 && t >= 90) {                         // (once the logo has landed)
#else
		if (t % 4 == 0) {
#endif
			tri_text_color(TRI_RGB555(31, 31, 31));
#ifndef SPR_LOGO_OK
			tri_text_center(6, "T H U N D E R B O L T");
#endif
			tri_text_center(12, "P-47 GROUND ATTACK, 1944");
			tri_text_center(14, "A  MACHINE GUNS      B  20MM CANNON");
			tri_text_center(15, "C  ROCKETS      STICK  BOMBS");
			if ((t / 30) & 1) tri_text_center(20, "PRESS A TO SCRAMBLE"); else tri_text_center(20, "                   ");
			if (hi_score) tri_text_center(24, "HIGH %06d", hi_score);
		}
#ifdef TRI_FPC3D
		uint32_t shown = t - t % 4;                         // the frame the text layer was last written in
		if (shown >= 90 && (shown / 30) & 1) tri_rect_fill(W() / 2 - 82, 158, 164, 12, TRI_RGB555(2, 3, 7));   // (over the haze)
#endif
		return;
	}
	if (t % 3 == 0 || last != score) {
		last = score;
		tri_text_color(TRI_RGB555(31, 31, 31));
		tri_text_print(1, 0, "SCORE %06d", score);
		tri_text_print(cols / 2 - 6, 0, "P-47 x%d  LV%d", lives, level);
		tri_text_print(cols - 24, 0, "CAN %3d  RKT %2d  BMB %2d", cannon, rockets, bombs);
		if (msg_t > 0) tri_text_center(3, "%s", msg); else if (msg_t == 0) { tri_text_print(0, 3, "%*s", cols, ""); msg_t = -1; }
#ifndef TRI_FPC3D
		if (gs == GS_OVER) { tri_text_center(12, "MISSION FAILED"); tri_text_center(14, "PRESS A"); }
		if (gs == GS_CLEAR && gs_t > 60) { tri_text_center(12, "TARGET DESTROYED"); tri_text_center(14, "MISSION %d COMPLETE", level); }
#endif
	}
	if (boss_spawned && !boss_dead && boss_hp_max > 0) {
		int w = 160 * boss_hp / boss_hp_max; if (w < 0) w = 0;
		tri_rect_fill(W() / 2 - 82, H - 10, 164, 6, TRI_RGB555(4, 4, 4));
		tri_rect_fill(W() / 2 - 80, H - 9, w, 4, TRI_RGB555(31, 6, 4));
	}
	// HUD band behind the top text row, hull pips after the score, and a plate behind any message
	tri_rect_fill(0, 0, W(), 10, TRI_RGB555(2, 3, 7));
	for (int i = 0; i < 4; i++) tri_rect_fill(108 + i * 8, 2, 6, 5, i < P.hp && P.alive ? TRI_RGB555(8, 30, 8) : TRI_RGB555(7, 7, 9));
	if (msg_t > 0) { int n = (int)strlen(msg); tri_rect_fill(W() / 2 - n * 4 - 6, 22, n * 8 + 12, 12, TRI_RGB555(2, 3, 7)); }
#ifndef TRI_FPC3D
	if (gs == GS_OVER || (gs == GS_CLEAR && gs_t > 60)) tri_rect_fill(W() / 2 - 80, 94, 160, 28, TRI_RGB555(2, 3, 7));
#endif
}

static void draw(void) {
	tri_layer_scroll(0, TOI(scroll) / 8, 0);
	tri_layer_scroll(2, TOI(scroll), 0);
#ifdef TRI_FPC3D
	tri_layer_enable(1, gs == GS_TITLE);                         // the title flies over fields, the game over its ground
	tri_layer_enable(2, gs != GS_TITLE);
	if (gs == GS_TITLE) title_scene();
#endif
	// ground targets first, then aircraft, so a low pass over a tank looks right
	for (int pass = 1; pass >= 0; pass--)
		for (int i = 0; i < N_ENT; i++) if (ent[i].type && ent[i].ground == pass) {
			ent_t *e = &ent[i];
			unsigned fl = e->flash & 2 ? TRI_SPRITE_BLEND_ADD : 0;
#ifdef TRI_FPC3D
			if (e->type == E_TRAIN) train_draw(e, fl);
#else
			if (e->type == E_TRAIN) spr(TOI(e->x), TOI(e->y), SPR_TRAIN, fl);
#endif
			else if (e->type == E_STUKA && e->state == 1) spr_rot(TOI(e->x) + SPR[SPR_STUKA].w / 2, TOI(e->y) + 12, SPR_STUKA, fl, 4096 - 500, 256);
			else if (e->type == E_STUKA && e->state == 2) spr_rot(TOI(e->x) + SPR[SPR_STUKA].w / 2, TOI(e->y) + 12, SPR_STUKA, fl, 300, 256);
			else spr(TOI(e->x), TOI(e->y), ent_spr[e->type], fl);
		}
#ifdef TRI_FPC3D
	wreck_draw();
#endif
#ifdef SPR_LOGO_OK
#ifdef TRI_FPC3D
	if (gs == GS_TITLE) logo_draw();
#else
	if (gs == GS_TITLE) spr(W() / 2 - SPR[SPR_LOGO].w / 2, 28, SPR_LOGO, 0);
#endif
#endif
	if (P.alive && !(P.inv & 2 && P.inv < 100)) {
		int w = SPR[SPR_P47].w, h = SPR[SPR_P47].h;
		int angle = TOI(P.vy * 70);
		spr_rot(TOI(P.x) + w / 2, TOI(P.y) + h / 2, SPR_P47, P.inv >= 100 ? TRI_SPRITE_BLEND_HALF : 0, 4096 + angle, 256);
	}
	for (int i = 0; i < N_SHOT; i++) if (shot[i].kind) {
		shot_t *s = &shot[i]; int x = TOI(s->x), y = TOI(s->y);
		switch (s->kind) {
		case S_GUN: tri_rect_fill(x, y, 6, 2, TRI_RGB555(31, 29, 12)); break;
		case S_CANNON: tri_rect_fill(x, y, 8, 3, TRI_RGB555(31, 31, 28)); tri_rect_fill(x, y + 1, 5, 1, TRI_RGB555(31, 20, 6)); break;
		case S_ROCKET: spr_rot(x + 12, y + 4, SPR_ROCKET, 0, 0, 150); break;
		case S_BOMB: { int a = s->vx + s->vy > 0 ? (int)(1024 * s->vy / (s->vx + s->vy)) : 0; spr_rot(x + 7, y + 5, SPR_BOMB, 0, a, 180); break; }
		}
	}
	for (int i = 0; i < N_ESHOT; i++) if (eshot[i].kind) {
		shot_t *s = &eshot[i]; int x = TOI(s->x), y = TOI(s->y);
		switch (s->kind) {
		case K_BULLET: tri_rect_fill(x, y, 4, 2, TRI_RGB555(31, 14, 6)); break;
		case K_SHELL: tri_rect_fill(x, y, 4, 3, TRI_RGB555(30, 26, 16)); break;
		case K_FLAK: tri_rect_fill(x, y, 3, 3, TRI_RGB555(6, 6, 6)); break;
		}
	}
	for (int i = 0; i < N_FX; i++) if (fx[i].kind && fx[i].t >= 0) {
		fx_t *f = &fx[i]; int x = TOI(f->x), y = TOI(f->y);
		switch (f->kind) {
		case F_BOOM: case F_BOOMSMALL: spr_rot(x, y, SPR_BOOM0 + f->frame, f->frame < 6 ? TRI_SPRITE_BLEND_ADD : 0, 0, f->scale); break;
		case F_SMOKE: spr_rot(x, y, SPR_SMOKE, TRI_SPRITE_BLEND_HALF, (int)(t * 8 + i * 300), f->scale); break;
		case F_FLASH: spr_rot(x, y, SPR_FIREBALL, TRI_SPRITE_BLEND_ADD, (int)(t * 300), f->scale); break;
		case F_MUZZLE: spr_rot(x, y, SPR_FIREBALL, TRI_SPRITE_BLEND_ADD, (int)(t * 500), f->scale); break;
		case F_FLAKBURST: spr_rot(x + 10, y + 10, SPR_FLAKBURST, 0, (int)(i * 700), f->scale); break;
		}
	}
	draw_hud();
#ifdef TRI_FPC3D
	banner_draw();
#endif
}

TRI_GAME("Thunderbolt", TRI_VIDEO_SQUARE, load, init, update, draw);
