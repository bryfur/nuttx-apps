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

# As modules they take the library from the firmware (its symbol table),
# never libapps' objects, which are not built for a module's long calls
# (a module in the PSRAM is out of a branch's reach of the flash)

LDLIBS = $(COMPILER_RT_LIB)
