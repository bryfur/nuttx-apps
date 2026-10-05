############################################################################
# apps/graphics/fpcgame/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# The SDK (a library in libapps), the TRI games' runtime on it (tri/), and
# fgdemo, its demonstration and test, and fgbench, the sprites a frame

CSRCS  = fg_core.c fg_gfx.c fg_3d.c fg_audio.c fg_audio_engine.c

ifeq ($(CONFIG_FPCGAME_TRI),y)
CSRCS += tri_gfx.c tri_text.c tri_audio.c tri_fpc.c
VPATH += :tri
endif

# The SDK goes into the games, which are loadable modules: compiled as
# module code (long calls: a module in the PSRAM is more than a branch's
# 16 MB from the firmware in the flash), archived for their links

ifeq ($(CONFIG_MODULES),y)
MODULE    = m
endif

ifneq ($(CONFIG_FPCGAME_DEMO),)
PROGNAME  = fgdemo fgbench fg3d
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_FPCGAME_DEMO)
MAINSRC   = fg_demo.c fg_bench.c fg_3ddemo.c
endif

# TRI's engine and SDK files come as they are: their warnings stay theirs
# (the SDK's are found through VPATH: their names here are tri/...)
fg_audio_engine.c_CFLAGS += -Wno-unused-function
tri/tri_gfx.c_CFLAGS += -Wno-shadow
tri/tri_text.c_CFLAGS += -Wno-shadow

# The 3D effects' rows from floats: the same on the board as in the
# simulator (no fused multiply-adds, which x86-64 has not), pixel for pixel
fg_3d.c_CFLAGS += -ffp-contract=off

# TRI's SDK files start fresh with each game (tri/tri_sdk.h, TRI_STATE): their
# statics in one section each, in the files' order
TRI_FRESH = -fno-data-sections -fno-toplevel-reorder
tri/tri_gfx.c_CFLAGS += $(TRI_FRESH)
tri/tri_text.c_CFLAGS += $(TRI_FRESH)
tri/tri_audio.c_CFLAGS += $(TRI_FRESH)

# The same for a loadable module's compile (it reads _CELFFLAGS)

$(foreach f,$(CSRCS) $(addprefix tri/,$(filter tri_%,$(CSRCS))),$(eval $(f)_CELFFLAGS += $($(f)_CFLAGS)))

include $(APPDIR)/Application.mk
