// SDK internals shared between the runtime's source files. Not for games.
#pragma once
#include "tri_sdk.h"

// ---- command recording (tri_sdk.c) ----------------------------------------------
// Words are appended to the destination VDP's list; the list is shipped at the
// end of the frame, or earlier when it fills a link packet.
void tri_emit(int dest, const uint32_t *words, int n);
void tri_emit_deliver(int dest, const uint32_t *words, int n);   // like tri_emit, but the list must arrive (asset data)
void tri_flush(int dest);
void tri_link_quiesce(void);          // before handing the CPU over: VDPs reset if connected, link idle

// ---- argument checking ------------------------------------------------------------
// Every public call validates its arguments with these; a failed check does
// nothing, counts in tri_stats()->api_errors and prints the first few.
void tri_api_error(const char *fn, const char *what);
static inline bool tri_check(bool ok, const char *fn, const char *what) {
	if (!ok) tri_api_error(fn, what);
	return ok;
}
static inline int tri_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- per-area reset, done at every (re)connect before load() ----------------------
void tri_gfx_reset(void);             // layers off, backdrop black, tilemap directory forgotten
void tri_text_reset(void);            // no text layer until tri_text_init
void tri_audio_reset(void);           // voices silent, master volume up, no song
void tri_music_tick(void);            // sequencer, once per frame

// ---- system core and images (tri_system.c) ----------------------------------------
void tri_system_start(void);                  // core 1: pad polling and the hotkeys
bool tri_system_pad(tri_pad_t *out);          // latest pad state; false if no pad answers
bool tri_system_overlay_request(void);        // true once when hold-C fires
void tri_system_service(bool is_os);          // hold-D and pending launches; may not return (chain)
void tri_system_poll_stats(uint32_t *polls, uint32_t *fails, uint32_t *hold_d, uint32_t *hold_c);
