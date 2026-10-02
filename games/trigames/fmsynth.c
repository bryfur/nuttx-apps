// FM synth playground: four patches, keys on the pad, pitch bend on the stick, a looping song.
// Declares the anamorphic 16:9 mode (360x240, wide pixels) to exercise that path.
#include "tri_sdk.h"

static const struct { const char *name; tri_fm_patch_t p; } patches[4] = {
	{ "bell",  { 0, 0, { { 7, 0, 0, 120, 255, 30, 0, 40 }, { 2, 0, 0, 255, 255, 40, 60, 40 } } } },
	{ "bass",  { 0, 3, { { 1, 0, 0, 160, 255, 90, 80, 90 }, { 1, 0, 0, 255, 255, 60, 200, 60 } } } },
	{ "lead",  { 0, 5, { { 2, 1, 1, 140, 255, 120, 180, 60 }, { 1, 0, 0, 255, 255, 100, 220, 50 } } } },
	{ "organ", { 1, 0, { { 1, 0, 0, 200, 255, 0, 255, 100 }, { 2, 0, 0, 200, 255, 0, 255, 100 } } } },
};

// a two-voice loop: bass on voice 8, arpeggio on voice 9 (frames at 60 Hz, 8 frames per 16th)
#define S 8
static const tri_note_t notes[] = {
	{ 0 * S, 8, 1, 36, 220, 14 }, { 4 * S, 8, 1, 36, 220, 14 }, { 8 * S, 8, 1, 43, 220, 14 }, { 12 * S, 8, 1, 41, 220, 14 },
	{ 0 * S, 9, 0, 60, 160, 6 }, { 1 * S, 9, 0, 64, 160, 6 }, { 2 * S, 9, 0, 67, 160, 6 }, { 3 * S, 9, 0, 72, 160, 6 },
	{ 4 * S, 9, 0, 60, 160, 6 }, { 5 * S, 9, 0, 64, 160, 6 }, { 6 * S, 9, 0, 67, 160, 6 }, { 7 * S, 9, 0, 72, 160, 6 },
	{ 8 * S, 9, 0, 62, 160, 6 }, { 9 * S, 9, 0, 67, 160, 6 }, { 10 * S, 9, 0, 71, 160, 6 }, { 11 * S, 9, 0, 74, 160, 6 },
	{ 12 * S, 9, 0, 60, 160, 6 }, { 13 * S, 9, 0, 65, 160, 6 }, { 14 * S, 9, 0, 69, 160, 6 }, { 15 * S, 9, 0, 72, 160, 6 },
};
static const tri_song_t song = { notes, sizeof notes / sizeof notes[0], 16 * S, true };

static int patch, playing, key_note[2] = { 60, 67 };
static bool key_down[2], song_on;
static uint32_t t;

static void load(void) {
	tri_backdrop_set(TRI_RGB555(4, 2, 6));
	tri_text_init(2, TRI_LAYER_ABOVE);
	for (int i = 0; i < 4; i++) tri_fm_patch_set(i, &patches[i].p);
}

static void init(void) { patch = 0; playing = 0; song_on = false; t = 0; tri_text_clear(); }

static void update(void) {
	uint32_t p = tri_pad_pressed(), r = tri_pad_released();
	if (p & TRI_PAD_C) patch = (patch + 1) & 3;
	if (p & TRI_PAD_D) { song_on = !song_on; if (song_on) tri_music_play(&song); else tri_music_stop(); }
	if (p & TRI_PAD_UP)   { key_note[0] += 12; key_note[1] += 12; }
	if (p & TRI_PAD_DOWN) { key_note[0] -= 12; key_note[1] -= 12; }
	uint32_t keys[2] = { TRI_PAD_A, TRI_PAD_B };
	for (int k = 0; k < 2; k++) {
		if (p & keys[k]) { tri_fm_note_on(k, patch, tri_audio_note_freq(key_note[k]), 200, 200); key_down[k] = true; }
		if (r & keys[k]) { tri_fm_note_off(k); key_down[k] = false; }
		if (key_down[k] && tri_pad_axis_x()) {                       // pitch bend: +- one semitone-ish per full deflection
			uint32_t f = tri_audio_note_freq(key_note[k]);
			f += (uint32_t)((int64_t)f * tri_pad_axis_x() / 2200);
			tri_fm_freq_set(k, f);
		}
	}
	t++;
}

static void draw(void) {
	if (t % 6) return;
	const tri_fm_patch_t *q = &patches[patch].p;
	tri_text_print(1, 2, "FM SYNTH  patch %d: %-6s (C: next)", patch, patches[patch].name);
	tri_text_print(1, 4, "A/B keys  stick X bend  up/dn octave");
	tri_text_print(1, 5, "D tap: song %s  D hold: menu", song_on ? "ON " : "off");
	tri_text_print(1, 8, "alg %d  feedback %d", q->alg, q->feedback);
	for (int o = 0; o < 2; o++) {
		tri_text_print(1, 10 + o * 3, "op%d mult %2d det %2d wave %d lvl %3d", o, q->op[o].mult, q->op[o].detune, q->op[o].wave, q->op[o].level);
		tri_text_print(5, 11 + o * 3, "A %3d  D %3d  S %3d  R %3d", q->op[o].attack, q->op[o].decay, q->op[o].sustain, q->op[o].release);
	}
	tri_text_print(1, 17, "keys: %3d %3d", key_note[0], key_note[1]);
}

TRI_GAME("FM synth", TRI_VIDEO_WIDE, load, init, update, draw);
