############################################################################
# apps/games/thunderbolt/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# Thunderbolt, TRI's WW2 side-scroller, on the FPC: TRI's game.c as it is,
# on the game SDK's TRI runtime (apps/graphics/fpcgame/tri). The assets
# (assets/: PNG sheets and tiles, Tiled CSV maps, WAV samples) become
# assets.c / assets.h at build time, through TRI's own tools/mkassets.py.

PROGNAME  = thunderbolt
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_GAMES_THUNDERBOLT)

MAINSRC   = game.c
CSRCS     = assets.c

CFLAGS   += ${INCDIR_PREFIX}assets ${INCDIR_PREFIX}.
game.c_CFLAGS += -Wno-unused-variable -Wno-unused-function

# a fresh start at every run, as from TRI's ROM (tri_sdk.h, TRI_GAME): the
# game's statics in one section each, in the file's order
game.c_CFLAGS += -fno-data-sections -fno-toplevel-reorder

MKASSETS  = $(APPDIR)/graphics/fpcgame/tri/mkassets.py

assets.c assets.h: assets/assets.toml $(wildcard assets/*.png assets/*.csv assets/*.wav) $(MKASSETS)
	$(Q) python3 $(MKASSETS) assets/assets.toml .

context:: assets.c

distclean::
	$(call DELFILE, assets.c)
	$(call DELFILE, assets.h)

# The same for a loadable module's compile (it reads _CELFFLAGS)

$(foreach f,$(MAINSRC) $(CSRCS),$(eval $(f)_CELFFLAGS += $($(f)_CFLAGS)))

include $(APPDIR)/Application.mk
