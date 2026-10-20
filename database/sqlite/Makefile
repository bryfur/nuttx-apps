############################################################################
# apps/database/sqlite/Makefile
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
#
############################################################################

include $(APPDIR)/Make.defs

SQLITEDIR=$(APPDIR)/database/sqlite
BUILDDIR=$(SQLITEDIR)/build

CSRCS += ${BUILDDIR}/sqlite3.c

CFLAGS += ${INCDIR_PREFIX}$(SQLITEDIR)
CFLAGS += ${DEFINE_PREFIX}_HAVE_SQLITE_CONFIG_H
CFLAGS += ${DEFINE_PREFIX}NDEBUG
CFLAGS += -Wno-unused-variable -Wno-undef -Wno-unused-function -Wno-shadow

# FPC: no mutexes (a program's data is in the PSRAM, which has no exclusive
# access: a lock there never returns), no WAL and no mmap (FAT files cannot
# be mapped), temporary tables in the RAM, no extensions loaded
CFLAGS += ${DEFINE_PREFIX}SQLITE_THREADSAFE=0
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_WAL
CFLAGS += ${DEFINE_PREFIX}SQLITE_MAX_MMAP_SIZE=0
CFLAGS += ${DEFINE_PREFIX}SQLITE_TEMP_STORE=3
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_LOAD_EXTENSION
CFLAGS += ${DEFINE_PREFIX}SQLITE_DEFAULT_MEMSTATUS=0

# The shell: no pipes and no system() (NuttX has no fork())
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_POPEN
CFLAGS += ${DEFINE_PREFIX}SQLITE_NOHAVE_SYSTEM

ifneq ($(CONFIG_UTILS_SQLITE),)
PROGNAME  = sqlite3
PRIORITY  = 100
STACKSIZE = ${CONFIG_UTILS_SQLITE_STACKSIZE}
MODULE = $(CONFIG_UTILS_SQLITE)
MAINSRC = sqlite3_main.c
endif

# FPC: SQLite's sources are the superproject's third_party/sqlite (a fork,
# branch fpc), reached through the link sqlite/ (FPC_LINK, apps/Make.defs);
# upstream's download named a tag that does not exist ("vesion-").  The
# amalgamation (sqlite3.c, sqlite3.h, shell.c) is made once in build/ by
# SQLite's own configure, with the host's compiler (CC is the target's
# here); the sqlite_cfg.h it writes there is the host's, so it goes, and
# sqlite3.c finds this directory's (NuttX's).

sqlite:
	$(call FPC_LINK,sqlite,sqlite)

${BUILDDIR}/sqlite3.c: | sqlite
	$(Q) mkdir -p ${BUILDDIR} && \
	cd ${BUILDDIR} && \
	echo "Generate SQLite amalgamation version" && \
	env -u CFLAGS -u LDFLAGS -u CPPFLAGS -u MAKEFLAGS -u MFLAGS CC=cc ${SQLITEDIR}/sqlite/configure 1> /dev/null && \
	env -u CFLAGS -u LDFLAGS -u CPPFLAGS -u MAKEFLAGS -u MFLAGS $(MAKE) CC=cc sqlite3.c shell.c 1> /dev/null && \
	rm -f sqlite_cfg.h

${BUILDDIR}/shell.c ${BUILDDIR}/sqlite3.h: ${BUILDDIR}/sqlite3.c

sqlite3_main.c: ${BUILDDIR}/shell.c

context:: ${BUILDDIR}/sqlite3.c

clean::
	$(call DELFILE, ${BUILDDIR}/*.o)

distclean::
	$(call DELDIR, ${BUILDDIR})
	$(call DELFILE, sqlite)

include $(APPDIR)/Application.mk
