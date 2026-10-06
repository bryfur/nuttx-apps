############################################################################
# apps/netutils/nctools/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# The network coprocessor's commands, on netutils/netcop.h (the library is
# the firmware's: a module links nothing else in)
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = wifi ifconfig curl wget ssh ntpdate host nc bt
PRIORITY  = $(CONFIG_NCTOOLS_PRIORITY)
STACKSIZE = $(CONFIG_NCTOOLS_STACKSIZE)
MODULE    = $(CONFIG_NETUTILS_NCTOOLS)
MAINSRC   = wifi.c ifconfig.c curl.c wget.c ssh.c ntpdate.c host.c nc.c bt.c

include $(APPDIR)/Application.mk
