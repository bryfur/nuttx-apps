// SDK audio: command emitters for the VDP2 sound engine (samples, FM), note
// frequencies, panning, and a frame-timed music sequencer.
#include <string.h>
#include "tri_sdk_internal.h"

static inline uint32_t vol2(int l, int r) { return (uint32_t)tri_clamp(l, 0, 255) | ((uint32_t)tri_clamp(r, 0, 255) << 8); }
static bool voice_ok(int v, const char *fn) { return tri_check(v >= 0 && v < TRI_AUDIO_VOICES, fn, "voice out of range"); }

// ---- samples -----------------------------------------------------------------------------------------
void tri_audio_sample_define(int id, uint32_t off, uint32_t samples, uint32_t loop_start, uint32_t rate_hz) {
	if (!tri_check(id >= 0 && id < TRI_AUDIO_SAMPLES, "tri_audio_sample_define", "sample out of range")) return;
	if (!tri_check(samples > 0 && rate_hz > 0, "tri_audio_sample_define", "empty sample or zero rate")) return;
	uint32_t c[5] = { TRI_HDR(TRI_OP_AUDIO_SAMPLE_DEF, 4, id), off, samples, loop_start, rate_hz };
	tri_emit(TRI_DEST_VDP2, c, 5);
}

void tri_sfx_play(int voice, int sample, uint32_t pitch16, int vol_l, int vol_r) {
	if (!voice_ok(voice, "tri_sfx_play")) return;
	if (!tri_check(sample >= 0 && sample < TRI_AUDIO_SAMPLES, "tri_sfx_play", "sample out of range")) return;
	uint32_t c[3] = { TRI_HDR(TRI_OP_AUDIO_PLAY, 2, voice | (sample << 4)), pitch16, vol2(vol_l, vol_r) };
	tri_emit(TRI_DEST_VDP2, c, 3);
}

uint32_t tri_audio_pan(int volume, int pan, bool right) {
	volume = tri_clamp(volume, 0, 255);
	pan = tri_clamp(pan, -127, 127);
	int side = right ? 127 + pan : 127 - pan;   // 0 (fully away) .. 254 (fully this side), 127 centre = full volume
	if (side > 127) side = 127;
	return (uint32_t)(volume * side / 127);
}

void tri_sfx_play_pan(int voice, int sample, uint32_t pitch16, int volume, int pan) {
	tri_sfx_play(voice, sample, pitch16, (int)tri_audio_pan(volume, pan, false), (int)tri_audio_pan(volume, pan, true));
}

void tri_sfx_stop(int voice) {
	if (!voice_ok(voice, "tri_sfx_stop")) return;
	uint32_t c = TRI_HDR(TRI_OP_AUDIO_STOP, 0, voice);
	tri_emit(TRI_DEST_VDP2, &c, 1);
}

void tri_audio_voice_volume(int voice, int vol_l, int vol_r) {
	if (!voice_ok(voice, "tri_audio_voice_volume")) return;
	uint32_t c[2] = { TRI_HDR(TRI_OP_AUDIO_VOL, 1, voice), vol2(vol_l, vol_r) };
	tri_emit(TRI_DEST_VDP2, c, 2);
}

void tri_audio_master_volume(int vol_l, int vol_r) {
	uint32_t c[2] = { TRI_HDR(TRI_OP_AUDIO_VOL, 1, 0xFFFF), vol2(vol_l, vol_r) };
	tri_emit(TRI_DEST_VDP2, c, 2);
}

// ---- FM ----------------------------------------------------------------------------------------------
void tri_fm_patch_set(int id, const tri_fm_patch_t *p) {
	if (!tri_check(id >= 0 && id < TRI_AUDIO_FM_PATCHES, "tri_fm_patch_set", "patch out of range")) return;
	uint32_t c[6] = {
		TRI_HDR(TRI_OP_AUDIO_FM_PATCH, 5, id),
		(uint32_t)(p->alg & 1) | ((uint32_t)(p->feedback & 7) << 4),
		TRI_FM_OP_W0(p->op[0].mult, p->op[0].detune, p->op[0].wave, p->op[0].level),
		TRI_FM_OP_W1(p->op[0].attack, p->op[0].decay, p->op[0].sustain, p->op[0].release),
		TRI_FM_OP_W0(p->op[1].mult, p->op[1].detune, p->op[1].wave, p->op[1].level),
		TRI_FM_OP_W1(p->op[1].attack, p->op[1].decay, p->op[1].sustain, p->op[1].release),
	};
	tri_emit(TRI_DEST_VDP2, c, 6);
}

void tri_fm_note_on(int voice, int patch, uint32_t freq16, int vol_l, int vol_r) {
	if (!voice_ok(voice, "tri_fm_note_on")) return;
	if (!tri_check(patch >= 0 && patch < TRI_AUDIO_FM_PATCHES, "tri_fm_note_on", "patch out of range")) return;
	uint32_t c[3] = { TRI_HDR(TRI_OP_AUDIO_FM_ON, 2, voice | (patch << 4)), freq16, vol2(vol_l, vol_r) };
	tri_emit(TRI_DEST_VDP2, c, 3);
}

void tri_fm_note_off(int voice) {
	if (!voice_ok(voice, "tri_fm_note_off")) return;
	uint32_t c = TRI_HDR(TRI_OP_AUDIO_FM_OFF, 0, voice);
	tri_emit(TRI_DEST_VDP2, &c, 1);
}

void tri_fm_freq_set(int voice, uint32_t freq16) {
	if (!voice_ok(voice, "tri_fm_freq_set")) return;
	uint32_t c[2] = { TRI_HDR(TRI_OP_AUDIO_FM_FREQ, 1, voice), freq16 };
	tri_emit(TRI_DEST_VDP2, c, 2);
}

// MIDI note -> Hz in 16.16. Octave 4 (notes 60..71) tabulated, others by shifting.
static const uint32_t octave4[12] = {   // 261.63 .. 493.88 Hz * 65536
	17146147, 18165760, 19245984, 20390656, 21603136, 22887808,
	24248832, 25690624, 27218176, 28836608, 30551552, 32368384
};
uint32_t tri_audio_note_freq(int n) {
	n = tri_clamp(n, 0, 119);
	int oct = n / 12, k = n % 12;
	uint32_t f = octave4[k];
	return oct >= 5 ? f << (oct - 5) : f >> (5 - oct);
}

// ---- sequencer ---------------------------------------------------------------------------------------
static const tri_song_t *song;
static int song_pos;                        // next event index
static uint32_t song_frame;
static uint16_t off_at[TRI_AUDIO_VOICES];   // frame to send FM_OFF (0 = none)

static void release_song_voices(void) {
	for (int v = 0; v < TRI_AUDIO_VOICES; v++) if (off_at[v]) { tri_fm_note_off(v); off_at[v] = 0; }
}

void tri_music_play(const tri_song_t *s) {
	if (!tri_check(s && s->notes && s->count > 0, "tri_music_play", "empty song")) return;
	release_song_voices();
	song = s; song_pos = 0; song_frame = 0;
}
void tri_music_stop(void) {
	release_song_voices();
	song = NULL;
}
bool tri_music_playing(void) { return song != NULL; }

void tri_audio_stop_all(void) {
	tri_music_stop();
	for (int v = 0; v < TRI_AUDIO_VOICES; v++) tri_sfx_stop(v);
}

void tri_audio_reset(void) {
	song = NULL;
	memset(off_at, 0, sizeof off_at);
	for (int v = 0; v < TRI_AUDIO_VOICES; v++) tri_sfx_stop(v);
	tri_audio_master_volume(255, 255);
}

void tri_music_tick(void) {
	if (!song) return;
	while (song_pos < song->count && song->notes[song_pos].frame <= song_frame) {
		const tri_note_t *n = &song->notes[song_pos++];
		tri_fm_note_on(n->voice & 15, n->patch, tri_audio_note_freq(n->note), n->vel, n->vel);
		off_at[n->voice & 15] = n->len ? (uint16_t)(song_frame + n->len) : 0;
	}
	for (int v = 0; v < TRI_AUDIO_VOICES; v++)
		if (off_at[v] && off_at[v] <= song_frame) { tri_fm_note_off(v); off_at[v] = 0; }
	song_frame++;
	if (song_frame >= song->length_frames) {
		if (song->loop) { song_frame = 0; song_pos = 0; }
		else if (song_pos >= song->count) {
			bool any = false;
			for (int v = 0; v < TRI_AUDIO_VOICES; v++) any |= off_at[v] != 0;
			if (!any) song = NULL;
		}
	}
}
