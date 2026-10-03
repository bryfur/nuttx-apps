############################################################################
# apps/graphics/fpcgl/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# fpcgl, the 3D GPU's GL library (fpcgl.h), and fgldemo
#
############################################################################

include $(APPDIR)/Make.defs

CSRCS  = fgl_setup.c fgl_core.c fgl_tex.c fgl_ext.c

ifeq ($(CONFIG_MODULES),y)
MODULE    = m
endif

ifneq ($(CONFIG_FPCGL_DEMO),)
PROGNAME  = fgldemo
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_FPCGL_DEMO)
MAINSRC   = fgl_demo.c
endif

include $(APPDIR)/Application.mk
