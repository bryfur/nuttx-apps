############################################################################
# apps/devtools/xcc/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# xcc (xcc/: tyfkda's compiler, upstream 9eea241c, its changes marked FPC)
# as NuttX programs:
#
#   wcc    C to WebAssembly (WASI), one program: preprocessor, compiler and
#          linker
#
# Each program's objects are linked into one (wcc.blob.o, ld -r with
# xcc.ld) in which only its main and the markers around its variables
# stay global, so that nothing of it clashes with the rest of the
# firmware when it is built in (the simulator), and its variables can be
# put back as loaded before every run (xcc_nuttx.c).  It runs in
# xcc_run(): its memory, exit() and temporary files as a process's.
#
# The headers and C libraries the compiled programs need are made here
# too, with a build of the same compiler for this machine (host/), into
# sysroot/ (include/, lib/): what goes to CONFIG_DEVTOOLS_XCC_ROOT on the
# SD card (docs/xcc.md).

PROGNAME  = wcc
PRIORITY  = $(CONFIG_DEVTOOLS_XCC_PRIORITY)
STACKSIZE = $(CONFIG_DEVTOOLS_XCC_STACKSIZE)
MODULE    = $(CONFIG_DEVTOOLS_XCC)

MAINSRC = wcc_main.c

X = xcc/src
L = xcc/libsrc

WCC_SRCS  = $(wildcard $(X)/wcc/*.c) $(wildcard $(X)/cc/frontend/*.c)
WCC_SRCS += $(X)/cpp/preprocessor.c $(X)/cpp/pp_parser.c $(X)/cpp/macro.c
WCC_SRCS += $(X)/util/util.c $(X)/util/table.c $(X)/util/archive.c

XCC_INCS = -I$(X)/util -I$(X)/cc/frontend -I$(X)/cc/backend -I$(X)/cpp

# -fno-short-enums: xcc passes -1 for "any" in its enum TokenKind
# (match(-1)), which arm-none-eabi's byte-sized enums make 255

XCCFLAGS  = -std=gnu11 -D_DEFAULT_SOURCE -fno-common -fno-short-enums -w
XCCFLAGS += $(XCC_INCS)
XCCFLAGS += -include $(CURDIR)/xcc_nuttx.h
WCCFLAGS  = $(XCCFLAGS) -DXCC_TARGET_ARCH=XCC_ARCH_WASM

ifeq ($(CONFIG_DEVTOOLS_XCC),m)
XCC_CC = $(MODULECC) -c $(CELFFLAGS)
else
XCC_CC = $(CC) -c $(CFLAGS)
endif

WCC_OBJS = $(WCC_SRCS:%.c=%.wcc$(OBJEXT)) xcc_marks.c.wcc$(OBJEXT)
BLOB_KEEP = xcc_data_start xcc_data_end xcc_bss_start xcc_bss_end

%.wcc$(OBJEXT): %.c xcc_nuttx.h
	$(ECHO_BEGIN)"CC: $< "
	$(Q) $(XCC_CC) $(WCCFLAGS) $(if $(filter %/wcc.c,$<),-Dmain=xcc_wcc_main) $< -o $@
	$(ECHO_END)

xcc_marks.c.wcc$(OBJEXT): xcc_marks.c
	$(Q) $(XCC_CC) $< -o $@

wcc.blob$(OBJEXT): $(WCC_OBJS) xcc.ld
	$(ECHO_BEGIN)"LD: $@ "
	$(Q) $(LD) -r -T xcc.ld -o $@.tmp $(WCC_OBJS)
	$(Q) $(OBJCOPY) $(addprefix -G ,xcc_wcc_main $(BLOB_KEEP)) $@.tmp $@
	$(Q) rm -f $@.tmp
	$(ECHO_END)

ifeq ($(CONFIG_DEVTOOLS_XCC),m)

# A loadable module: the blob and the runtime linked into it, not into
# libapps.a

xcc_nuttx.m$(OBJEXT): xcc_nuttx.c xcc_nuttx.h
	$(Q) $(XCC_CC) $< -o $@

include $(APPDIR)/Application.mk

$(PROGLIST): wcc.blob$(OBJEXT) xcc_nuttx.m$(OBJEXT)

LDLIBS = wcc.blob$(OBJEXT) xcc_nuttx.m$(OBJEXT) $(COMPILER_RT_LIB)

else

CSRCS = xcc_nuttx.c
EXTOBJS = wcc.blob$(OBJEXT)

include $(APPDIR)/Application.mk

endif

# The sysroot: xcc's headers, and its WASI C library built by the host's
# wcc

HOSTDIR = host
SYSROOT = sysroot

HOSTWCC_SRCS = $(WCC_SRCS)
HOSTCFLAGS_XCC = -O2 -std=gnu11 -D_DEFAULT_SOURCE -w $(XCC_INCS)

$(HOSTDIR)/wcc: $(HOSTWCC_SRCS)
	$(ECHO_BEGIN)"HOSTCC: $@ "
	$(Q) mkdir -p $(HOSTDIR)
	$(Q) $(HOSTCC) $(HOSTCFLAGS_XCC) -DXCC_TARGET_ARCH=XCC_ARCH_WASM -o $@ $^
	$(ECHO_END)

WLIBC_SRCS = $(wildcard $(L)/math/*.c $(L)/misc/*.c $(L)/stdio/*.c \
                        $(L)/stdlib/*.c $(L)/string/*.c $(L)/_wasm/unistd/*.c)
WCRT0_SRCS = $(wildcard $(L)/_wasm/crt0/*.c)
WLIBC_OBJS = $(addprefix $(HOSTDIR)/wlibc/,$(notdir $(WLIBC_SRCS:.c=.o)))
WCRT0_OBJS = $(addprefix $(HOSTDIR)/wlibc/,$(notdir $(WCRT0_SRCS:.c=.o)))

vpath %.c $(sort $(dir $(WLIBC_SRCS) $(WCRT0_SRCS)))

$(HOSTDIR)/wlibc/%.o: %.c $(HOSTDIR)/wcc
	$(Q) mkdir -p $(HOSTDIR)/wlibc
	$(Q) $(HOSTDIR)/wcc -nostdinc -isystem xcc/include -c -o $@ $<

$(SYSROOT)/lib/wlibc.a: $(WLIBC_OBJS)
	$(Q) mkdir -p $(SYSROOT)/lib
	$(Q) python3 mkar.py $@ $^

$(SYSROOT)/lib/wcrt0.a: $(WCRT0_OBJS)
	$(Q) mkdir -p $(SYSROOT)/lib
	$(Q) python3 mkar.py $@ $^

# With the WebAssembly runtime's API (docs/wasm.md): #include <fpc_wasm.h>

FPC_WASM_H = $(wildcard $(APPDIR)/graphics/fpcwasm/sdk/fpc_wasm.h)

$(SYSROOT)/.include: $(shell find xcc/include -type f) $(FPC_WASM_H) Makefile
	$(Q) rm -rf $(SYSROOT)/include
	$(Q) mkdir -p $(SYSROOT)
	$(Q) cp -r xcc/include $(SYSROOT)/include
	$(Q) rm -rf $(SYSROOT)/include/mach $(SYSROOT)/include/mach-o
	$(if $(FPC_WASM_H),$(Q) cp $(FPC_WASM_H) $(SYSROOT)/include/)
	$(Q) touch $@

sysroot: $(SYSROOT)/lib/wlibc.a $(SYSROOT)/lib/wcrt0.a $(SYSROOT)/.include

.PHONY: sysroot

all:: sysroot

clean::
	$(call DELFILE, wcc.blob$(OBJEXT))
	$(call DELFILE, xcc_nuttx.m$(OBJEXT))
	$(Q) find xcc -name '*.wcc$(OBJEXT)' -delete
	$(call DELFILE, xcc_marks.c.wcc$(OBJEXT))
	$(call DELDIR, $(HOSTDIR))
	$(call DELDIR, $(SYSROOT))
