############################################################################
# apps/audioutils/fpcmixer/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = mixer
PRIORITY  = $(CONFIG_AUDIOUTILS_FPCMIXER_PRIORITY)
STACKSIZE = $(CONFIG_AUDIOUTILS_FPCMIXER_STACKSIZE)
MODULE    = $(CONFIG_AUDIOUTILS_FPCMIXER)

MAINSRC   = fpcmixer_main.c

include $(APPDIR)/Application.mk
