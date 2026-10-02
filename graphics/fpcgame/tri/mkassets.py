#!/usr/bin/env python3
"""mkassets.py - TRI2350 asset pipeline.

    mkassets.py <assets.toml> <out_dir> [--quiet]

Reads a game's assets.toml (PNG sheets and tilesets, Tiled CSV tilemaps, WAV
samples, palette slot assignments), converts everything to the console's
native formats, assigns VRAM addresses and hardware ids, and writes
<out_dir>/assets.h and <out_dir>/assets.c: a table of tri_asset_t records
(firmware/sdk/tri_assets.h) that the SDK uploads with tri_assets_load().
Games refer to the records through the generated ASSET_* ids (an index into
the table, and the hardware sheet/tileset/tilemap/sample/palette ids).

Standard library only (Python 3.11+): zlib, struct, wave, tomllib.
"""
import os
import struct
import sys
import wave
import zlib

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("mkassets.py needs Python 3.11+ (tomllib)")

# ---- console limits (keep in sync with the firmware) ---------------------------
VDP1_VRAM_SIZE = 48 * 1024                 # SRAM sheets (VDP1_VRAM_SIZE in firmware/vdp1/vdp1.h)
VDP2_VRAM_SIZE = 128 * 1024
VRAM_PSRAM = 0x100000                        # VRAM offsets from here are the VDP's PSRAM (TRI_VRAM_PSRAM, tri_isa.h)
VRAM_PSRAM_SIZE = 0x380000                   # TRI_VRAM_PSRAM_SIZE: 3.5 MB per VDP on the FPGA
VDP2_VRAM_USABLE = VDP2_VRAM_SIZE - 0x4000   # top 16 KB: system overlay font + map
VDP2_TEXT_AREA = 0x18000                     # tri_text_init()'s default font + map live at 0x18000..0x1C000
MAX_SHEETS, MAX_TILESETS, MAX_TILEMAPS, MAX_SAMPLES = 16, 7, 7, 64   # VDP2 tileset/tilemap 7 belong to the overlay
MAX_V1_PALETTES, MAX_V2_PALETTES = 16, 15   # VDP2 palette 15 belongs to the overlay
MAX_COLOURS = 255                           # per palette, index 0 is transparent
MAX_TILE_INDEX = 4095                       # 12-bit tile index in a tilemap entry
MAX_MAP_LOG2 = 10
TILE_BYTES = 64
KIND = {"v1_sheet": 0, "v2_tileset": 1, "v2_tilemap": 2, "v1_palette": 3, "v2_palette": 4, "sample": 5}
TILED_HFLIP, TILED_VFLIP, TILED_DFLIP = 0x80000000, 0x40000000, 0x20000000


class AssetError(Exception):
    pass


# ---- PNG decoder (8-bit RGB/RGBA/grey/grey+alpha, 1/2/4/8-bit palette) --------
def read_png(path):
    """Return (width, height, pixels) with pixels a list of (r, g, b, a) rows."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssetError(f"{path}: not a PNG file")
    pos, chunks, idat = 8, {}, []
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
        else:
            chunks.setdefault(ctype, body)
    if b"IHDR" not in chunks:
        raise AssetError(f"{path}: missing IHDR")
    w, h, depth, ctype, comp, flt, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    if interlace:
        raise AssetError(f"{path}: interlaced PNGs are not supported (re-save without interlacing)")
    if ctype == 3:
        if depth not in (1, 2, 4, 8):
            raise AssetError(f"{path}: unsupported palette bit depth {depth}")
    elif depth != 8:
        raise AssetError(f"{path}: only 8-bit channels are supported (got {depth}-bit, colour type {ctype})")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        raise AssetError(f"{path}: unsupported colour type {ctype}")
    bits_pp = channels * depth
    bpp = max(1, bits_pp // 8)                       # bytes per complete pixel, for the filters
    stride = (w * bits_pp + 7) // 8
    raw = zlib.decompress(b"".join(idat))
    if len(raw) != h * (stride + 1):
        raise AssetError(f"{path}: unexpected image data size")
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise AssetError(f"{path}: bad filter type {ftype}")
        rows.append(bytes(line))
        prev = line
    palette = chunks.get(b"PLTE", b"")
    trns = chunks.get(b"tRNS", b"")
    out = []
    for line in rows:
        px = []
        if ctype == 3:
            for x in range(w):
                if depth == 8:
                    idx = line[x]
                else:
                    per_byte = 8 // depth
                    idx = (line[x // per_byte] >> (8 - depth * (x % per_byte + 1))) & ((1 << depth) - 1)
                if idx * 3 + 2 >= len(palette):
                    raise AssetError(f"{path}: palette index {idx} outside PLTE")
                a = trns[idx] if idx < len(trns) else 255
                px.append((palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2], a))
        elif ctype == 0:
            px = [(v, v, v, 255) for v in line]
        elif ctype == 4:
            px = [(line[i], line[i], line[i], line[i + 1]) for i in range(0, 2 * w, 2)]
        elif ctype == 2:
            px = [(line[i], line[i + 1], line[i + 2], 255) for i in range(0, 3 * w, 3)]
        else:
            px = [(line[i], line[i + 1], line[i + 2], line[i + 3]) for i in range(0, 4 * w, 4)]
        out.append(px)
    return w, h, out


def is_transparent(rgba):
    r, g, b, a = rgba
    return a < 128 or (r, g, b) == (255, 0, 255)


def rgb555(rgba):
    r, g, b, _ = rgba
    return (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)


class Palette:
    """Colours in order of first use; index 0 is transparent."""

    def __init__(self, name, vdp):
        self.name, self.vdp = name, vdp
        self.colours = [0]                     # RGB555 values, entry 0 = transparent
        self.lookup = {}
        self.slot = None
        self.users = []

    def index(self, rgba, owner):
        if is_transparent(rgba):
            return 0
        c = rgb555(rgba)
        i = self.lookup.get(c)
        if i is None:
            if len(self.colours) > MAX_COLOURS:
                raise AssetError(f"{owner}: more than {MAX_COLOURS} colours in palette '{self.name}' "
                                 "(reduce the image's colours or split it)")
            i = len(self.colours)
            self.colours.append(c)
            self.lookup[c] = i
        return i


# ---- audio ----------------------------------------------------------------------
def read_wav(path, rate=None):
    """Return (rate, samples) with samples a list of ints in -128..127 (mono)."""
    with wave.open(path, "rb") as wf:
        nch, width, src_rate, n = wf.getnchannels(), wf.getsampwidth(), wf.getframerate(), wf.getnframes()
        raw = wf.readframes(n)
    if width not in (1, 2, 3, 4):
        raise AssetError(f"{path}: unsupported sample width {width}")
    frames = []
    step = width * nch
    for i in range(0, len(raw) - step + 1, step):
        acc = 0
        for c in range(nch):
            s = raw[i + c * width:i + (c + 1) * width]
            if width == 1:
                v = (s[0] - 128) * 256                 # 8-bit WAV is unsigned
            else:
                v = int.from_bytes(s, "little", signed=True) >> (8 * (width - 2))
            acc += v
        frames.append(acc // nch)                      # 16-bit mono
    if rate and rate != src_rate:                      # linear resample
        out, ratio = [], src_rate / rate
        count = int(len(frames) / ratio)
        for j in range(count):
            p = j * ratio
            i = int(p)
            f = p - i
            a = frames[i]
            b = frames[i + 1] if i + 1 < len(frames) else a
            out.append(int(a + (b - a) * f))
        frames, src_rate = out, rate
    return src_rate, [max(-128, min(127, v >> 8)) for v in frames]


# ---- the build ------------------------------------------------------------------
class Build:
    def __init__(self, toml_path, quiet=False):
        self.dir = os.path.dirname(os.path.abspath(toml_path))
        self.quiet = quiet
        with open(toml_path, "rb") as f:
            self.cfg = tomllib.load(f)
        self.assets = []             # dicts: name, kind, id, w, h, vram, data(bytes), rate, loop, pal, defines
        self.p1_used = 0             # PSRAM bytes placed on VDP1 / VDP2
        self.p2_used = 0
        self.names = set()
        self.palettes = {}           # name -> Palette (named after the sheet/tileset that owns it)
        self.tilesets = {}           # name -> dict(ntiles, remap)
        self.v1_used = 0
        self.v2_used = 0
        self.counts = {"sheet": 0, "tileset": 0, "tilemap": 0, "sample": 0}

    # -- helpers
    def path(self, rel):
        p = os.path.join(self.dir, rel)
        if not os.path.exists(p):
            raise AssetError(f"missing file {p}")
        return p

    def unique(self, name):
        if not name or not all(c.isalnum() or c == "_" for c in name):
            raise AssetError(f"bad asset name '{name}' (letters, digits, underscore)")
        if name in self.names:
            raise AssetError(f"duplicate asset name '{name}'")
        self.names.add(name)

    # VRAM placement: SRAM unless the entry says psram = true or SRAM is full,
    # then the VDP's PSRAM (offsets from VRAM_PSRAM). Each VDP has its own cursors.
    def alloc(self, vdp, size, owner, psram=False, align=1):
        sram_limit = VDP1_VRAM_SIZE if vdp == 1 else VDP2_VRAM_USABLE
        used = self.v1_used if vdp == 1 else self.v2_used
        off = (used + align - 1) & ~(align - 1)
        if not psram and off + size <= sram_limit:
            if vdp == 1: self.v1_used = off + size
            else: self.v2_used = off + size
            return off
        pused = self.p1_used if vdp == 1 else self.p2_used
        poff = (pused + align - 1) & ~(align - 1)
        if poff + size > VRAM_PSRAM_SIZE:
            raise AssetError(f"{owner}: VDP{vdp} PSRAM full ({poff + size} > {VRAM_PSRAM_SIZE} bytes)")
        if vdp == 1: self.p1_used = poff + size
        else: self.p2_used = poff + size
        return VRAM_PSRAM + poff

    def alloc_v1(self, size, owner, psram=False):
        return self.alloc(1, size, owner, psram)

    def alloc_v2(self, size, owner, align=1, psram=False):
        return self.alloc(2, size, owner, psram, align)

    def next_id(self, kind, limit, owner):
        n = self.counts[kind]
        if n >= limit:
            raise AssetError(f"{owner}: too many {kind}s (max {limit})")
        self.counts[kind] = n + 1
        return n

    def palette_for(self, entry, owner, vdp):
        shared = entry.get("palette")
        if shared:
            pal = self.palettes.get(shared)
            if pal is None:
                raise AssetError(f"{owner}: palette='{shared}' refers to an unknown sheet/tileset "
                                 "(define it earlier in the file)")
            if pal.vdp != vdp:
                raise AssetError(f"{owner}: cannot share a VDP{pal.vdp} palette with a VDP{vdp} asset")
        else:
            pal = Palette(owner, vdp)
            self.palettes[owner] = pal
        pal.users.append(owner)
        return pal

    # -- asset kinds
    def add_sheet(self, e):
        name = e.get("name")
        self.unique(name)
        vdp = int(e.get("vdp", 1))
        if vdp != 1:
            raise AssetError(f"{name}: sheets go to VDP1 (vdp=1)")
        w, h, px = read_png(self.path(e["file"]))
        if w > 0xFFFF or h > 0xFFFF or w * h > VRAM_PSRAM_SIZE:
            raise AssetError(f"{name}: sheet {w}x{h} exceeds VDP1 VRAM")
        pal = self.palette_for(e, name, 1)
        data = bytearray(w * h)
        for y in range(h):
            for x in range(w):
                data[y * w + x] = pal.index(px[y][x], name)
        sid = self.next_id("sheet", MAX_SHEETS, name)
        off = self.alloc_v1(len(data), name, psram=bool(e.get("psram", False)))
        self.assets.append(dict(name=name, kind="v1_sheet", id=sid, w=w, h=h, vram=off, data=bytes(data),
                                rate=0, loop=0, pal=pal, defines={"SHEET": sid, "W": w, "H": h}))

    def add_tileset(self, e):
        name = e.get("name")
        self.unique(name)
        vdp = int(e.get("vdp", 2))
        if vdp != 2:
            raise AssetError(f"{name}: tilesets go to VDP2 (vdp=2)")
        tile = int(e.get("tile", 8))
        if tile != 8:
            raise AssetError(f"{name}: only 8x8 tiles are supported")
        dedupe = bool(e.get("dedupe", True))
        w, h, px = read_png(self.path(e["file"]))
        if w % tile or h % tile:
            raise AssetError(f"{name}: image {w}x{h} is not a multiple of {tile}")
        pal = self.palette_for(e, name, 2)
        cols, rows = w // tile, h // tile
        tiles = [bytes(TILE_BYTES)]                 # tile 0: empty by convention (skipped by the compositor)
        seen = {tiles[0]: 0}
        remap = [0]                                 # Tiled local id + 1 -> stored index
        for ty in range(rows):
            for tx in range(cols):
                t = bytearray(TILE_BYTES)
                for y in range(tile):
                    for x in range(tile):
                        t[y * tile + x] = pal.index(px[ty * tile + y][tx * tile + x], name)
                t = bytes(t)
                if dedupe and t in seen:
                    remap.append(seen[t])
                else:
                    if len(tiles) > MAX_TILE_INDEX:
                        raise AssetError(f"{name}: more than {MAX_TILE_INDEX} tiles")
                    seen[t] = len(tiles)
                    remap.append(len(tiles))
                    tiles.append(t)
        data = b"".join(tiles)
        tid = self.next_id("tileset", MAX_TILESETS, name)
        off = self.alloc_v2(len(data), name, psram=bool(e.get("psram", False)))
        self.tilesets[name] = dict(ntiles=len(tiles), remap=remap, source_tiles=cols * rows)
        self.assets.append(dict(name=name, kind="v2_tileset", id=tid, w=len(tiles), h=0, vram=off, data=data,
                                rate=0, loop=0, pal=pal,
                                defines={"TILESET": tid, "TILES": len(tiles), "COLS": cols, "ROWS": rows}))

    def add_tilemap(self, e):
        name = e.get("name")
        self.unique(name)
        ts = self.tilesets.get(e.get("tileset"))
        if ts is None:
            raise AssetError(f"{name}: tileset='{e.get('tileset')}' is not a tileset defined earlier in the file")
        pal = int(e.get("pal", 0))
        if not 0 <= pal <= 3:
            raise AssetError(f"{name}: pal must be 0..3 (added to the layer's palette base)")
        rows = []
        with open(self.path(e["file"]), "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip().rstrip(",")
                if line:
                    rows.append([int(v) for v in line.split(",")])
        if not rows:
            raise AssetError(f"{name}: empty map")
        width = max(len(r) for r in rows)
        height = len(rows)
        log2w = max(1, (width - 1).bit_length())
        log2h = max(1, (height - 1).bit_length())
        if log2w > MAX_MAP_LOG2 or log2h > MAX_MAP_LOG2:
            raise AssetError(f"{name}: map {width}x{height} exceeds {1 << MAX_MAP_LOG2} tiles per side")
        mw, mh = 1 << log2w, 1 << log2h
        entries = bytearray(mw * mh * 2)
        for y, row in enumerate(rows):
            for x, gid in enumerate(row):
                if gid < 0:
                    gid &= 0xFFFFFFFF
                hflip = 1 if gid & TILED_HFLIP else 0
                vflip = 1 if gid & TILED_VFLIP else 0
                if gid & TILED_DFLIP:
                    raise AssetError(f"{name}: tile at ({x},{y}) is rotated (diagonal flip); the hardware only flips")
                local = gid & 0x0FFFFFFF
                if local >= len(ts["remap"]):
                    raise AssetError(f"{name}: tile id {local} at ({x},{y}) is outside tileset '{e['tileset']}' "
                                     f"({ts['source_tiles']} tiles; map values are Tiled gids with firstgid 1)")
                index = ts["remap"][local]
                v = index | hflip << 12 | vflip << 13 | pal << 14
                struct.pack_into("<H", entries, (y * mw + x) * 2, v)
        mid = self.next_id("tilemap", MAX_TILEMAPS, name)
        off = self.alloc_v2(len(entries), name, align=2, psram=bool(e.get("psram", False)))
        self.assets.append(dict(name=name, kind="v2_tilemap", id=mid, w=log2w, h=log2h, vram=off, data=bytes(entries),
                                rate=0, loop=0, pal=None,
                                defines={"TILEMAP": mid, "LOG2W": log2w, "LOG2H": log2h, "COLS": width, "ROWS": height}))

    def add_sample(self, e):
        name = e.get("name")
        self.unique(name)
        rate, samples = read_wav(self.path(e["file"]), e.get("rate"))
        loop = int(e.get("loop", -1))
        if loop >= len(samples):
            raise AssetError(f"{name}: loop start {loop} beyond the end ({len(samples)} samples)")
        data = bytes((v & 0xFF) for v in samples)
        sid = self.next_id("sample", MAX_SAMPLES, name)
        off = self.alloc_v2(len(data), name, psram=bool(e.get("psram", False)))
        self.assets.append(dict(name=name, kind="sample", id=sid, w=0, h=0, vram=off, data=data,
                                rate=rate, loop=0xFFFFFFFF if loop < 0 else loop, pal=None,
                                defines={"SAMPLE": sid, "RATE": rate, "LEN": len(samples)}))

    def add_palette_slots(self):
        # explicit slots first, then the rest in order of definition
        explicit = {}
        for e in self.cfg.get("palette", []):
            src = e.get("from")
            pal = self.palettes.get(src)
            if pal is None:
                raise AssetError(f"palette '{e.get('name')}': from='{src}' is not a sheet or tileset")
            slot = int(e.get("slot", -1))
            limit = MAX_V1_PALETTES if pal.vdp == 1 else MAX_V2_PALETTES
            if not 0 <= slot < limit:
                raise AssetError(f"palette '{e.get('name')}': slot {slot} outside 0..{limit - 1} for VDP{pal.vdp}")
            if explicit.get((pal.vdp, slot)):
                raise AssetError(f"palette '{e.get('name')}': VDP{pal.vdp} slot {slot} already assigned")
            explicit[(pal.vdp, slot)] = pal
            pal.slot = slot
            if e.get("name"):
                pal.alias = e["name"]
        for pal in self.palettes.values():
            if pal.slot is None:
                limit = MAX_V1_PALETTES if pal.vdp == 1 else MAX_V2_PALETTES
                slot = 0
                while (pal.vdp, slot) in explicit:
                    slot += 1
                if slot >= limit:
                    raise AssetError(f"palette '{pal.name}': no free VDP{pal.vdp} palette slot (max {limit})")
                explicit[(pal.vdp, slot)] = pal
                pal.slot = slot
        for pal in self.palettes.values():
            data = b"".join(struct.pack("<H", c) for c in pal.colours)
            kind = "v1_palette" if pal.vdp == 1 else "v2_palette"
            pname = getattr(pal, "alias", None) or ("pal_" + pal.name)
            self.unique(pname)
            self.assets.append(dict(name=pname, kind=kind, id=pal.slot, w=len(pal.colours), h=0, vram=0, data=data,
                                    rate=0, loop=0, pal=None,
                                    defines={"PAL": pal.slot, "COLOURS": len(pal.colours)}))

    def run(self):
        for e in self.cfg.get("sheet", []):
            self.add_sheet(e)
        for e in self.cfg.get("tileset", []):
            self.add_tileset(e)
        for e in self.cfg.get("tilemap", []):
            self.add_tilemap(e)
        for e in self.cfg.get("sample", []):
            self.add_sample(e)
        self.add_palette_slots()
        if not self.assets:
            raise AssetError("no assets defined")

    # -- output
    def write(self, out_dir):
        os.makedirs(out_dir, exist_ok=True)
        h = ["// Generated by tools/mkassets.py - do not edit.",
             "#pragma once", '#include "tri_assets.h"', ""]
        c = ["// Generated by tools/mkassets.py - do not edit.",
             '#include "assets.h"', ""]
        for i, a in enumerate(self.assets):
            up = a["name"].upper()
            h.append(f"#define ASSET_{up} {i}")
            for k, v in a["defines"].items():
                h.append(f"#define ASSET_{up}_{k} {v}")
            if a["pal"] is not None:
                h.append(f"#define ASSET_{up}_PAL {a['pal'].slot}")
            h.append("")
        h.append(f"#define ASSET_COUNT {len(self.assets)}")
        h.append("extern const tri_asset_t game_assets[ASSET_COUNT];")
        h.append("")
        for a in self.assets:
            c.append(f"static const uint8_t asset_{a['name']}_data[{max(1, len(a['data']))}] = {{")
            d = a["data"] or b"\0"
            for j in range(0, len(d), 16):
                c.append("\t" + ",".join(f"0x{b:02x}" for b in d[j:j + 16]) + ",")
            c.append("};")
        c.append("")
        c.append(f"const tri_asset_t game_assets[ASSET_COUNT] = {{")
        for a in self.assets:
            pal = a["pal"].slot if a["pal"] is not None else 0
            c.append(f"\t{{ /* {a['name']} */ {KIND[a['kind']]}, {a['id']}, {a['w']}, {a['h']}, 0x{a['vram']:x}, "
                     f"{len(a['data'])}, asset_{a['name']}_data, {a['rate']}, 0x{a['loop']:x}u, {pal} }},")
        c.append("};")
        with open(os.path.join(out_dir, "assets.h"), "w") as f:
            f.write("\n".join(h) + "\n")
        with open(os.path.join(out_dir, "assets.c"), "w") as f:
            f.write("\n".join(c) + "\n")

    def summary(self):
        lines = [f"mkassets: {len(self.assets)} assets, VDP1 SRAM {self.v1_used}/{VDP1_VRAM_SIZE} B + PSRAM {self.p1_used} B, "
                 f"VDP2 SRAM {self.v2_used}/{VDP2_VRAM_USABLE} B + PSRAM {self.p2_used} B, flash {sum(len(a['data']) for a in self.assets)} B"]
        if self.v2_used > VDP2_TEXT_AREA:
            lines.append(f"  note: VDP2 usage passes 0x{VDP2_TEXT_AREA:x}, the SDK's default text area; "
                         "place the text with tri_text_init_at() if the game prints text")
        for a in self.assets:
            vd = "" if a["kind"].endswith("palette") else (f" @PSRAM+0x{a['vram'] - VRAM_PSRAM:06x}" if a["vram"] >= VRAM_PSRAM else f" @0x{a['vram']:05x}")
            lines.append(f"  {a['kind']:10s} {a['name']:20s} id {a['id']:2d} {len(a['data']):7d} B{vd}")
        return "\n".join(lines)


def main(argv):
    quiet = "--quiet" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    try:
        b = Build(args[0], quiet)
        b.run()
        b.write(args[1])
    except AssetError as e:
        print(f"mkassets: error: {e}", file=sys.stderr)
        return 1
    if not quiet:
        print(b.summary())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
