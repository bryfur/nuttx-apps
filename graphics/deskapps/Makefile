############################################################################
# apps/graphics/deskapps/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# The desktop's applications as programs of their own (loadable modules in
# /mnt/sd0/bin, CONFIG_FPCDESK_PROGRAMS): each is the application's file in
# graphics/fpcdesk, built on its own with a main() that hands it to
# desk_program_main().  What it uses of the desktop (the drawing, the
# fonts, the theme, the kit, the runtime) is the firmware's, resolved by the
# loader: the module links nothing else in.
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = calc monitor tetris breakout solitaire widgets syslog
PROGNAME += files terminal sheet chess viewer paint
PRIORITY  = 100 100 100 100 100 100 100 100 100 100 100 100 100
STACKSIZE = 8192 8192 8192 8192 8192 8192 8192 8192 8192 8192 8192 8192
STACKSIZE += 8192
MODULE    = m
MAINSRC   = calc_main.c monitor_main.c tetris_main.c breakout_main.c
MAINSRC  += solitaire_main.c widgets_main.c syslog_main.c files_main.c
MAINSRC  += terminal_main.c sheet_main.c chess_main.c viewer_main.c
MAINSRC  += paint_main.c

CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/graphics/fpcdesk

include $(APPDIR)/Application.mk

LDLIBS = $(COMPILER_RT_LIB)
