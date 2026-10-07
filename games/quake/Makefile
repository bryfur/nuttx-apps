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
GAMESRCS := $(addprefix $(QG)/,$(GAMESRCS)) fpc_drawq.c fpc_gpu3d.c fpc_glq.c
GAMESRCS += fpc_sio.c

MAINSRC = fpc_quake.c

# -fno-short-enums: enums as wide as an int, as on the game's own
# platforms (arm-none-eabi makes them a byte where it can): qboolean holds
# a flag bit, and the model and sprite headers read from the files have
# enums in them

GAMEFLAGS = -include fpc_quake.h -fno-short-enums -std=gnu17

# The M33's FPU is single precision: a constant like 0.5 made the float
# arithmetic around it double, in software (7 % of a frame)

GAMEFLAGS += -fsingle-precision-constant
GAMEFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable
GAMEFLAGS += -Wno-unused-function -Wno-pointer-sign -Wno-dangling-else
GAMEFLAGS += -Wno-misleading-indentation -Wno-parentheses -Wno-format
GAMEFLAGS += -Wno-strict-prototypes -Wno-shadow -Wno-undef

# A loadable module (/mnt/sd0/bin/quake): its code and variables are the
# loader's for the time the game runs, nothing in the firmware's memory
# the rest of the time.  The game's objects go into the module itself, not
# into libapps.a, and the module link takes nothing from libapps.a (it took
# NSH's cmd_alias for the game's): what the game calls outside itself is
# the firmware's, resolved by the loader.  They get GAMEFLAGS; fpc_quake.c,
# which allocates the game's heap, does not.

ifeq ($(CONFIG_GAMES_QUAKE),m)

# A loadable module: the game linked into it (LDLIBS), not into libapps

GAMEOBJS = $(GAMESRCS:%=$(PREFIX)%$(SUFFIX)$(OBJEXT))
LDLIBS = $(GAMEOBJS)
$(foreach f,$(GAMESRCS),$(eval $(f)_CELFFLAGS += $(GAMEFLAGS) -MMD -MP -MF $(f)$$(SUFFIX)$(OBJEXT).d))

include $(APPDIR)/Application.mk

$(GAMEOBJS): $(PREFIX)%.c$(SUFFIX)$(OBJEXT): %.c
	$(call ELFCOMPILE, $<, $@)

$(PROGLIST): $(GAMEOBJS)

LDLIBS = $(GAMEOBJS) $(COMPILER_RT_LIB)

-include $(GAMEOBJS:=.d)

clean::
	$(Q) rm -f $(GAMEOBJS) $(GAMEOBJS:=.d)

else

# Built in (the simulator): the game's files as any app's, its globals
# that NSH and Doom have too renamed

QRENAME  = cmd_alias cmd_source cmd_wait deathmatch gammatable nomonsters
QRENAME += onground startepisode timelimit
CSRCS = $(GAMESRCS)
$(foreach f,$(GAMESRCS),$(eval $(f)_CFLAGS += $(GAMEFLAGS) $(foreach n,$(QRENAME),-D$(n)=quake_$(n))))

include $(APPDIR)/Application.mk

endif
