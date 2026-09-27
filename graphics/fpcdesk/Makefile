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
CSRCS    += desk_ui.c desk_theme.c desk_widgets.c desk_settings.c desk_log.c

ifeq ($(CONFIG_FPCDESK_DESKCTL),y)
PROGNAME  += deskctl
PRIORITY  += 100
STACKSIZE += 4096
MAINSRC   += deskctl_main.c
endif

include $(APPDIR)/Application.mk
