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
CSRCS    += desk_game.c desk_games.c
CSRCS    += desk_image.c desk_viewer.c desk_paint.c
CSRCS    += desk_kit.c desk_kit_paint.c desk_kit_dialog.c desk_kit_surface.c
CSRCS    += desk_kitdemo.c
CSRCS    += desk_fx.c
CSRCS    += desk_alarm.c desk_clock.c desk_calendar.c
CSRCS    += desk_num.c desk_sheet.c desk_sheet_eng.c desk_sheet_io.c

ifeq ($(CONFIG_FPCDESK_CLIENTS),y)
CSRCS    += desk_client.c desk_program.c
endif

ifeq ($(CONFIG_FPCDESK_PYTHON),y)
CSRCS    += desk_python.c desk_ide.c desk_pyexamples.c
endif

# Applications that are programs of their own (apps/graphics/deskapps):
# built into the desktop, only their names and icons (desk_program.c)

ifeq ($(CONFIG_FPCDESK_PROGRAMS),y)
DESK_PROGRAMS  = desk_calc.c desk_monitor.c desk_tetris.c desk_breakout.c
DESK_PROGRAMS += desk_solitaire.c desk_kitdemo.c desk_log.c desk_files.c
DESK_PROGRAMS += desk_term.c desk_sheet.c desk_chess.c desk_viewer.c
DESK_PROGRAMS += desk_paint.c
$(foreach f,$(DESK_PROGRAMS),$(eval $(f)_CFLAGS += -DDESK_PROGRAM_STUB))
endif

ifeq ($(CONFIG_FPCDESK_SOUND),y)
CSRCS    += desk_sound.c desk_volume.c
endif

ifeq ($(CONFIG_FPCDESK_MUSIC),y)
CSRCS    += desk_music.c
ifeq ($(CONFIG_AUDIOUTILS_LIBMAD),y)
CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/audioutils/libmad/libmad
endif
endif

ifeq ($(CONFIG_FPCDESK_BENCH),y)
CSRCS    += desk_bench.c desk_bench_run.c desk_bench_demo.c
endif

ifeq ($(CONFIG_FPCDESK_DESKCTL),y)
PROGNAME  += deskctl
PRIORITY  += 100
STACKSIZE += 4096
MAINSRC   += deskctl_main.c
endif

ifeq ($(CONFIG_FPCDESK_CHESS),y)
CSRCS     += desk_chess.c desk_chess_engine.c desk_chess_pieces.c
PROGNAME  += chess
PRIORITY  += 100
STACKSIZE += 16384
MAINSRC   += desk_chess_main.c
endif

ifeq ($(CONFIG_FPCDESK_SHEET_CMD),y)
PROGNAME  += sheet
PRIORITY  += 100
STACKSIZE += 8192
MAINSRC   += sheet_main.c
endif

include $(APPDIR)/Application.mk

ifeq ($(CONFIG_FPCDESK_PYTHON),y)

# The IDE's examples: the MicroPython port's, made into C

PYEXAMPLES = $(sort $(wildcard $(APPDIR)/interpreters/micropython/examples/*.py))

desk_pyexamples.c: $(PYEXAMPLES) tools/mkpyexamples.py
	$(Q) python3 tools/mkpyexamples.py $(PYEXAMPLES) > $@.tmp
	$(Q) mv $@.tmp $@

context:: desk_pyexamples.c

depend:: desk_pyexamples.c

distclean::
	$(call DELFILE, desk_pyexamples.c)
endif
