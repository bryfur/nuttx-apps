############################################################################
# apps/games/trigames/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# TRI's other games, as they are (TRI's firmware/games/demo, bench, synth),
# on the game SDK's TRI runtime (apps/graphics/fpcgame/tri)

PROGNAME  = skyrocks spritebench fmsynth
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_GAMES_TRIGAMES)

MAINSRC   = skyrocks.c spritebench.c fmsynth.c

skyrocks.c_CFLAGS    += -Wno-unused-variable -Wno-unused-function
spritebench.c_CFLAGS += -Wno-unused-variable -Wno-unused-function
fmsynth.c_CFLAGS     += -Wno-unused-variable -Wno-unused-function

# a fresh start at every run, as from TRI's ROM (tri_sdk.h, TRI_GAME): each
# game's statics in one section each, in the file's order
skyrocks.c_CFLAGS    += -fno-data-sections -fno-toplevel-reorder
spritebench.c_CFLAGS += -fno-data-sections -fno-toplevel-reorder
fmsynth.c_CFLAGS     += -fno-data-sections -fno-toplevel-reorder

include $(APPDIR)/Application.mk
