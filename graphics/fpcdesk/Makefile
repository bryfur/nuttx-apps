############################################################################
# apps/graphics/fpcdesk/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_FPCDESK_PROGNAME)
PRIORITY  = $(CONFIG_FPCDESK_PRIORITY)
STACKSIZE = $(CONFIG_FPCDESK_STACKSIZE)
MODULE    = $(CONFIG_GRAPHICS_FPCDESK)

MAINSRC   = desk_main.c
CSRCS     = desk_gputest.c desk_draw.c desk_panel.c desk_apps.c desk_fonts.c
CSRCS    += desk_term.c desk_vt.c desk_files.c desk_edit.c desk_monitor.c
CSRCS    += desk_calc.c desk_tetris.c desk_solitaire.c desk_breakout.c
CSRCS    += desk_ui.c desk_theme.c desk_settings.c desk_log.c
CSRCS    += desk_wallimg.c
CSRCS    += desk_game.c
CSRCS    += desk_image.c desk_viewer.c desk_paint.c
CSRCS    += desk_kit.c desk_kit_paint.c desk_kit_dialog.c desk_kit_surface.c
CSRCS    += desk_kitdemo.c

ifeq ($(CONFIG_FPCDESK_SOUND),y)
CSRCS    += desk_sound.c desk_volume.c
endif

ifeq ($(CONFIG_FPCDESK_MUSIC),y)
CSRCS    += desk_music.c
ifeq ($(CONFIG_AUDIOUTILS_LIBMAD),y)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/audioutils/libmad/libmad
endif
endif

ifeq ($(CONFIG_FPCDESK_DESKCTL),y)
PROGNAME  += deskctl
PRIORITY  += 100
STACKSIZE += 4096
MAINSRC   += deskctl_main.c
endif

include $(APPDIR)/Application.mk
