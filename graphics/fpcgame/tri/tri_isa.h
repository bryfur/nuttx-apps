// TRI2350 command ISA - the contract between the CPU board and the VDPs.
// Portable C, no SDK dependencies. Every command is a 32-bit header word
//     op:8 | nwords:8 | imm:16        (nwords = payload words that follow)
// followed by nwords payload words. Lists are executed in order.
#pragma once
#include <stdint.h>

#define TRI_ISA_VERSION 3

// Destinations (also the link packet 'dest' field)
#define TRI_DEST_CPU  0
#define TRI_DEST_VDP1 1
#define TRI_DEST_VDP2 2
#define TRI_DEST_MAX  2

// Video modes (TRI_OP_VIDEO_MODE). The canvas is always 240 lines; every mode
// doubles lines. The width is the game's canvas; VDP2 stretches it to the output.
//   0  classic 4:3        320x240 -> 640x480p60 (pixel doubled)
//   1  sharp anamorphic   360x240 -> 720x480p60 16:9 (pixel doubled, wide pixels)
//   2  virtual 16:9       424x240 -> 720x480p60 16:9 (2-2-1 stretch, square pixels)
#define TRI_VIDEO_CLASSIC 0
#define TRI_VIDEO_WIDE    1
#define TRI_VIDEO_SQUARE  2
#define TRI_VIDEO_MODES   3
#define TRI_VIDEO_W(m)    ((m) == TRI_VIDEO_CLASSIC ? 320 : (m) == TRI_VIDEO_WIDE ? 360 : 424)
#define TRI_VIDEO_OUT_W(m) ((m) == TRI_VIDEO_CLASSIC ? 640 : 720)   // DVI active pixels
#define TRI_VIDEO_DEFAULT TRI_VIDEO_SQUARE
#define TRI_RENDER_W 424    // widest canvas: buffers are sized for it
#define TRI_RENDER_H 240

#define TRI_HDR(op, nwords, imm) ((uint32_t)(op) | ((uint32_t)(nwords) << 8) | ((uint32_t)(imm) << 16))
#define TRI_HDR_OP(w)     ((w) & 0xFF)
#define TRI_HDR_NWORDS(w) (((w) >> 8) & 0xFF)
#define TRI_HDR_IMM(w)    ((w) >> 16)
#define TRI_MAX_NWORDS 255

// RGB555 with bit 15 = opaque flag (used by object lines and palette entries)
#define TRI_RGB555(r, g, b) ((uint16_t)(((r) & 31) << 10 | ((g) & 31) << 5 | ((b) & 31)))
#define TRI_OPAQUE 0x8000

// ---- Common ops (0x00-0x1F) -------------------------------------------------
#define TRI_OP_NOP         0x00
#define TRI_OP_FRAME_BEGIN 0x01  // imm = mode (VDP1: 0 = 2D, 1 = 3D). nwords 0
#define TRI_OP_FRAME_END   0x02  // nwords 0
#define TRI_OP_UPLOAD      0x03  // payload: w0 = byte offset in the VDP's VRAM, w1 = byte length, then data
// VRAM addressing (both VDPs): offsets below the VDP's SRAM VRAM size are its
// SRAM (fast: VDP1 48 KB of sheets, VDP2 128 KB of tiles, maps, samples);
// offsets from TRI_VRAM_PSRAM up map onto the bulk memory (3.5 MB per VDP:
// the FPGA VDP's SDRAM window, docs/fpga.md; the RP2350 VDPs had 8 MB of
// PSRAM behind their XIP cache). Uploads, sheets, tilesets, tilemaps and
// samples may live in either, each wholly in one region.
#define TRI_VRAM_PSRAM      0x100000u   // VRAM offset of the first bulk ("PSRAM") byte
#define TRI_VRAM_PSRAM_SIZE 0x380000u
#define TRI_OP_VRAM_COPY   0x06  // payload: w0 = destination VRAM offset, w1 = source VRAM offset, w2 = byte length; any region to any region, on the VDP (overlap allowed)
#define TRI_OP_PALETTE     0x04  // imm = pal(4) | first(8) << 4; payload: nwords words, 2 RGB555 entries each (low half first)
#define TRI_OP_VIDEO_MODE  0x05  // imm = TRI_VIDEO_*; nwords 0. Sent to both VDPs; VDP2 reboots if the DVI timing changes

// ---- VDP1 ops (0x20-0x3F) ---------------------------------------------------
// VDP1 VRAM holds 8bpp sprite sheets. A sheet is a rectangle of pixels; a
// sprite blits a w x h cell from (sx, sy) in a sheet to (x, y) on screen through
// one of 16 palettes (index 0 transparent). Coordinates are int16.
#define TRI_OP_SHEET_DEF   0x20  // imm = sheet id (0-15); payload: w0 = VRAM byte offset, w1 = width | height << 16 (pixels)
#define TRI_OP_SPRITE      0x21  // payload: w0 = x | y << 16, w1 = sx | sy << 16, w2 = w | h << 16, w3 = TRI_SPR_ATTR(...)
#define TRI_OP_RECT        0x22  // imm = RGB555 colour; payload: w0 = x | y << 16, w1 = w | h << 16
// Affine sprite: the cell (sx,sy,w,h) is drawn around the destination centre
// (cx,cy) through the inverse matrix M = [a b; c d] (8.8 fixed): for each
// destination pixel p, source = cell_centre + M * (p - centre). Rotation by
// angle t and uniform scale s: a = d = cos(t)/s, b = sin(t)/s, c = -sin(t)/s.
#define TRI_OP_SPRITE_AFFINE 0x23  // payload: w0 = cx | cy << 16, w1 = sx | sy << 16, w2 = w | h << 16, w3 = TRI_SPR_ATTR, w4 = a | b << 16, w5 = c | d << 16 (int16 8.8)
#define TRI_SPR_ATTR(sheet, pal, hflip, vflip, blend) \
	((uint32_t)((sheet) & 15) | ((uint32_t)((pal) & 15) << 4) | ((uint32_t)((hflip) & 1) << 8) | \
	 ((uint32_t)((vflip) & 1) << 9) | ((uint32_t)((blend) & 3) << 10))
#define TRI_BLEND_NONE 0
#define TRI_BLEND_HALF 1         // 50/50 with what is already in the framebuffer
#define TRI_BLEND_ADD  2         // saturating add

// ---- VDP2 ops (0x40-0x5F) ---------------------------------------------------
#define TRI_OP_TILESET_DEF 0x40  // imm = tileset id (0-7); payload: w0 = VRAM byte offset (4-aligned), w1 = tile count (8x8, 8bpp, 64 B/tile)
#define TRI_OP_TILEMAP_DEF 0x41  // imm = tilemap id (0-7); payload: w0 = VRAM byte offset, w1 = log2(width tiles) | log2(height tiles) << 8
#define TRI_OP_LAYER       0x42  // imm = layer (0-4); payload: w0 = TRI_LAYER_CFG(...)
#define TRI_OP_SCROLL      0x43  // imm = layer;      payload: w0 = (x & 0xFFFF) | (y << 16), both int16
#define TRI_OP_BACKDROP    0x44  // imm = RGB555 backdrop colour. nwords 0
#define TRI_OP_OVERLAY     0x45  // imm = 0 hide / 1 show the system telemetry overlay (layer 4). nwords 0

#define TRI_LAYER_CFG(enable, tileset, tilemap, pal_base, prio) \
	((uint32_t)((enable) & 1) | ((uint32_t)((tileset) & 7) << 1) | ((uint32_t)((tilemap) & 7) << 4) | \
	 ((uint32_t)((pal_base) & 15) << 8) | ((uint32_t)((prio) & 1) << 12))
#define TRI_LAYER_PRIO_BELOW_OBJ 0
#define TRI_LAYER_PRIO_ABOVE_OBJ 1
#define TRI_LAYER_OVERLAY 4      // system layer, always on top; 0-3 are the game's

// Tilemap entry (uint16_t): tile:12 | hflip:1 | vflip:1 | pal:2 (added to the layer's pal_base)
#define TRI_TILE(index, hflip, vflip, pal) \
	((uint16_t)(((index) & 0xFFF) | ((hflip) & 1) << 12 | ((vflip) & 1) << 13 | ((pal) & 3) << 14))
#define TRI_TILE_INDEX(e) ((e) & 0xFFF)
#define TRI_TILE_HFLIP(e) (((e) >> 12) & 1)
#define TRI_TILE_VFLIP(e) (((e) >> 13) & 1)
#define TRI_TILE_PAL(e)   (((e) >> 14) & 3)

// ---- Audio ops (0x60-0x6F), executed by VDP2's sound engine -----------------
// 16 voices, each either a PCM sample player or a 2-operator FM voice. Output
// 48 kHz stereo. Volumes are 0..255 per side. Samples are 8-bit signed mono in
// VDP2 VRAM.
#define TRI_OP_AUDIO_SAMPLE_DEF 0x60  // imm = sample id (0-63); payload: w0 = VRAM byte offset, w1 = length (samples),
                                      //   w2 = loop start (samples; 0xFFFFFFFF = one-shot), w3 = native rate (Hz)
#define TRI_OP_AUDIO_PLAY       0x61  // imm = voice | sample << 4; payload: w0 = pitch (16.16, 1.0 = native rate),
                                      //   w1 = vol L | vol R << 8
#define TRI_OP_AUDIO_STOP       0x62  // imm = voice. nwords 0
#define TRI_OP_AUDIO_VOL        0x63  // imm = voice (0xFFFF = master); payload: w0 = vol L | vol R << 8
#define TRI_OP_AUDIO_FM_PATCH   0x64  // imm = patch id (0-31); payload: 5 words, see tri_fm_patch layout below
#define TRI_OP_AUDIO_FM_ON      0x65  // imm = voice | patch << 4; payload: w0 = frequency (Hz, 16.16), w1 = vol L | vol R << 8
#define TRI_OP_AUDIO_FM_OFF     0x66  // imm = voice (enters release). nwords 0
#define TRI_OP_AUDIO_FM_FREQ    0x67  // imm = voice; payload: w0 = frequency (Hz, 16.16) (pitch bend / portamento)
#define TRI_AUDIO_VOICES 16
#define TRI_AUDIO_SAMPLES 64
#define TRI_AUDIO_FM_PATCHES 32
#define TRI_AUDIO_RATE 48000
// FM patch, 5 words:
//   w0 = algorithm (0 = op0 modulates op1, 1 = both carriers / additive) | feedback (0-7, op0 self-modulation) << 4
//   w1 = op0: mult (0-15) | detune (signed 4 bit) << 4 | waveform (0 sine, 1 half, 2 abs, 3 pulse) << 8 | level (0-255, 255 = loudest) << 16
//   w2 = op0: attack | decay << 8 | sustain << 16 | release << 24  (rates 0-255, sustain is a level)
//   w3 = op1: as w1
//   w4 = op1: as w2
#define TRI_FM_OP_W0(mult, detune, wave, level) \
	((uint32_t)((mult) & 15) | ((uint32_t)((detune) & 15) << 4) | ((uint32_t)((wave) & 3) << 8) | ((uint32_t)((level) & 255) << 16))
#define TRI_FM_OP_W1(attack, decay, sustain, release) \
	((uint32_t)((attack) & 255) | ((uint32_t)((decay) & 255) << 8) | ((uint32_t)((sustain) & 255) << 16) | ((uint32_t)((release) & 255) << 24))
