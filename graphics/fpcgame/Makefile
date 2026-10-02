############################################################################
# apps/graphics/fpcgame/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# The SDK (a library in libapps), the TRI games' runtime on it (tri/), and
# fgdemo, its demonstration and test, and fgbench, the sprites a frame

CSRCS  = fg_core.c fg_gfx.c fg_audio.c fg_audio_engine.c

ifeq ($(CONFIG_FPCGAME_TRI),y)
CSRCS += tri_gfx.c tri_text.c tri_audio.c tri_fpc.c
VPATH += :tri
endif

ifeq ($(CONFIG_FPCGAME_DEMO),y)
PROGNAME  = fgdemo fgbench
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_FPCGAME_DEMO)
MAINSRC   = fg_demo.c fg_bench.c
endif

# TRI's engine and SDK files come as they are: their warnings stay theirs
# (the SDK's are found through VPATH: their names here are tri/...)
fg_audio_engine.c_CFLAGS += -Wno-unused-function
tri/tri_gfx.c_CFLAGS += -Wno-shadow
tri/tri_text.c_CFLAGS += -Wno-shadow

# TRI's SDK files start fresh with each game (tri/tri_sdk.h, TRI_STATE): their
# statics in one section each, in the files' order
TRI_FRESH = -fno-data-sections -fno-toplevel-reorder
tri/tri_gfx.c_CFLAGS += $(TRI_FRESH)
tri/tri_text.c_CFLAGS += $(TRI_FRESH)
tri/tri_audio.c_CFLAGS += $(TRI_FRESH)

include $(APPDIR)/Application.mk
