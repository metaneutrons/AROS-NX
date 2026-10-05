# Closed native configuration for the esp32p4-d1001 source profile.
# This projection replaces config/aros.cfg for explicitly selected native
# KOBJ and literal compile-rule evaluation. It does not import generated Make configuration, process
# environment overrides, or previously built Make payloads. Local source
# make.opts files remain ordered inputs after this global include.
#
# Sources inventoried alongside this file in native-build-v1.json:
# - config/make.cfg.in: KOBJ_LDFLAGS is only assigned in the aarch64 branch;
#   native RV32 retains it as undefined so later local ?= remains effective.
# - config/make.cfg.in: KERNEL_KOBJ_LDSCRIPT starts empty and changes only for
#   amiga/m68k, not this profile.
# - config/target.cfg.in: FUNCINSTR_LIBS = instrfunc.
# - config/build.cfg.in: TARGET_FUNCINSTR := no.
# The native profile selects these source defaults; optional generated global
# make.opts/make.defaults and ambient overrides are not part of this profile.
undefine KOBJ_LDFLAGS
KERNEL_KOBJ_LDSCRIPT :=
FUNCINSTR_LIBS = instrfunc
TARGET_FUNCINSTR := no

# %copy_includes defaults to $(INCLUDE_FILES). This profile does not import
# ambient Make variables, so an otherwise undefined header list is explicitly
# empty. Individual source recipes continue to replace it before declaration.
INCLUDE_FILES :=

# compiler/include/mmakefile.src leaves its optional empty-directory list
# commented out. Select an explicit empty list rather than treating an
# unresolved variable as proof that no extra directories are required.
INCEMPTYDIRS :=

# Normal, non-debug GNU build policy, derived from the inventoried configure
# and configuration templates. The selected GCC 16 compiler independently
# accepts the two target warning suppressors below; the Clang-only
# unused-command-line-argument and unknown-warning-option lanes are empty.
# No ambient user flag or compiler wrapper is selected by this source profile.
# Source-local USER_* assignments remain ordered and affect each invocation.
# config/make.cfg.in derives AROS_DEVELOPER; the native directory projection
# maps it to the current build's SYS/Developer rather than GCC's baked sysroot.
TARGET_SYSROOT = --sysroot=$(AROS_DEVELOPER)
TARGET_ISA_CFLAGS = -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany
USER_CFLAGS ?=
USER_INCLUDES ?=
USER_CPPFLAGS ?=
TARGET_CFLAGS = $(strip -O2 -Wall -Werror -Wno-pointer-sign -Wno-parentheses $(USER_CFLAGS))
TARGET_CPPFLAGS = $(strip $(USER_INCLUDES) $(USER_CPPFLAGS))
# The literal source recipe explicitly repeats these lanes. Do not deduplicate
# them or add CMake's unrelated global compile options to this command.
CFLAGS = $(strip $(TARGET_ISA_CFLAGS) $(TARGET_CFLAGS))
CPPFLAGS = $(strip $(TARGET_CPPFLAGS))

# Target command roles bind only to the admitted native toolchain. The tools
# supply these three symbolic paths; the source chooses the role and flags.
# configure.in selects GNU AR/RANLIB (without LTO), with AR flags "cr";
# config/target.cfg.in and make.tmpl retain their argument order.
TARGET_CC = $(NATIVE_TARGET_CC)
AR = $(NATIVE_TARGET_AR) cr
RANLIB = $(NATIVE_TARGET_RANLIB)
OBJDIR := $(GENDIR)/$(CURDIR)

# config/features.in probes this quote lane. The admitted GCC 16.2.0 compiled
# an ELF32/RISC-V single-float test with -iquote. on 2026-10-05; keep the empty
# suffix explicit rather than borrowing a host compiler's feature result.
CFLAGS_IQUOTE := -iquote
CFLAGS_IQUOTE_END :=

# geninc.cfg.in retains quotes around configure's empty ENABLE_EXECSMP.
# The execbase recipe compares against the literal two-quote Make value;
# an unquoted empty value would incorrectly select its SMP replacement lane.
EXECSMP = ""
