############################################################################
# apps/testing/dcptest/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = dcptest
PRIORITY  = 100
STACKSIZE = 8192
MODULE    = $(CONFIG_TESTING_DCPTEST)

MAINSRC = dcptest_main.c
ASRCS   = dcptest_dcp.S dcptest_nomask.S dcptest_flags.S

# The coprocessor's helpers are the arch's own source, built here under
# names of their own (dcpt_*, dcpn_*)

DCP_DIR = $(TOPDIR)$(DELIM)arch$(DELIM)arm$(DELIM)src$(DELIM)rp23xx
AFLAGS += ${INCDIR_PREFIX}$(DCP_DIR)
CFLAGS += ${INCDIR_PREFIX}$(DCP_DIR)

# The reference: libgcc's double-precision routines and newlib's sqrt,
# their symbols renamed soft_*

SOFTDF = $(CURDIR)$(DELIM)softdf.o
LDLIBS += $(SOFTDF)

include $(APPDIR)/Application.mk

$(SOFTDF): softdf.sh
	$(Q) sh softdf.sh "$(CC) $(ARCHCPUFLAGS)" "$(CROSSDEV)" $@

$(PROGLIST): $(SOFTDF)

clean::
	$(call DELFILE, $(SOFTDF))
