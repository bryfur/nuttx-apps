############################################################################
# apps/games/quake/Makefile
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
############################################################################

include $(APPDIR)/Make.defs

# Program options

MODULE    = $(CONFIG_GAMES_QUAKE)
PRIORITY  = $(CONFIG_GAMES_QUAKE_PRIORITY)
STACKSIZE = $(CONFIG_GAMES_QUAKE_STACKSIZE)
PROGNAME  = quake

QG = quakegeneric

CFLAGS += ${INCDIR_PREFIX}$(QG)
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../drivers/gpu

# The game (quakegeneric: WinQuake with a small platform layer), its null
# CD, input, sound, system and video drivers adapted to this board

GAMESRCS  = cd_null.c chase.c cl_demo.c cl_input.c cl_main.c cl_parse.c
GAMESRCS += cl_tent.c cmd.c common.c console.c crc.c cvar.c d_edge.c
GAMESRCS += d_fill.c d_init.c d_modech.c d_part.c d_polyse.c d_scan.c
GAMESRCS += d_sky.c d_sprite.c d_surf.c d_vars.c d_zpoint.c draw.c
GAMESRCS += host_cmd.c host.c in_null.c keys.c mathlib.c menu.c model.c
GAMESRCS += net_loop.c net_main.c net_none.c net_vcr.c nonintel.c
GAMESRCS += pr_cmds.c pr_edict.c pr_exec.c r_aclip.c r_alias.c r_bsp.c
GAMESRCS += r_draw.c r_edge.c r_efrag.c r_light.c r_main.c r_misc.c
GAMESRCS += r_part.c r_sky.c r_sprite.c r_surf.c r_vars.c sbar.c screen.c
GAMESRCS += snd_null.c sv_main.c sv_move.c sv_phys.c sv_user.c sys_null.c
GAMESRCS += vid_null.c view.c wad.c world.c zone.c quakegeneric.c
GAMESRCS := $(addprefix $(QG)/,$(GAMESRCS))

MAINSRC = fpc_quake.c

# The game's objects linked into one (quake.ld) with a single data and a
# single bss block, in the PSRAM, which fpc_quake.c puts back as built at
# every start (in a flat build they would otherwise keep the last game's
# values); every allocation of theirs from the game's own heap (fpc_quake.h)

GAMEBLOCK = quake_game$(OBJEXT)
EXTOBJS  += $(GAMEBLOCK)
GAMELD    = quake.ld

# -fno-short-enums: enums as wide as an int, as on the game's own
# platforms (arm-none-eabi makes them a byte where it can): qboolean holds
# a flag bit, and the model and sprite headers read from the files have
# enums in them

GAMEFLAGS = -include fpc_quake.h -fno-short-enums
GAMEFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable
GAMEFLAGS += -Wno-unused-function -Wno-pointer-sign -Wno-dangling-else
GAMEFLAGS += -Wno-misleading-indentation -Wno-parentheses -Wno-format
GAMEFLAGS += -Wno-strict-prototypes -Wno-shadow -Wno-undef

include $(APPDIR)/Application.mk

GAMEOBJS = $(GAMESRCS:%=$(PREFIX)%$(SUFFIX)$(OBJEXT))

$(GAMEOBJS): $(PREFIX)%.c$(SUFFIX)$(OBJEXT): %.c
	$(call COMPILE, $<, $@, $(GAMEFLAGS) -MMD -MP -MF $@.d)

# Only what fpc_quake.c calls stays global: the game's names (cmd_alias,
# gammatable, ...) would clash with NSH's and Doom's in a flat build

GAMEKEEP  = QG_Create QG_Tick Sys_FloatTime
GAMEKEEP += __quake_data_start __quake_data_end __quake_bss_start
GAMEKEEP += __quake_bss_end

$(GAMEBLOCK): $(GAMEOBJS) $(GAMELD)
	$(Q) $(LD) -r -T $(GAMELD) -o $@ $(GAMEOBJS)
	$(Q) $(OBJCOPY) $(addprefix --keep-global-symbol=,$(GAMEKEEP)) $@

-include $(GAMEOBJS:=.d)

clean::
	$(call DELFILE, $(GAMEBLOCK))
	$(Q) rm -f $(GAMEOBJS) $(GAMEOBJS:=.d)
