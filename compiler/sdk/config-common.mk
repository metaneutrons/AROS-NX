# Source-owned native SDK projection, not a generated classic make.cfg.
# The SDK target descriptor selects ISA/code-generation flags; the native
# language hooks apply them to the measured compiler roles, not PATH tools.
TARGET_CC = $(NATIVE_TARGET_CC)
TARGET_SYSROOT = --sysroot=$(AROS_DEVELOPER)
AR = $(NATIVE_TARGET_AR) cr
RANLIB = $(NATIVE_TARGET_RANLIB)
# The SDK descriptor owns the baseline target ABI. Recipe-local ISA additions
# still use the classic source interface, in its original flag order.
ISA_FLAGS :=
TARGET_ISA_CFLAGS = $(ISA_FLAGS)
OPTIMIZATION_CFLAGS := -O2
CONFIG_CFLAGS := -fno-common
CONFIG_CPPFLAGS :=
USER_CFLAGS ?=
USER_CPPFLAGS ?=
USER_INCLUDES ?=
TARGET_CFLAGS = $(strip $(OPTIMIZATION_CFLAGS) $(CONFIG_CFLAGS) $(USER_CFLAGS))
TARGET_CPPFLAGS = $(strip $(USER_INCLUDES) $(CONFIG_CPPFLAGS) $(USER_CPPFLAGS))
CFLAGS = $(strip $(TARGET_ISA_CFLAGS) $(TARGET_CFLAGS))
CPPFLAGS = $(strip $(TARGET_CPPFLAGS))
CFLAGS_IQUOTE := -iquote
CFLAGS_IQUOTE_END :=
MKDEPEND := $(SRCDIR)/scripts/mkdep
# The LLVM SDK has no configure-selected GCC IPA workaround. Keep the
# source macro's safety context explicit rather than using an ambient value.
SAFETY_CFLAGS :=
OBJDIR := $(GENDIR)/$(CURDIR)
ifneq ($(AROS_TARGET_CPU32),)
AROS_LIB32 := $(AROS_DEVELOPER)/$(AROS_DIR_LIB)32
TARGET_ISA_32_CFLAGS = $(NATIVE_TARGET_32_CFLAGS)
endif
# Explicit SDK authoring policy, not a claim of global MetaMake defaults.
DEBUG :=
STARTUP_DEBUG := 0
INCEMPTYDIRS :=
KERNEL_KOBJ_LDSCRIPT := -T $(SRCDIR)/config/kobj-romtag.ld
