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
#   wcc    C to WebAssembly (WASI): preprocessor, compiler and linker in one
#   xcc    C to native NuttX modules (Thumb-2, the ARM32 backend): its
#          driver, preprocessor, compiler, assembler and linker, run one
#          after the other in the same process (CONFIG_DEVTOOLS_XCC_NATIVE)
#
# Each of these programs (wcc; xccd, the driver, cpp, cc1, as, ld) is
# linked into one object of its own (NAME.blob.o, ld -r with xcc.ld) in
# which only its main (xcc_NAME_main) and the markers around its variables
# stay global, so that nothing of it clashes with anything else when it is
# built in (the simulator), and its variables can be put back as loaded
# before every run (xcc_nuttx.c: xcc_run()).
#
# The headers and libraries the compiled programs need are made here too
# (sysroot/, for CONFIG_DEVTOOLS_XCC_ROOT on the SD card), with builds of
# the same compilers for this machine (host/): include/ and lib/ for wcc
# (xcc's headers and its WASI C library); arm/ for xcc: arm/include
# (stdarg.h, math.h, float.h), arm/nuttx (the firmware's own NuttX headers
# that the standard ones need: nuttx/mkheaders.py), arm/lib/crt0.o.
# docs/xcc.md.

X = xcc/src
L = xcc/libsrc

PROGNAME  = wcc
PRIORITY  = $(CONFIG_DEVTOOLS_XCC_PRIORITY)
STACKSIZE = $(CONFIG_DEVTOOLS_XCC_STACKSIZE)
MODULE    = $(CONFIG_DEVTOOLS_XCC)
MAINSRC   = wcc_main.c
BLOBS     = wcc

ifeq ($(CONFIG_DEVTOOLS_XCC_NATIVE),y)
PROGNAME += xcc
PRIORITY += $(CONFIG_DEVTOOLS_XCC_PRIORITY)
STACKSIZE += $(CONFIG_DEVTOOLS_XCC_STACKSIZE)
MAINSRC  += xcc_entry.c
BLOBS    += xccd cpp cc1 as ld
endif

# The programs' sources, flags and the file with their main

UTIL_SRCS = $(X)/util/util.c $(X)/util/table.c

wcc_SRCS  = $(wildcard $(X)/wcc/*.c) $(wildcard $(X)/cc/frontend/*.c)
wcc_SRCS += $(X)/cpp/preprocessor.c $(X)/cpp/pp_parser.c $(X)/cpp/macro.c $(X)/cpp/pch.c
wcc_SRCS += $(UTIL_SRCS) $(X)/util/archive.c
wcc_FLAGS = -I$(X)/util -I$(X)/cc/frontend -I$(X)/cpp -DXCC_TARGET_ARCH=XCC_ARCH_WASM
wcc_MAIN  = $(X)/wcc/wcc.c

ARM_FLAGS = -DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX

xccd_SRCS  = xcc_main.c
xccd_FLAGS =
xccd_MAIN  = xcc_main.c

cpp_SRCS  = $(wildcard $(X)/cpp/*.c) $(X)/cc/frontend/lexer.c $(X)/cc/frontend/ast.c
cpp_SRCS += $(UTIL_SRCS)
cpp_FLAGS = -I$(X)/util -I$(X)/cc/frontend -I$(X)/cpp $(ARM_FLAGS)
cpp_MAIN  = $(X)/cpp/cpp.c

cc1_SRCS  = $(wildcard $(X)/cc/frontend/*.c) $(wildcard $(X)/cc/backend/*.c)
cc1_SRCS += $(X)/cc/builtin.c $(X)/cc/cc1.c $(wildcard $(X)/cc/arch/arm32/*.c)
cc1_SRCS += $(UTIL_SRCS)
cc1_FLAGS = -I$(X)/util -I$(X)/cc/frontend -I$(X)/cc/backend $(ARM_FLAGS)
cc1_MAIN  = $(X)/cc/cc1.c

as_SRCS  = $(wildcard $(X)/as/*.c) $(wildcard $(X)/as/arch/arm32/*.c) $(UTIL_SRCS)
as_FLAGS = -I$(X)/util -I$(X)/as $(ARM_FLAGS)
as_MAIN  = $(X)/as/as.c

ld_SRCS  = $(X)/ld/arm32/ld_arm32.c $(UTIL_SRCS) $(X)/util/archive.c
ld_FLAGS = -I$(X)/util $(ARM_FLAGS)
ld_MAIN  = $(X)/ld/arm32/ld_arm32.c

# -fno-short-enums: xcc passes -1 for "any" in its enum TokenKind
# (match(-1)), which arm-none-eabi's byte-sized enums make 255

XCCFLAGS = -std=gnu11 -D_DEFAULT_SOURCE -fno-common -fno-short-enums -w
XCCFLAGS += -include $(CURDIR)/xcc_nuttx.h

ifeq ($(CONFIG_DEVTOOLS_XCC),m)
XCC_CC = $(MODULECC) -c $(CELFFLAGS)
else
XCC_CC = $(CC) -c $(CFLAGS)
endif

define BLOB_RULES
$(1)_OBJS = $$(patsubst %.c,%.$(1)$(OBJEXT),$$($(1)_SRCS)) xcc_marks.c.$(1)$(OBJEXT)

$$(patsubst %.c,%.$(1)$(OBJEXT),$$($(1)_SRCS)): %.$(1)$(OBJEXT): %.c xcc_nuttx.h
	$$(ECHO_BEGIN)"CC: $$< "
	$$(Q) $$(XCC_CC) $$(XCCFLAGS) $$($(1)_FLAGS) \
		$$(if $$(filter $$($(1)_MAIN),$$<),-Dmain=xcc_$(1)_main) $$< -o $$@
	$$(ECHO_END)

xcc_marks.c.$(1)$(OBJEXT): xcc_marks.c
	$$(Q) $$(XCC_CC) -DXCC_MARK=$(1) $$< -o $$@

$(1).blob$(OBJEXT): $$($(1)_OBJS) xcc.ld
	$$(ECHO_BEGIN)"LD: $$@ "
	$$(Q) $$(LD) -r -T xcc.ld -o $$@.tmp $$($(1)_OBJS)
	$$(Q) $$(OBJCOPY) $$(addprefix -G ,xcc_$(1)_main $(1)_data_start $(1)_data_end \
		$(1)_bss_start $(1)_bss_end) $$@.tmp $$@
	$$(Q) rm -f $$@.tmp
	$$(ECHO_END)
endef

$(foreach b,$(BLOBS),$(eval $(call BLOB_RULES,$(b))))

ifeq ($(CONFIG_DEVTOOLS_XCC),m)

# Loadable modules: the blobs and the runtime linked into them, not into
# libapps.a

xcc_nuttx.m$(OBJEXT): xcc_nuttx.c xcc_nuttx.h
	$(Q) $(XCC_CC) $< -o $@

include $(APPDIR)/Application.mk

$(BINDIR)$(DELIM)wcc: wcc.blob$(OBJEXT) xcc_nuttx.m$(OBJEXT)
$(BINDIR)$(DELIM)wcc: LDLIBS = wcc.blob$(OBJEXT) xcc_nuttx.m$(OBJEXT) $(COMPILER_RT_LIB)

XCC_BLOB_OBJS = $(addsuffix .blob$(OBJEXT),xccd cpp cc1 as ld)
$(BINDIR)$(DELIM)xcc: $(XCC_BLOB_OBJS) xcc_nuttx.m$(OBJEXT)
$(BINDIR)$(DELIM)xcc: LDLIBS = $(XCC_BLOB_OBJS) xcc_nuttx.m$(OBJEXT) $(COMPILER_RT_LIB)

else

CSRCS = xcc_nuttx.c
EXTOBJS = $(addsuffix .blob$(OBJEXT),$(BLOBS))

include $(APPDIR)/Application.mk

endif

# Host builds of the compilers: the sysroot's makers

HOSTDIR = host
SYSROOT = sysroot
HOSTCFLAGS_XCC = -O2 -std=gnu11 -D_DEFAULT_SOURCE -w

$(HOSTDIR)/wcc: $(wcc_SRCS)
	$(ECHO_BEGIN)"HOSTCC: $@ "
	$(Q) mkdir -p $(HOSTDIR)
	$(Q) $(HOSTCC) $(HOSTCFLAGS_XCC) $(wcc_FLAGS) -o $@ $^
	$(ECHO_END)

$(HOSTDIR)/cpp: $(cpp_SRCS)
	$(Q) mkdir -p $(HOSTDIR)
	$(Q) $(HOSTCC) $(HOSTCFLAGS_XCC) $(cpp_FLAGS) -o $@ $^

$(HOSTDIR)/cc1: $(cc1_SRCS)
	$(Q) mkdir -p $(HOSTDIR)
	$(Q) $(HOSTCC) $(HOSTCFLAGS_XCC) $(cc1_FLAGS) -o $@ $^

$(HOSTDIR)/as: $(as_SRCS)
	$(Q) mkdir -p $(HOSTDIR)
	$(Q) $(HOSTCC) $(HOSTCFLAGS_XCC) $(as_FLAGS) -o $@ $^

$(HOSTDIR)/ld: $(ld_SRCS)
	$(Q) mkdir -p $(HOSTDIR)
	$(Q) $(HOSTCC) $(HOSTCFLAGS_XCC) $(ld_FLAGS) -o $@ $^

# wcc's sysroot: xcc's headers (with the WebAssembly runtime's API,
# docs/wasm.md: #include <fpc_wasm.h>) and its WASI C library

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

FPC_WASM_H = $(wildcard $(APPDIR)/graphics/fpcwasm/sdk/fpc_wasm.h)

$(SYSROOT)/.include: $(shell find xcc/include -type f) $(FPC_WASM_H) Makefile
	$(Q) rm -rf $(SYSROOT)/include
	$(Q) mkdir -p $(SYSROOT)
	$(Q) cp -r xcc/include $(SYSROOT)/include
	$(Q) rm -rf $(SYSROOT)/include/mach $(SYSROOT)/include/mach-o
	$(if $(FPC_WASM_H),$(Q) cp $(FPC_WASM_H) $(SYSROOT)/include/)
	$(Q) touch $@

SYSROOT_TARGETS = $(SYSROOT)/lib/wlibc.a $(SYSROOT)/lib/wcrt0.a $(SYSROOT)/.include

# xcc's sysroot (arm/): the overlay headers, crt0.o and, for the board,
# the firmware's NuttX headers

ifeq ($(CONFIG_DEVTOOLS_XCC_NATIVE),y)

ARM_PREDEF = -D__XCC -D__ILP32__ -D__arm__ -D__thumb__ -D__thumb2__ -D__ARM_EABI__ \
             -D__ARM_PCS_VFP -D__VFP_FP__ -D__STDC__ -D__STDC_VERSION__=199901L \
             -D__NuttX__ -D__asm__=__asm -D__volatile__=volatile

$(SYSROOT)/arm/lib/crt0.o: nuttx/crt0.c $(HOSTDIR)/cpp $(HOSTDIR)/cc1 $(HOSTDIR)/as
	$(Q) mkdir -p $(SYSROOT)/arm/lib $(HOSTDIR)/arm
	$(Q) $(HOSTDIR)/cpp $(ARM_PREDEF) -o $(HOSTDIR)/arm/crt0.i $<
	$(Q) $(HOSTDIR)/cc1 -o $(HOSTDIR)/arm/crt0.s $(HOSTDIR)/arm/crt0.i
	$(Q) $(HOSTDIR)/as -o $@ $(HOSTDIR)/arm/crt0.s

$(SYSROOT)/arm/.include: $(wildcard nuttx/include/*.h) Makefile
	$(Q) rm -rf $(SYSROOT)/arm/include
	$(Q) mkdir -p $(SYSROOT)/arm
	$(Q) cp -r nuttx/include $(SYSROOT)/arm/include
	$(Q) touch $@

SYSROOT_TARGETS += $(SYSROOT)/arm/lib/crt0.o $(SYSROOT)/arm/.include

ifneq ($(CONFIG_ARCH_SIM),y)
$(SYSROOT)/arm/.nuttx: nuttx/headers.list nuttx/mkheaders.py $(HOSTDIR)/cpp \
                       $(TOPDIR)/include/nuttx/config.h $(SYSROOT)/arm/.include
	$(Q) python3 nuttx/mkheaders.py $(HOSTDIR)/cpp $(TOPDIR)/include $(SYSROOT)/arm/include \
		nuttx/headers.list $(SYSROOT)/arm/nuttx
	$(Q) touch $@

SYSROOT_TARGETS += $(SYSROOT)/arm/.nuttx
endif

# The headers' version (a sum of them all): the precompiled headers made
# with others are not used (cpp/pch.c, --pch-key)

$(SYSROOT)/arm/stamp: $(filter-out $(SYSROOT)/arm/stamp,$(SYSROOT_TARGETS))
	$(Q) (cd $(SYSROOT)/arm && find include nuttx -type f 2>/dev/null | LC_ALL=C sort | \
		xargs cat | md5sum | cut -c1-16) > $@

SYSROOT_TARGETS += $(SYSROOT)/arm/stamp
endif

sysroot: $(SYSROOT_TARGETS)

.PHONY: sysroot

all:: sysroot

clean::
	$(Q) rm -f *.blob$(OBJEXT) xcc_marks.c.*$(OBJEXT) xcc_nuttx.m$(OBJEXT)
	$(Q) find xcc -name '*$(OBJEXT)' -delete
	$(call DELDIR, $(HOSTDIR))
	$(call DELDIR, $(SYSROOT))
