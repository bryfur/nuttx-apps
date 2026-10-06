############################################################################
# apps/netutils/netcop/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# netcopd and the library (netutils/netcop.h), on the protocol's portable
# core in fpc/netcop/core (docs/netcop.md)
#
############################################################################

include $(APPDIR)/Make.defs

NCCORE = $(APPDIR)/../netcop/core

CSRCS  = nc_lib.c nc_codec.c nc_msg.c nc_link.c nc_host.c
CSRCS += ncd_sock.c ncd_hid.c
VPATH += :$(NCCORE)
DEPPATH += --dep-path $(NCCORE)

# /dev/vinput's records (fpc/drivers/netcop)

CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../drivers/netcop

PROGNAME  = netcopd
PRIORITY  = $(CONFIG_NETCOP_PRIORITY)
STACKSIZE = $(CONFIG_NETCOP_STACKSIZE)
MODULE    = n
MAINSRC   = netcopd.c

include $(APPDIR)/Application.mk
