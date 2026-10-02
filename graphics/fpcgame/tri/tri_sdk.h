// TRI2350 SDK: the game-facing API.
//
// (FPC: a copy of TRI's firmware/sdk/tri_sdk.h, as of TRI de2286d, for the
// FPC game SDK's TRI runtime, apps/graphics/fpcgame/tri_fpc.c. Changed: a
// game is a NuttX program (TRI_GAME gives it its main(); no ROM header,
// and the descriptor's name is the game's own, so several share an image),
// tri_game_run() returns, "GAME -s SCRIPT" plays fg_script() input, TRI_PSRAM
// is the board's PSRAM .bss.)
//
// A game is four callbacks (tri_game_t) plus this runtime, linked as a
// complete image for one ROM slot. The runtime owns the frame loop paced by
// VDP2's VSYNC, the links to the VDPs, the gamepad (polled on core 1), command
// recording, uploads and telemetry. Draw calls look immediate-mode but record
// into per-VDP command lists (tri_isa.h) shipped at the end of each frame:
// VDP1 draws objects (sprites, rects) into a framebuffer, VDP2 composites tile
// layers around it, outputs DVI and mixes the audio. Assets are ids, never
// addresses (tri_assets.h, tools/mkassets.py).
//
// Naming: every call is tri_<area>_<verb>. Areas: video/screen, frame/time,
// random, pad, vram/palette, sheet/tileset/tilemap/asset, layer/backdrop,
// text, sprite/rect, audio/sfx/fm/music, overlay, rom, os.
//
// Errors: a call with an argument out of range (bad layer, id, VDP, mode)
// does nothing, counts in tri_stats()->api_errors and prints the first few
// occurrences on the console. Nothing here blocks except uploads and anything
// sent from load(), which wait for the link (see docs/sdk.md, Delivery).
#pragma once
#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tri_isa.h"
#include "tri_pad.h"
#include "tri_assets.h"
#include "tri_rom.h"

#define TRI_SDK_VERSION 2

// ============================================================================
// Game descriptor
// ============================================================================
typedef struct {
	const char *name;        // shown by the OS menu (31 characters fit the ROM header)
	uint8_t video_mode;      // TRI_VIDEO_CLASSIC / WIDE / SQUARE the game is designed for, or TRI_VIDEO_ANY
	void (*load)(void);      // upload assets and set up layers; on launch and again whenever the VDPs reconnect
	void (*init)(void);      // reset game state; once per launch, after the first load()
	void (*update)(void);    // logic, once per frame, before draw()
	void (*draw)(void);      // record this frame's sprites, scrolls and text
} tri_game_t;

// Declares the game and stamps its ROM header (found by the OS in the first
// 4 KB of the slot). `mode` is the video mode the game is designed for: the
// OS switches the console to it before chaining into the slot, and the SDK
// applies it again at start so it holds however the game was launched.
// Games that adapt to any canvas width pass TRI_VIDEO_ANY.
#define TRI_GAME(gname, mode, fload, finit, fupdate, fdraw) \
	static const tri_game_t tri_game_desc = { gname, (mode), fload, finit, fupdate, fdraw }; \
	int main(int argc, char *argv[]) { return tri_game_main(&tri_game_desc, argc, argv); }

// The two command destinations, for the calls that take a `vdp` argument.
#define TRI_VDP1 TRI_DEST_VDP1   // objects: sprite sheets, 16 palettes, 48 KB VRAM
#define TRI_VDP2 TRI_DEST_VDP2   // layers, audio: tilesets, tilemaps, samples, 16 palettes, 128 KB VRAM
#define TRI_VDP1_VRAM_SIZE (48 * 1024)    // SRAM sheets (the 424-wide framebuffers take the rest of VDP1's RAM)
#define TRI_VDP2_VRAM_SIZE (128 * 1024)   // SRAM tiles, maps, samples; the top 16 KB are the overlay's
// Each VDP also has 3.5 MB of bulk memory ("PSRAM"), addressed as VRAM
// offsets from TRI_VRAM_PSRAM (tri_isa.h): sheets, tilesets, tilemaps and
// samples can live there (uploads, defines and the asset tool accept either
// region; mark an asset `psram = true` or let the tool spill there when SRAM
// is full). On the FPGA VDP both regions are the same SDRAM, so the split only
// matters for fitting; the RP2350 VDPs read PSRAM through a cache, slower.
#define TRI_VDP_PSRAM_SIZE TRI_VRAM_PSRAM_SIZE

// ============================================================================
// PSRAM on the CPU board
// ----------------------------------------------------------------------------
// 8 MB of ordinary memory for the game, brought up by the runtime before
// load(). Two ways to use it: declare large arrays with TRI_PSRAM (placed in
// PSRAM by the linker, zeroed at start like .bss), or carve from the rest with
// tri_psram_alloc() (never freed; a launch starts over). It is plain memory
// for the cores (they share its cache); only if you hand PSRAM to a DMA call
// tri_psram_flush() first. tri_psram_size() is 0 on a board without the chip,
// in which case TRI_PSRAM arrays must not be touched and alloc returns NULL.
// ============================================================================
#ifdef CONFIG_ARCH_SIM
#define TRI_PSRAM __attribute__((aligned(4)))
#else
#define TRI_PSRAM __attribute__((section(".psram_bss"), aligned(4)))
#endif
size_t tri_psram_size(void);                // bytes present (8 MB), 0 if absent
size_t tri_psram_free(void);                // bytes still available to tri_psram_alloc()
void  *tri_psram_alloc(size_t bytes);       // 16-byte aligned, zeroed; NULL when it does not fit
void   tri_psram_flush(void);               // write the cache back (before a DMA reads PSRAM)

// ============================================================================
// Video mode and screen
// ----------------------------------------------------------------------------
// The canvas is always 240 lines; the width is the video mode's (tri_isa.h):
// CLASSIC 320 (4:3, square pixels), WIDE 360 (16:9, wide pixels), SQUARE 424
// (16:9, square pixels). The console mode is chosen in the OS menu and stored
// in the settings sector; a game's declared mode (TRI_GAME) overrides it while
// the game runs. Switching between WIDE and SQUARE is instant; to or from
// CLASSIC changes the DVI clock, so VDP2 reboots into the new timing, the SDK
// sees a disconnect for about a second and calls load() again when it is back.
// ============================================================================
int  tri_video_mode(void);                  // the mode in effect (TRI_VIDEO_*), never TRI_VIDEO_ANY
void tri_video_mode_set(int mode);          // switch both VDPs now; no-op if already in `mode` or out of range
bool tri_video_mode_save(void);             // store the current mode as the console default (used by the OS menu)
const char *tri_video_mode_name(int mode);  // "4:3 320x240" style label, "any" for TRI_VIDEO_ANY, "?" otherwise
int  tri_video_mode_width(int mode);        // canvas width of a mode in pixels (console mode for TRI_VIDEO_ANY)
int  tri_screen_width(void);                // canvas width in pixels for the current mode (320 / 360 / 424)
int  tri_screen_height(void);               // always 240
int  tri_text_columns(void);                // 8x8 characters across the canvas (40 / 45 / 53)
int  tri_text_rows(void);                   // 8x8 characters down the canvas (30)

// ============================================================================
// Frame, time, telemetry, random numbers
// ============================================================================
uint32_t tri_frame_count(void);             // frames since init() (60 per second when the VDPs are connected)
uint32_t tri_time_us(void);                 // microseconds since launch (wraps after 71 minutes; use differences)
uint32_t tri_time_ms(void);                 // milliseconds since launch
bool     tri_connected(void);               // both VDPs answer; draws are dropped while false (load() reruns when it returns)

typedef struct {
	uint32_t frame;                 // == tri_frame_count()
	uint32_t vsync_period_us;       // measured VSYNC period (16667 in classic, 16683 in the 720-wide modes)
	uint32_t frame_time_us;         // update() + draw() + list send of the last frame
	uint32_t frame_time_max_us;     // worst frame_time_us in the current one-second window
	uint32_t lists_sent, lists_dropped;   // command lists shipped / dropped because the VDPs were behind
	uint32_t tx_packets, tx_bytes;  // link A totals
	uint32_t reconnects;            // times load() ran (1 after a normal launch)
	uint32_t api_errors;            // calls rejected for out-of-range arguments
	bool connected, pad_present;
	uint8_t video_mode;             // TRI_VIDEO_* in effect
	uint32_t psram_bytes;           // PSRAM detected at start (0 = none)
} tri_stats_t;
const tri_stats_t *tri_stats(void);         // live counters; read, never write

uint32_t tri_random(void);                  // xorshift32, never 0
int      tri_random_range(int lo, int hi);  // uniform-ish in [lo, hi] inclusive; returns lo if hi < lo
void     tri_random_seed(uint32_t seed);    // 0 is replaced by a fixed non-zero seed

// ============================================================================
// Gamepad (polled on core 1 at ~250 Hz, latched once per frame before update())
// ----------------------------------------------------------------------------
// Buttons are TRI_PAD_* bits (tri_pad.h): A B C D, the stick click, and
// UP/DOWN/LEFT/RIGHT derived from the stick past a threshold. Holding D for a
// second returns to the OS and holding C toggles the overlay; single taps of
// both are delivered to the game as usual.
// ============================================================================
uint32_t tri_pad_buttons(void);             // held during this frame
uint32_t tri_pad_pressed(void);             // went down this frame (edge)
uint32_t tri_pad_released(void);            // went up this frame (edge)
uint32_t tri_pad_stick(void);               // the stick as a bitfield: TRI_PAD_UP/DOWN/LEFT/RIGHT only
int      tri_pad_axis_x(void);              // stick, -127 (left) .. 127 (right), 0 in the dead zone
int      tri_pad_axis_y(void);              // stick, -127 (down) .. 127 (up)
bool     tri_pad_present(void);             // a pad answered on the last poll

// ============================================================================
// VRAM and palettes (both VDPs)
// ----------------------------------------------------------------------------
// VDP1 VRAM (48 KB) holds 8bpp sprite sheets. VDP2 VRAM (128 KB) holds 8x8
// 8bpp tiles, uint16 tilemaps and 8-bit signed audio samples; the top 16 KB
// (0x1C000..0x20000), tileset 7, tilemap 7, palette 15 and layer 3 belong to
// the system overlay. Palette entries are RGB555 (TRI_RGB555); entry 0 of
// every palette is transparent for sheets and tiles.
// Uploads are chunked into link packets and always delivered (they wait for
// the link rather than being dropped).
// ============================================================================
void tri_vram_upload(int vdp, uint32_t vram_off, const void *data, uint32_t len);   // TRI_VDP1 / TRI_VDP2
void tri_vram_copy(int vdp, uint32_t dst_off, uint32_t src_off, uint32_t len);     // VRAM to VRAM on the VDP, any region to any (PSRAM bank -> SRAM slot)
void tri_palette_set(int vdp, int pal, int first, const uint16_t *rgb555, int n);   // pal 0..15, entries first..first+n-1 (n <= 256)
void tri_palette_entry(int vdp, int pal, int index, uint16_t rgb555);               // one entry

// ============================================================================
// Definitions: sheets (VDP1), tilesets and tilemaps (VDP2), assets
// ============================================================================
void tri_sheet_define(int id, uint32_t vram_off, int w, int h);          // sheet 0..15: w x h 8bpp pixels at vram_off in VDP1
void tri_tileset_define(int id, uint32_t vram_off, int ntiles);          // tileset 0..6: ntiles 8x8 tiles (64 B each) in VDP2
void tri_tilemap_define(int id, uint32_t vram_off, int log2w, int log2h); // tilemap 0..6: 2^log2w x 2^log2h uint16 entries (max 2^10 each)
int  tri_tilemap_width(int id);                                          // tiles across a defined tilemap, 0 if undefined
int  tri_tilemap_height(int id);
// Asset tables from tools/mkassets.py: upload the data and define every record.
void tri_assets_load(const tri_asset_t *table, int count);              // all of them (the usual load() body)
void tri_asset_load(const tri_asset_t *asset);                          // one record, e.g. &game_assets[ASSET_SHIP]

// ============================================================================
// Tile layers and backdrop (VDP2)
// ----------------------------------------------------------------------------
// Four game layers, 0..3. Each shows a tilemap through a tileset with a
// palette base (the tile entry's 2-bit palette is added to it) either below
// or above the object plane. Layer changes take effect on the next VDP2 frame.
// ============================================================================
#define TRI_LAYER_BELOW TRI_LAYER_PRIO_BELOW_OBJ   // objects draw over the layer
#define TRI_LAYER_ABOVE TRI_LAYER_PRIO_ABOVE_OBJ   // the layer draws over objects (HUDs, foregrounds)
void tri_layer_set(int layer, int tileset, int tilemap, int pal_base, int prio);   // configure and enable
void tri_layer_enable(int layer, bool enable);                                     // show/hide, keeping the configuration
void tri_layer_priority(int layer, int prio);                                      // TRI_LAYER_BELOW / TRI_LAYER_ABOVE alone
void tri_layer_scroll(int layer, int x, int y);                                    // top-left of the view in the map, pixels, wraps
void tri_backdrop_set(uint16_t rgb555);                                            // colour where no layer or object covers

// ============================================================================
// Tilemap contents (VDP2, through the map defined with tri_tilemap_define)
// ----------------------------------------------------------------------------
// Entries are TRI_TILE(index, hflip, vflip, pal). Writes are uploads, so they
// are delivered; a run is clipped at the right edge of the map.
// ============================================================================
void tri_tilemap_write(int tilemap, int x, int y, const uint16_t *entries, int n);  // n entries from tile (x, y)
void tri_tilemap_set(int tilemap, int x, int y, uint16_t entry);                    // one cell
void tri_tilemap_fill(int tilemap, int x, int y, int w, int h, uint16_t entry);     // a rectangle of cells

// ============================================================================
// Text: the 8x8 system font on a tile layer of your choice
// ----------------------------------------------------------------------------
// tri_text_init() uploads the font (96 tiles, 6 KB) and a 64x32 map (4 KB) to
// the default text area of VDP2 VRAM (0x18000..0x1C000, tileset 5, tilemap 5,
// palette 14) and enables `layer` for it; use tri_text_init_at() to place it
// elsewhere. Text is palette entry 1 (white until tri_text_color) on a
// transparent background. Coordinates are characters; the map is 64 x 32, of
// which tri_text_columns() x tri_text_rows() are visible.
// ============================================================================
#define TRI_TEXT_FONT_VRAM 0x18000
#define TRI_TEXT_MAP_VRAM  0x1A000
#define TRI_TEXT_TILESET   5
#define TRI_TEXT_TILEMAP   5
#define TRI_TEXT_PALETTE   14
void tri_text_init(int layer, int prio);
void tri_text_init_at(uint32_t font_vram, uint32_t map_vram, int tileset, int tilemap, int pal, int layer, int prio);
void tri_text_color(uint16_t rgb555);                                              // foreground for all text on screen
void tri_text_print(int col, int row, const char *fmt, ...) __attribute__((format(printf, 3, 4)));   // up to 64 characters
void tri_text_center(int row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));         // centred on the canvas
void tri_text_clear(void);                                                         // the whole map

// ============================================================================
// Objects (VDP1): sprites and rectangles
// ----------------------------------------------------------------------------
// A sprite blits the w x h cell at (sx, sy) of a sheet through a VDP1
// palette; index 0 is transparent. Screen coordinates are pixels, the canvas
// origin is top-left, objects may go partly off screen. Objects are drawn in
// call order into the frame recorded by draw(); VDP1 renders that frame during
// the next one, so a sprite is visible two frames after the call.
// ============================================================================
#define TRI_SPRITE_HFLIP      1u
#define TRI_SPRITE_VFLIP      2u
#define TRI_SPRITE_BLEND_HALF 4u   // 50/50 with what is already there
#define TRI_SPRITE_BLEND_ADD  8u   // saturating add (glows, shots)
void tri_sprite_draw(int x, int y, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags);
// Rotate by `angle` (0..4095 = one turn, clockwise) and scale by `scale` (8.8 fixed, 256 = 1.0, min 16)
// around the destination centre (cx, cy).
void tri_sprite_draw_rotated(int cx, int cy, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags, int angle, int scale);
// Arbitrary inverse matrix [a b; c d] in 8.8 fixed point (destination -> source, see TRI_OP_SPRITE_AFFINE).
void tri_sprite_draw_matrix(int cx, int cy, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags, int a, int b, int c, int d);
void tri_rect_fill(int x, int y, int w, int h, uint16_t rgb555);   // solid, opaque
int  tri_sin(int angle);   // 8.8 fixed, angle 0..4095 per turn
int  tri_cos(int angle);

// ============================================================================
// Audio (VDP2 sound engine): 16 voices at 48 kHz into the HDMI stream
// ----------------------------------------------------------------------------
// A voice plays either a sample or a 2-operator FM patch; starting one kind
// on a voice replaces the other. Volumes are 0..255 per side. Pitch is 16.16
// (65536 = the sample's native rate); frequencies are Hz in 16.16.
// ============================================================================
void tri_audio_sample_define(int id, uint32_t vram_off, uint32_t samples, uint32_t loop_start, uint32_t rate_hz);  // id 0..63; loop_start 0xFFFFFFFF = one-shot
void tri_audio_voice_volume(int voice, int vol_l, int vol_r);       // live volume of a playing voice
void tri_audio_master_volume(int vol_l, int vol_r);                 // 255/255 after load()
void tri_audio_stop_all(void);                                      // every voice, and the music
uint32_t tri_audio_note_freq(int midi_note);                        // MIDI note (0..119, 69 = A4 440 Hz) -> Hz 16.16
uint32_t tri_audio_pan(int volume, int pan, bool right);            // helper: side volume for pan -127 (left) .. 127 (right)

void tri_sfx_play(int voice, int sample, uint32_t pitch16, int vol_l, int vol_r);    // voice 0..15, sample 0..63
void tri_sfx_play_pan(int voice, int sample, uint32_t pitch16, int volume, int pan); // volume 0..255, pan -127..127
void tri_sfx_stop(int voice);

typedef struct {
	uint8_t alg;        // 0: op0 modulates op1 (FM); 1: both operators are carriers (additive)
	uint8_t feedback;   // 0..7, op0 self-modulation
	struct {
		uint8_t mult;   // frequency multiplier 0..15 (0 = x0.5)
		int8_t  detune; // -8..7
		uint8_t wave;   // 0 sine, 1 half, 2 abs, 3 pulse
		uint8_t level;  // 0..255, 255 loudest (for op0 in alg 0: modulation depth)
		uint8_t attack, decay, sustain, release;   // rates 0..255 (255 fastest); sustain is a level
	} op[2];
} tri_fm_patch_t;
void tri_fm_patch_set(int id, const tri_fm_patch_t *patch);                       // patch 0..31
void tri_fm_note_on(int voice, int patch, uint32_t freq16, int vol_l, int vol_r);  // (re)trigger the envelope
void tri_fm_note_off(int voice);                                                   // enter release
void tri_fm_freq_set(int voice, uint32_t freq16);                                  // bend a sounding voice

// Music: a song is a list of FM notes with frame times, played by the SDK
// from the frame loop (between update() and draw()). Notes must be in frame order.
typedef struct { uint16_t frame; uint8_t voice, patch, note, vel; uint16_t len; } tri_note_t;   // len frames until note off, 0 = never
typedef struct { const tri_note_t *notes; int count; uint16_t length_frames; bool loop; } tri_song_t;
void tri_music_play(const tri_song_t *song);   // from the start; replaces the current song
void tri_music_stop(void);                     // releases the song's sounding voices
bool tri_music_playing(void);

// ============================================================================
// Overlay, ROM slots and the OS
// ============================================================================
void tri_overlay_show(bool show);           // VDP2's telemetry overlay (layer 3); hold C on the pad toggles it too
bool tri_overlay_visible(void);
void tri_os_exit(void);                     // leave the game: chain back into the OS (what hold-D does)

// ROM slot directory (raw flash reads, so they work from any image).
typedef struct {
	int slot;               // 0..TRI_SLOTS-1
	char name[32];
	uint8_t video_mode;     // TRI_VIDEO_* or TRI_VIDEO_ANY
	uint8_t header_version;
	uint32_t sdk_version;
} tri_rom_entry_t;
int  tri_rom_scan(tri_rom_entry_t *out, int max);   // every slot with a valid header, in slot order; returns the count
bool tri_rom_read(int slot, tri_rom_entry_t *out);  // one slot; false if empty or not a game
// Chain into a slot at the next frame boundary. The console is switched to the
// game's declared video mode first (unless TRI_VIDEO_ANY), so the game boots
// straight into it and its own mode command changes nothing.
void tri_rom_launch(int slot);

// Entry points. Games get theirs from tri_game_main.c (linked by tri_add_game);
// the OS image calls tri_os_run with the menu app. Neither returns.
int  tri_game_run(const tri_game_t *game);    // (FPC: returns when the game ends: 0, or a negated errno)
int  tri_game_main(const tri_game_t *game, int argc, char *argv[]);   // (FPC: TRI_GAME's main(): -s SCRIPT, fg_script())
void tri_os_run(const tri_game_t *menu);
