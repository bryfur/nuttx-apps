// Games in ROM: every game is a complete image linked at its own 1 MB flash
// slot, chain-loaded by the OS through the boot ROM, and it owns the whole
// CPU while it runs. Returning to the menu is a reboot into the OS image at
// the start of flash. The same slots will be filled from SD later.
//
// The OS finds a game by its ROM header: a small struct stamped by TRI_GAME()
// into the `.tri_rom_header` section, which tri_add_game() pins with a KEEP
// right after the embedded block so it survives --gc-sections and lands in
// the first 4 KB of the slot.
#pragma once
#include <stdint.h>

#define TRI_SLOT_BASE  0x10100000u      // slot 0; the OS lives below it
#define TRI_SLOT_SIZE  0x00100000u
#define TRI_SLOTS      15               // 16 MB flash

#define TRI_ROM_MAGIC "TRI2350-GAME"
#define TRI_ROM_HEADER_VERSION 2        // 1: name only; 2: adds the video mode
#define TRI_ROM_HEADER_SCAN 4096        // the OS looks for the magic in the first 4 KB of a slot

// "Use the console's video mode" in a ROM header / tri_game_t. Not an ISA
// value: the SDK resolves it to the stored console setting before anything is
// sent to the VDPs.
#define TRI_VIDEO_ANY 0xFF

typedef struct {
	char magic[12];                     // TRI_ROM_MAGIC, not zero-terminated
	uint32_t version;                   // TRI_ROM_HEADER_VERSION
	char name[32];                      // zero-terminated, shown by the menu
	uint32_t sdk_version;               // TRI_SDK_VERSION the game was built with
	uint8_t  video_mode;                // TRI_VIDEO_* the game is designed for, or TRI_VIDEO_ANY (v2+)
	uint8_t  reserved[3];
} tri_rom_header_t;
