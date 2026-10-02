// Asset records. tools/mkassets.py generates a table of these from a game's
// assets.toml (PNG sheets/tilesets, Tiled maps, WAV samples, palettes); the SDK
// uploads and registers all of them with tri_assets_load(). Games refer to
// assets by the generated ASSET_* ids, never by address.
#pragma once
#include <stdint.h>

typedef enum {
	TRI_ASSET_V1_SHEET = 0,   // 8bpp pixels -> VDP1 VRAM; id = sheet id (0-15); w, h = size in pixels; pal = palette id
	TRI_ASSET_V2_TILESET,     // 8bpp 8x8 tiles -> VDP2 VRAM; id = tileset id (0-7); w = tile count
	TRI_ASSET_V2_TILEMAP,     // uint16 entries -> VDP2 VRAM; id = tilemap id (0-7); w, h = log2 width/height in tiles
	TRI_ASSET_V1_PALETTE,     // RGB555 entries; id = palette (0-15); w = entry count (first entry index 0)
	TRI_ASSET_V2_PALETTE,     // RGB555 entries; id = palette (0-15); w = entry count
	TRI_ASSET_SAMPLE,         // 8-bit signed mono -> VDP2 VRAM; id = sample id (0-63); len = samples; rate = Hz; loop = loop start or 0xFFFFFFFF
} tri_asset_kind_t;

typedef struct {
	uint8_t  kind;            // tri_asset_kind_t
	uint8_t  id;
	uint16_t w, h;
	uint32_t vram_off;        // destination offset in the VDP's VRAM (assigned by the tool)
	uint32_t len;             // bytes in data
	const void *data;
	uint32_t rate;            // samples: native rate
	uint32_t loop;            // samples: loop start
	uint8_t  pal;             // sheets: default palette
} tri_asset_t;
