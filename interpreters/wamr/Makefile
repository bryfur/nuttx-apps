############################################################################
# apps/interpreters/wamr/Makefile
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

# The source: the FPC's fork of WAMR (2.4.5 and the FPC's changes as
# commits), the superproject's submodule third_party/wasm-micro-runtime,
# reached through the link wamr/ (FPC_LINK, apps/Make.defs)

WAMR_UNPACK   = wamr

-include $(WAMR_UNPACK)/product-mini/platforms/nuttx/wamr.mk

VPATH += $(WAMR_UNPACK)/product-mini/platforms/nuttx
DEPPATH += --dep-path $(WAMR_UNPACK)/product-mini/platforms/nuttx

ifeq ($(CONFIG_INTERPRETERS_IWASM_TASK),y)
MAINSRC = main.c

PROGNAME  = iwasm
PRIORITY  = $(CONFIG_INTERPRETERS_WAMR_PRIORITY)
STACKSIZE = $(CONFIG_INTERPRETERS_WAMR_STACKSIZE)
MODULE    = $(CONFIG_INTERPRETERS_WAMR)

endif

# The interpreter's loop can be a section of its own where the board
# places sections by name (the fork's "interp section": the fpc board puts a
# module's .time_critical* in internal SRAM while it runs).  Not done: on
# the board it made no difference (Rocks -b 400 interpreted: 23.0 ms a
# frame in SRAM, 22.5 ms from the PSRAM; its 12.5 KB fit the XIP cache)
#
# CFLAGS += '-DWASM_INTERP_SECTION=".time_critical.wasm_interp"'

# The RP2350's PSRAM (where the runtime's memory comes from: the user
# heap) has no exclusive accesses: a LDREX/STREX loop on it never ends.
# Read-modify-writes in a critical section instead (the fork's "atomics
# without exclusives")

ifeq ($(CONFIG_RP23XX_PSRAM),y)
CFLAGS += -DBH_ATOMIC_RMW_CSECTION -DWASM_UINT16_IS_ATOMIC=0
endif

# The WebAssembly JIT (the FPC's apps-fpc/graphics/fpcwasm/jit, reached as
# apps/external): part of the runtime, built with its configuration (it
# uses the runtime's structures); a stub where it has no code generator
# (CONFIG_FPCWASM_JIT off, the simulator)

ifneq ($(CONFIG_GRAPHICS_FPCWASM),)
CSRCS += wasm_jit.c wasm_jit_rt.c
VPATH += $(FPC_APPS)/graphics/fpcwasm/jit
DEPPATH += --dep-path $(FPC_APPS)/graphics/fpcwasm/jit
endif

# What WAMR's POSIX layer needs that this NuttX may leave out

CSRCS += wamr_nuttx_stubs.c

WAMR_NEED_CUSTOM_INIT = n

# Check if we need to build custom init code

ifeq ($(CONFIG_INTERPRETERS_WAMR_EXTERNAL_MODULE_REGISTRY),y)
WAMR_NEED_CUSTOM_INIT = y
endif

# If we need custom init code, add it to the build
ifeq ($(WAMR_NEED_CUSTOM_INIT),y)
# When the loadable module configuration is enabled,we need to use CELFFLAGS
$(WAMR_UNPACK)/product-mini/platforms/nuttx/main.c_CFLAGS = -Dwasm_runtime_full_init=wamr_custom_init
$(WAMR_UNPACK)/product-mini/platforms/nuttx/main.c_CELFFLAGS = -Dwasm_runtime_full_init=wamr_custom_init
CSRCS += wamr_custom_init.c
endif

.PHONY: wamr_link

wamr_link:
	$(call FPC_LINK,$(WAMR_UNPACK),wasm-micro-runtime)

context:: wamr_link

# Register the external WAMR module

ifeq ($(CONFIG_INTERPRETERS_WAMR_EXTERNAL_MODULE_REGISTRY),y)

PDATLIST = $(strip $(call RWILDCARD, registry, *.pdat))
BDATLIST = $(strip $(call RWILDCARD, registry, *.bdat))

registry$(DELIM).updated:
	$(Q) touch registry$(DELIM).updated

wamr_external_module_list.h: registry$(DELIM).updated
ifeq ($(BDATLIST),)
	$(call DELFILE, wamr_external_module_list.h)
	$(Q) touch wamr_external_module_list.h
else
	$(call CATFILE, wamr_external_module_list.h, $(BDATLIST))
endif

wamr_external_module_proto.h: registry$(DELIM).updated
ifeq ($(PDATLIST),)
	$(call DELFILE, wamr_external_module_proto.h)
	$(Q) touch wamr_external_module_proto.h
else
	$(call CATFILE, wamr_external_module_proto.h, $(PDATLIST))
endif

depend:: $(GLUECSRCS) wamr_external_module_list.h wamr_external_module_proto.h
endif # CONFIG_INTERPRETERS_WAMR_EXTERNAL_MODULE_REGISTRY

clean::
	$(call DELFILE, wamr_external_module_list.h)
	$(call DELFILE, wamr_external_module_proto.h)

clean_context::
	$(call DELFILE, $(BDATLIST))
	$(call DELFILE, $(PDATLIST))

distclean:: clean clean_context
	$(call DELDIR, $(WAMR_UNPACK))

include $(APPDIR)/Application.mk
