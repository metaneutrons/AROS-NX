# Native SDK configuration, not a generated classic make.cfg.
# The consumer contract seals this projection and the classic input templates.
# Compiler paths are roles supplied by the native engine, never host PATH tools.
TARGET_CC = $(NATIVE_TARGET_CC)
TARGET_SYSROOT = --sysroot=$(AROS_DEVELOPER)
AR = $(NATIVE_TARGET_AR) cr
RANLIB = $(NATIVE_TARGET_RANLIB)

# configure.in's ESP32-P4 RV32 defaults and an explicit native SDK policy.
GCC_DEFAULT_CPU := rv32imafc_zicsr_zifencei_zaamo_zalrsc
GCC_DEFAULT_MODE := ilp32f
TARGET_ISA_CFLAGS = -march=$(GCC_DEFAULT_CPU) -mabi=$(GCC_DEFAULT_MODE) -mcmodel=medany
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
OBJDIR := $(GENDIR)/$(CURDIR)

# These ordinary default-off debug switches are local Make recipe inputs;
# they are not exported MetaMake globals or permissions to omit dependencies.
DEBUG :=
STARTUP_DEBUG := 0

# Classic Make expands the undefined INCEMPTYDIRS to empty. Declare that
# configuration explicitly instead of asking the native engine to guess it.
INCEMPTYDIRS :=

# Shared kernel headers included by the SDK need one source-defined board
# macro. Bind the existing default; board.mk supplies its flags. This does
# not select firmware, partitions, deployment or a hardware qualification.
P4_BOARD := d1001

# Preserve the generic source linker contract; never blank the romtag script.
KERNEL_KOBJ_LDSCRIPT := -T $(SRCDIR)/config/kobj-romtag.ld
