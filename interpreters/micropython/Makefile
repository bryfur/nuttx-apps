############################################################################
# apps/interpreters/micropython/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# MicroPython for NuttX.  The source is the FPC's fork (1.29.0 and the
# FPC's changes as commits), the superproject's submodule
# third_party/micropython, reached through the link micropython/
# (FPC_LINK, apps/Make.defs).  MicroPython's generated headers
# (the qstrs, the module and root pointer tables, the version) are made
# by its own rules (mpy.mk) with this build's compiler and flags, then
# its sources are compiled as this program's.
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_INTERPRETERS_MICROPYTHON_PROGNAME)
PRIORITY  = $(CONFIG_INTERPRETERS_MICROPYTHON_PRIORITY)
STACKSIZE = $(CONFIG_INTERPRETERS_MICROPYTHON_STACKSIZE)
MODULE    = $(CONFIG_INTERPRETERS_MICROPYTHON)

# The release the fork is based on (the version MicroPython reports)

MPY_VERSION = 1.29.0
MPY         = micropython
MPY_BUILD   = build
MPY_GENHDR  = $(MPY_BUILD)/genhdr/qstrdefs.generated.h

# The sources: the core, the modules of extmod this port has, the shared
# pieces (the REPL, readline), the port

MPY_PY      = $(filter-out $(MPY)/py/emitnative.c,$(wildcard $(MPY)/py/*.c))

MPY_EXTMOD  = modjson.c modre.c modrandom.c modselect.c modheapq.c
MPY_EXTMOD += modbinascii.c moductypes.c modplatform.c modos.c modtime.c
MPY_EXTMOD += modvfs.c vfs.c vfs_posix.c vfs_posix_file.c vfs_reader.c
MPY_EXTMOD += machine_mem.c

MPY_SHARED  = readline/readline.c runtime/pyexec.c runtime/gchelper_generic.c
MPY_SHARED += runtime/sys_stdio_mphal.c timeutils/timeutils.c

MPY_SRCS    = $(MPY_PY) $(addprefix $(MPY)/extmod/,$(MPY_EXTMOD))
MPY_SRCS   += $(addprefix $(MPY)/shared/,$(MPY_SHARED))

PORT_SRCS   = mphalport.c qstrindex.c
ifeq ($(CONFIG_INTERPRETERS_MICROPYTHON_FPC),y)
PORT_SRCS  += modfpc.c
endif

# hashlib.sha256: /dev/crypto (the board's SHA-256 accelerator) or sha2.c
ifeq ($(CONFIG_CRYPTO),y)
PORT_SRCS  += modhashlib_nuttx.c
endif

MAINSRC     = main.c

# The agent (docs/agent.md): the _agent module, the agent package frozen
# as bytecode (manifest.py, by mpy.mk: frozen_agent.c), the agent command

AGENT_PY    = $(sort $(wildcard agent/*.py))
ifeq ($(CONFIG_INTERPRETERS_MICROPYTHON_AGENT),y)
PORT_SRCS  += modagent.c agent_http.c agent_proc.c agent_search.c
PROGNAME   += agent
MAINSRC    += agent_main.c
AGENT_SRCS  = frozen_agent.c
MPY_FROZEN  = FROZEN_MANIFEST_PY=manifest.py FROZEN_C=frozen_agent.c
MPY_FROZEN += FROZEN_DEPS="$(AGENT_PY)"
endif

CSRCS       = $(PORT_SRCS) $(AGENT_SRCS) $(notdir $(MPY_SRCS))

# MicroPython's sources by name (no two have the same), found in their
# folders: so the dependencies on headers (the generated ones) are right

MPY_DIRS    = $(MPY)/py $(MPY)/extmod $(MPY)/shared/readline
MPY_DIRS   += $(MPY)/shared/runtime $(MPY)/shared/timeutils
VPATH      += $(MPY_DIRS)
DEPPATH    += $(addprefix --dep-path ,$(MPY_DIRS))

# Flags: MicroPython's headers, the generated ones; its names that NuttX's
# libraries have too

MPY_CFLAGS  = ${INCDIR_PREFIX}$(CURDIR) ${INCDIR_PREFIX}$(CURDIR)/$(MPY)
MPY_CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/$(MPY_BUILD)
MPY_CFLAGS += -Dreadline=mp_readline -Dreadline_init=mp_readline_init
MPY_CFLAGS += -Dreadline_init0=mp_readline_init0
MPY_CFLAGS += -Dreadline_process_char=mp_readline_process_char
MPY_CFLAGS += -Dreadline_note_newline=mp_readline_note_newline
MPY_CFLAGS += -Dreadline_push_history=mp_readline_push_history
MPY_CFLAGS += -fno-strict-aliasing -DFFCONF_H=\"lib/oofatfs/ffconf.h\"

CFLAGS     += $(MPY_CFLAGS)
CFLAGS     += -DMPY_PYTHON_MAIN=$(subst -,_,$(subst ",,$(CONFIG_INTERPRETERS_MICROPYTHON_PROGNAME)))_main
CFLAGS     += -Wno-shadow -Wno-undef -Wno-unused-function
CFLAGS     += -Wno-unused-but-set-variable -Wno-missing-field-initializers
CFLAGS     += -Wno-sign-compare -Wno-implicit-fallthrough
CFLAGS     += -Wno-strict-prototypes -Wno-array-bounds -Wno-format

# The source: the link to the submodule (made again if it is not one)

.PHONY: mpy_link

mpy_link:
	$(call FPC_LINK,$(MPY),micropython)

# The generated headers, by MicroPython's rules (mpy.mk), with this build's
# preprocessor and flags.  Run at every build: it only does what the
# sources changed (a new qstr, a new module).

.PHONY: mpy_genhdr

mpy_genhdr: mpy_link
	$(shell mkdir -p $(MPY_BUILD))$(file >$(MPY_BUILD)/cflags.new,$(CFLAGS))
	$(Q) cmp -s $(MPY_BUILD)/cflags.new $(MPY_BUILD)/cflags || \
		mv $(MPY_BUILD)/cflags.new $(MPY_BUILD)/cflags
	$(Q) rm -f $(MPY_BUILD)/cflags.new
	$(Q) MICROPY_GIT_TAG=v$(MPY_VERSION) MICROPY_GIT_HASH=nuttx \
		SOURCE_DATE_EPOCH=1767225600 \
		$(MAKE) -s -f mpy.mk MICROPYTHON_TOP=$(MPY) BUILD=$(MPY_BUILD) \
		CC="$(CC)" CPP="$(CC) -E" \
		SRC_QSTR_PORT="$(MAINSRC) $(PORT_SRCS) $(MPY_SRCS)" \
		NUTTX_CONFIG_H=$(TOPDIR)/include/nuttx/config.h $(MPY_FROZEN)

context:: mpy_genhdr

depend:: mpy_genhdr

distclean::
	$(call DELFILE, frozen_agent.c)
	$(call DELDIR, $(MPY))
	$(call DELDIR, $(MPY_BUILD))

include $(APPDIR)/Application.mk

# Every object after the generated headers

$(OBJS) $(MAINCOBJ): | mpy_genhdr
