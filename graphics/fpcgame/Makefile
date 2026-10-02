############################################################################
# apps/graphics/fpcgame/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# The SDK (a library in libapps), the TRI games' runtime on it (tri/), and
# fgdemo, its demonstration and test

CSRCS  = fg_core.c fg_gfx.c fg_audio.c fg_audio_engine.c

ifeq ($(CONFIG_FPCGAME_TRI),y)
CSRCS += tri_gfx.c tri_text.c tri_audio.c tri_fpc.c
VPATH += :tri
endif

ifeq ($(CONFIG_FPCGAME_DEMO),y)
PROGNAME  = fgdemo
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_FPCGAME_DEMO)
MAINSRC   = fg_demo.c
endif

# TRI's engine and SDK files come as they are: their warnings stay theirs
fg_audio_engine.c_CFLAGS += -Wno-unused-function
tri_gfx.c_CFLAGS += -Wno-shadow
tri_text.c_CFLAGS += -Wno-shadow

include $(APPDIR)/Application.mk
