// SDK graphics: VRAM uploads, palettes, sheet/tileset/tilemap definitions,
// asset tables, tile layers, tilemap writes, objects and the fixed-point
// trigonometry the affine sprites use.
#include <string.h>
#include "tri_sdk_internal.h"

// The system overlay owns VDP2 tileset 7, tilemap 7, palette 15 and layer 3.
#define GAME_TILESETS 7
#define GAME_TILEMAPS 7
#define GAME_LAYERS   4
#define V2_GAME_PALETTES 15

static bool vdp_ok(int vdp, const char *fn) { return tri_check(vdp == TRI_VDP1 || vdp == TRI_VDP2, fn, "vdp must be TRI_VDP1 or TRI_VDP2"); }
static inline uint32_t pack(int lo, int hi) { return ((uint32_t)lo & 0xFFFF) | ((uint32_t)hi << 16); }

// ---- uploads and palettes ---------------------------------------------------------------------------
void tri_vram_upload(int vdp, uint32_t off, const void *data, uint32_t len) {
	if (!vdp_ok(vdp, "tri_vram_upload")) return;
	uint32_t sram = vdp == TRI_VDP1 ? TRI_VDP1_VRAM_SIZE : TRI_VDP2_VRAM_SIZE;
	bool in_sram = off < sram && len <= sram - off;
	bool in_psram = off >= TRI_VRAM_PSRAM && off - TRI_VRAM_PSRAM < TRI_VRAM_PSRAM_SIZE && len <= TRI_VRAM_PSRAM_SIZE - (off - TRI_VRAM_PSRAM);
	if (!tri_check(in_sram || in_psram, "tri_vram_upload", "range outside VRAM (SRAM below its size, PSRAM from TRI_VRAM_PSRAM)")) return;
	const uint8_t *p = data;
	uint32_t w[TRI_MAX_NWORDS + 1];
	while (len) {
		uint32_t chunk = len > (TRI_MAX_NWORDS - 2) * 4 ? (TRI_MAX_NWORDS - 2) * 4 : len;
		uint32_t nw = (chunk + 3) / 4;
		w[0] = TRI_HDR(TRI_OP_UPLOAD, 2 + nw, 0); w[1] = off; w[2] = chunk;
		w[3 + nw - 1] = 0;                        // the tail of a partial word
		memcpy(w + 3, p, chunk);
		tri_emit_deliver(vdp, w, (int)(3 + nw));
		p += chunk; off += chunk; len -= chunk;
	}
}

// VRAM to VRAM on the VDP itself (no link traffic): e.g. a level's sheets kept
// in PSRAM and the active one pulled into an SRAM slot.
void tri_vram_copy(int vdp, uint32_t dst, uint32_t src, uint32_t len) {
	if (!vdp_ok(vdp, "tri_vram_copy")) return;
	uint32_t sram = vdp == TRI_VDP1 ? TRI_VDP1_VRAM_SIZE : TRI_VDP2_VRAM_SIZE;
	bool ok = true;
	for (int i = 0; i < 2; i++) {
		uint32_t off = i ? src : dst;
		bool in_sram = off < sram && len <= sram - off;
		bool in_psram = off >= TRI_VRAM_PSRAM && off - TRI_VRAM_PSRAM < TRI_VRAM_PSRAM_SIZE && len <= TRI_VRAM_PSRAM_SIZE - (off - TRI_VRAM_PSRAM);
		ok = ok && (in_sram || in_psram);
	}
	if (!tri_check(ok, "tri_vram_copy", "range outside VRAM (SRAM below its size, PSRAM from TRI_VRAM_PSRAM)")) return;
	uint32_t c[4] = { TRI_HDR(TRI_OP_VRAM_COPY, 3, 0), dst, src, len };
	tri_emit_deliver(vdp, c, 4);
}

void tri_palette_set(int vdp, int pal, int first, const uint16_t *rgb, int n) {
	if (!vdp_ok(vdp, "tri_palette_set")) return;
	if (!tri_check(pal >= 0 && pal < 16, "tri_palette_set", "palette out of range")) return;
	if (!tri_check(first >= 0 && n >= 0 && first + n <= 256, "tri_palette_set", "entries outside 0..255")) return;
	uint32_t w[1 + 128];
	while (n > 0) {
		int take = n > 254 ? 254 : n;             // 127 words per command, two entries each
		int nw = (take + 1) / 2;
		w[0] = TRI_HDR(TRI_OP_PALETTE, nw, (pal & 15) | ((first & 0xFF) << 4));
		for (int i = 0; i < nw; i++) {
			uint32_t lo = rgb[2 * i], hi = (2 * i + 1 < take) ? rgb[2 * i + 1] : 0;
			w[1 + i] = lo | (hi << 16);
		}
		tri_emit(vdp, w, 1 + nw);
		rgb += take; first += take; n -= take;
	}
}

void tri_palette_entry(int vdp, int pal, int index, uint16_t rgb555) { tri_palette_set(vdp, pal, index, &rgb555, 1); }

// ---- definitions -------------------------------------------------------------------------------------
typedef struct { uint32_t off; uint8_t log2w, log2h; bool defined; } tilemap_info_t;
static tilemap_info_t maps[GAME_TILEMAPS];

void tri_sheet_define(int id, uint32_t off, int w, int h) {
	if (!tri_check(id >= 0 && id < 16, "tri_sheet_define", "sheet out of range")) return;
	if (!tri_check(w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF, "tri_sheet_define", "bad size")) return;
	uint32_t c[3] = { TRI_HDR(TRI_OP_SHEET_DEF, 2, id), off, pack(w, h) };
	tri_emit(TRI_DEST_VDP1, c, 3);
}

void tri_tileset_define(int id, uint32_t off, int ntiles) {
	if (!tri_check(id >= 0 && id < GAME_TILESETS, "tri_tileset_define", "tileset out of range (7 is the overlay's)")) return;
	if (!tri_check(ntiles > 0 && ntiles <= 4096, "tri_tileset_define", "tile count out of range")) return;
	if (!tri_check((off & 3) == 0, "tri_tileset_define", "offset must be 4-byte aligned")) return;
	uint32_t c[3] = { TRI_HDR(TRI_OP_TILESET_DEF, 2, id), off, (uint32_t)ntiles };
	tri_emit(TRI_DEST_VDP2, c, 3);
}

void tri_tilemap_define(int id, uint32_t off, int log2w, int log2h) {
	if (!tri_check(id >= 0 && id < GAME_TILEMAPS, "tri_tilemap_define", "tilemap out of range (7 is the overlay's)")) return;
	if (!tri_check(log2w >= 0 && log2w <= 10 && log2h >= 0 && log2h <= 10, "tri_tilemap_define", "log2 size out of range")) return;
	uint32_t c[3] = { TRI_HDR(TRI_OP_TILEMAP_DEF, 2, id), off, (uint32_t)log2w | ((uint32_t)log2h << 8) };
	tri_emit(TRI_DEST_VDP2, c, 3);
	maps[id] = (tilemap_info_t){ off, (uint8_t)log2w, (uint8_t)log2h, true };
}

static const tilemap_info_t *map_info(int id, const char *fn) {
	if (!tri_check(id >= 0 && id < GAME_TILEMAPS && maps[id].defined, fn, "tilemap not defined")) return NULL;
	return &maps[id];
}
int tri_tilemap_width(int id)  { return (id >= 0 && id < GAME_TILEMAPS && maps[id].defined) ? 1 << maps[id].log2w : 0; }
int tri_tilemap_height(int id) { return (id >= 0 && id < GAME_TILEMAPS && maps[id].defined) ? 1 << maps[id].log2h : 0; }

void tri_asset_load(const tri_asset_t *a) {
	switch (a->kind) {
	case TRI_ASSET_V1_SHEET:   tri_vram_upload(TRI_VDP1, a->vram_off, a->data, a->len); tri_sheet_define(a->id, a->vram_off, a->w, a->h); break;
	case TRI_ASSET_V2_TILESET: tri_vram_upload(TRI_VDP2, a->vram_off, a->data, a->len); tri_tileset_define(a->id, a->vram_off, a->w); break;
	case TRI_ASSET_V2_TILEMAP: tri_vram_upload(TRI_VDP2, a->vram_off, a->data, a->len); tri_tilemap_define(a->id, a->vram_off, a->w, a->h); break;
	case TRI_ASSET_V1_PALETTE: tri_palette_set(TRI_VDP1, a->id, 0, a->data, a->w); break;
	case TRI_ASSET_V2_PALETTE: tri_palette_set(TRI_VDP2, a->id, 0, a->data, a->w); break;
	case TRI_ASSET_SAMPLE:     tri_vram_upload(TRI_VDP2, a->vram_off, a->data, a->len); tri_audio_sample_define(a->id, a->vram_off, a->len, a->loop, a->rate); break;
	default: tri_api_error("tri_asset_load", "unknown asset kind"); break;
	}
}

void tri_assets_load(const tri_asset_t *table, int count) {
	for (int i = 0; i < count; i++) tri_asset_load(&table[i]);
}

// ---- layers and backdrop -----------------------------------------------------------------------------------
// The LAYER command carries the whole configuration, so a shadow copy lets
// enable and priority be changed on their own.
static struct { bool enable; uint8_t tileset, tilemap, pal_base, prio; } layers[GAME_LAYERS];

static void emit_layer(int n) {
	uint32_t c[2] = { TRI_HDR(TRI_OP_LAYER, 1, n),
	                  TRI_LAYER_CFG(layers[n].enable, layers[n].tileset, layers[n].tilemap, layers[n].pal_base, layers[n].prio) };
	tri_emit(TRI_DEST_VDP2, c, 2);
}
static bool layer_ok(int n, const char *fn) { return tri_check(n >= 0 && n < GAME_LAYERS, fn, "layer must be 0..3 (4 is the overlay's)"); }

void tri_layer_set(int n, int tileset, int tilemap, int pal_base, int prio) {
	if (!layer_ok(n, "tri_layer_set")) return;
	if (!tri_check(tileset >= 0 && tileset < GAME_TILESETS && tilemap >= 0 && tilemap < GAME_TILEMAPS, "tri_layer_set", "tileset/tilemap out of range")) return;
	if (!tri_check(pal_base >= 0 && pal_base < 16 && (prio == 0 || prio == 1), "tri_layer_set", "palette base or priority out of range")) return;
	layers[n].enable = true;
	layers[n].tileset = (uint8_t)tileset; layers[n].tilemap = (uint8_t)tilemap;
	layers[n].pal_base = (uint8_t)pal_base; layers[n].prio = (uint8_t)prio;
	emit_layer(n);
}
void tri_layer_enable(int n, bool enable) {
	if (!layer_ok(n, "tri_layer_enable")) return;
	layers[n].enable = enable;
	emit_layer(n);
}
void tri_layer_priority(int n, int prio) {
	if (!layer_ok(n, "tri_layer_priority")) return;
	if (!tri_check(prio == 0 || prio == 1, "tri_layer_priority", "priority must be TRI_LAYER_BELOW or TRI_LAYER_ABOVE")) return;
	layers[n].prio = (uint8_t)prio;
	emit_layer(n);
}
void tri_layer_scroll(int n, int x, int y) {
	if (!layer_ok(n, "tri_layer_scroll")) return;
	uint32_t c[2] = { TRI_HDR(TRI_OP_SCROLL, 1, n), pack(x, y) };
	tri_emit(TRI_DEST_VDP2, c, 2);
}
void tri_backdrop_set(uint16_t rgb555) {
	uint32_t c = TRI_HDR(TRI_OP_BACKDROP, 0, rgb555 & 0x7FFF);
	tri_emit(TRI_DEST_VDP2, &c, 1);
}

void tri_gfx_reset(void) {
	memset(layers, 0, sizeof layers);
	memset(maps, 0, sizeof maps);
	for (int n = 0; n < GAME_LAYERS; n++) {
		emit_layer(n);
		tri_layer_scroll(n, 0, 0);   // scroll lives on VDP2 and outlives a CPU reset or a game's exit
	}
	tri_backdrop_set(0);
}

// ---- tilemap contents --------------------------------------------------------------------------------------
void tri_tilemap_write(int id, int x, int y, const uint16_t *entries, int n) {
	const tilemap_info_t *m = map_info(id, "tri_tilemap_write");
	if (!m) return;
	int w = 1 << m->log2w, h = 1 << m->log2h;
	if (!tri_check(x >= 0 && x < w && y >= 0 && y < h, "tri_tilemap_write", "cell outside the map")) return;
	if (n <= 0) return;
	if (x + n > w) n = w - x;
	tri_vram_upload(TRI_VDP2, m->off + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 2, entries, (uint32_t)n * 2);
}
void tri_tilemap_set(int id, int x, int y, uint16_t entry) { tri_tilemap_write(id, x, y, &entry, 1); }
void tri_tilemap_fill(int id, int x, int y, int w, int h, uint16_t entry) {
	static uint16_t row[1024];
	if (!map_info(id, "tri_tilemap_fill")) return;
	if (w > 1024) w = 1024;
	for (int i = 0; i < w; i++) row[i] = entry;
	for (int j = 0; j < h; j++) tri_tilemap_write(id, x, y + j, row, w);
}

// ---- objects ---------------------------------------------------------------------------------------------------
static bool sprite_ok(int sheet, int pal, int w, int h, const char *fn) {
	return tri_check(sheet >= 0 && sheet < 16 && pal >= 0 && pal < 16, fn, "sheet or palette out of range")
	    && tri_check(w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF, fn, "bad cell size");
}
static uint32_t attr(int sheet, int pal, unsigned flags) {
	int blend = (flags & TRI_SPRITE_BLEND_HALF) ? TRI_BLEND_HALF : (flags & TRI_SPRITE_BLEND_ADD) ? TRI_BLEND_ADD : TRI_BLEND_NONE;
	return TRI_SPR_ATTR(sheet, pal, (flags & TRI_SPRITE_HFLIP) ? 1 : 0, (flags & TRI_SPRITE_VFLIP) ? 1 : 0, blend);
}

void tri_sprite_draw(int x, int y, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags) {
	if (!sprite_ok(sheet, pal, w, h, "tri_sprite_draw")) return;
	uint32_t c[5] = { TRI_HDR(TRI_OP_SPRITE, 4, 0), pack(x, y), pack(sx, sy), pack(w, h), attr(sheet, pal, flags) };
	tri_emit(TRI_DEST_VDP1, c, 5);
}
void tri_sprite_draw_matrix(int cx, int cy, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags, int a, int b, int cc, int d) {
	if (!sprite_ok(sheet, pal, w, h, "tri_sprite_draw_matrix")) return;
	uint32_t c[7] = { TRI_HDR(TRI_OP_SPRITE_AFFINE, 6, 0), pack(cx, cy), pack(sx, sy), pack(w, h), attr(sheet, pal, flags), pack(a, b), pack(cc, d) };
	tri_emit(TRI_DEST_VDP1, c, 7);
}
void tri_sprite_draw_rotated(int cx, int cy, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags, int angle, int scale) {
	if (scale < 16) scale = 16;
	int cs = tri_cos(angle), sn = tri_sin(angle);
	// destination -> source: rotate by -angle and divide by the scale (8.8 in, 8.8 out)
	int a = (cs << 8) / scale, b = (sn << 8) / scale;
	tri_sprite_draw_matrix(cx, cy, sheet, sx, sy, w, h, pal, flags, a, b, -b, a);
}
void tri_rect_fill(int x, int y, int w, int h, uint16_t rgb555) {
	if (!tri_check(w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF, "tri_rect_fill", "bad size")) return;
	uint32_t c[3] = { TRI_HDR(TRI_OP_RECT, 2, rgb555 & 0x7FFF), pack(x, y), pack(w, h) };
	tri_emit(TRI_DEST_VDP1, c, 3);
}

// sine, 8.8 fixed, 1024 entries per turn (angles are 0..4095: 4 per entry, no interpolation)
static const int16_t sin_table[1024] = {
	   0,    2,    3,    5,    6,    8,    9,   11,   13,   14,   16,   17,   19,   20,   22,   24,
	  25,   27,   28,   30,   31,   33,   34,   36,   38,   39,   41,   42,   44,   45,   47,   48,
	  50,   51,   53,   55,   56,   58,   59,   61,   62,   64,   65,   67,   68,   70,   71,   73,
	  74,   76,   77,   79,   80,   82,   83,   85,   86,   88,   89,   91,   92,   94,   95,   97,
	  98,   99,  101,  102,  104,  105,  107,  108,  109,  111,  112,  114,  115,  117,  118,  119,
	 121,  122,  123,  125,  126,  128,  129,  130,  132,  133,  134,  136,  137,  138,  140,  141,
	 142,  144,  145,  146,  147,  149,  150,  151,  152,  154,  155,  156,  157,  159,  160,  161,
	 162,  164,  165,  166,  167,  168,  170,  171,  172,  173,  174,  175,  177,  178,  179,  180,
	 181,  182,  183,  184,  185,  186,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,
	 198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  207,  208,  209,  210,  211,  212,
	 213,  214,  215,  215,  216,  217,  218,  219,  220,  220,  221,  222,  223,  224,  224,  225,
	 226,  227,  227,  228,  229,  229,  230,  231,  231,  232,  233,  233,  234,  235,  235,  236,
	 237,  237,  238,  238,  239,  239,  240,  241,  241,  242,  242,  243,  243,  244,  244,  245,
	 245,  245,  246,  246,  247,  247,  248,  248,  248,  249,  249,  249,  250,  250,  250,  251,
	 251,  251,  252,  252,  252,  252,  253,  253,  253,  253,  254,  254,  254,  254,  254,  255,
	 255,  255,  255,  255,  255,  255,  256,  256,  256,  256,  256,  256,  256,  256,  256,  256,
	 256,  256,  256,  256,  256,  256,  256,  256,  256,  256,  256,  255,  255,  255,  255,  255,
	 255,  255,  254,  254,  254,  254,  254,  253,  253,  253,  253,  252,  252,  252,  252,  251,
	 251,  251,  250,  250,  250,  249,  249,  249,  248,  248,  248,  247,  247,  246,  246,  245,
	 245,  245,  244,  244,  243,  243,  242,  242,  241,  241,  240,  239,  239,  238,  238,  237,
	 237,  236,  235,  235,  234,  233,  233,  232,  231,  231,  230,  229,  229,  228,  227,  227,
	 226,  225,  224,  224,  223,  222,  221,  220,  220,  219,  218,  217,  216,  215,  215,  214,
	 213,  212,  211,  210,  209,  208,  207,  207,  206,  205,  204,  203,  202,  201,  200,  199,
	 198,  197,  196,  195,  194,  193,  192,  191,  190,  189,  188,  186,  185,  184,  183,  182,
	 181,  180,  179,  178,  177,  175,  174,  173,  172,  171,  170,  168,  167,  166,  165,  164,
	 162,  161,  160,  159,  157,  156,  155,  154,  152,  151,  150,  149,  147,  146,  145,  144,
	 142,  141,  140,  138,  137,  136,  134,  133,  132,  130,  129,  128,  126,  125,  123,  122,
	 121,  119,  118,  117,  115,  114,  112,  111,  109,  108,  107,  105,  104,  102,  101,   99,
	  98,   97,   95,   94,   92,   91,   89,   88,   86,   85,   83,   82,   80,   79,   77,   76,
	  74,   73,   71,   70,   68,   67,   65,   64,   62,   61,   59,   58,   56,   55,   53,   51,
	  50,   48,   47,   45,   44,   42,   41,   39,   38,   36,   34,   33,   31,   30,   28,   27,
	  25,   24,   22,   20,   19,   17,   16,   14,   13,   11,    9,    8,    6,    5,    3,    2,
	   0,   -2,   -3,   -5,   -6,   -8,   -9,  -11,  -13,  -14,  -16,  -17,  -19,  -20,  -22,  -24,
	 -25,  -27,  -28,  -30,  -31,  -33,  -34,  -36,  -38,  -39,  -41,  -42,  -44,  -45,  -47,  -48,
	 -50,  -51,  -53,  -55,  -56,  -58,  -59,  -61,  -62,  -64,  -65,  -67,  -68,  -70,  -71,  -73,
	 -74,  -76,  -77,  -79,  -80,  -82,  -83,  -85,  -86,  -88,  -89,  -91,  -92,  -94,  -95,  -97,
	 -98,  -99, -101, -102, -104, -105, -107, -108, -109, -111, -112, -114, -115, -117, -118, -119,
	-121, -122, -123, -125, -126, -128, -129, -130, -132, -133, -134, -136, -137, -138, -140, -141,
	-142, -144, -145, -146, -147, -149, -150, -151, -152, -154, -155, -156, -157, -159, -160, -161,
	-162, -164, -165, -166, -167, -168, -170, -171, -172, -173, -174, -175, -177, -178, -179, -180,
	-181, -182, -183, -184, -185, -186, -188, -189, -190, -191, -192, -193, -194, -195, -196, -197,
	-198, -199, -200, -201, -202, -203, -204, -205, -206, -207, -207, -208, -209, -210, -211, -212,
	-213, -214, -215, -215, -216, -217, -218, -219, -220, -220, -221, -222, -223, -224, -224, -225,
	-226, -227, -227, -228, -229, -229, -230, -231, -231, -232, -233, -233, -234, -235, -235, -236,
	-237, -237, -238, -238, -239, -239, -240, -241, -241, -242, -242, -243, -243, -244, -244, -245,
	-245, -245, -246, -246, -247, -247, -248, -248, -248, -249, -249, -249, -250, -250, -250, -251,
	-251, -251, -252, -252, -252, -252, -253, -253, -253, -253, -254, -254, -254, -254, -254, -255,
	-255, -255, -255, -255, -255, -255, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256,
	-256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -255, -255, -255, -255, -255,
	-255, -255, -254, -254, -254, -254, -254, -253, -253, -253, -253, -252, -252, -252, -252, -251,
	-251, -251, -250, -250, -250, -249, -249, -249, -248, -248, -248, -247, -247, -246, -246, -245,
	-245, -245, -244, -244, -243, -243, -242, -242, -241, -241, -240, -239, -239, -238, -238, -237,
	-237, -236, -235, -235, -234, -233, -233, -232, -231, -231, -230, -229, -229, -228, -227, -227,
	-226, -225, -224, -224, -223, -222, -221, -220, -220, -219, -218, -217, -216, -215, -215, -214,
	-213, -212, -211, -210, -209, -208, -207, -207, -206, -205, -204, -203, -202, -201, -200, -199,
	-198, -197, -196, -195, -194, -193, -192, -191, -190, -189, -188, -186, -185, -184, -183, -182,
	-181, -180, -179, -178, -177, -175, -174, -173, -172, -171, -170, -168, -167, -166, -165, -164,
	-162, -161, -160, -159, -157, -156, -155, -154, -152, -151, -150, -149, -147, -146, -145, -144,
	-142, -141, -140, -138, -137, -136, -134, -133, -132, -130, -129, -128, -126, -125, -123, -122,
	-121, -119, -118, -117, -115, -114, -112, -111, -109, -108, -107, -105, -104, -102, -101,  -99,
	 -98,  -97,  -95,  -94,  -92,  -91,  -89,  -88,  -86,  -85,  -83,  -82,  -80,  -79,  -77,  -76,
	 -74,  -73,  -71,  -70,  -68,  -67,  -65,  -64,  -62,  -61,  -59,  -58,  -56,  -55,  -53,  -51,
	 -50,  -48,  -47,  -45,  -44,  -42,  -41,  -39,  -38,  -36,  -34,  -33,  -31,  -30,  -28,  -27,
	 -25,  -24,  -22,  -20,  -19,  -17,  -16,  -14,  -13,  -11,   -9,   -8,   -6,   -5,   -3,   -2,
};
int tri_sin(int angle) { return sin_table[((unsigned)angle >> 2) & 1023]; }
int tri_cos(int angle) { return sin_table[(((unsigned)angle >> 2) + 256) & 1023]; }

// FPC: this file's statics as at boot at each game's start (tri_sdk.h)
TRI_STATE(gfx)
