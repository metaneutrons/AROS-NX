# Copyright (C) 2026 The AROS Development Team. All rights reserved.
#
# Install layout for native GNU toolchain packages. The validated producer
# release selector requires relocatable staging; ordinary builds preserve
# their historical physical prefix unless they explicitly opt in.
AROS_GNU_RELEASE_LAYOUT ?= $(if $(filter 1,$(AROS_TOOLCHAIN_RELEASE)),yes,no)

ifneq ($(AROS_GNU_RELEASE_LAYOUT),yes)
ifneq ($(AROS_GNU_RELEASE_LAYOUT),no)
$(error AROS_GNU_RELEASE_LAYOUT must be either yes or no)
endif
endif

ifeq ($(AROS_TOOLCHAIN_RELEASE),1)
ifneq ($(AROS_GNU_RELEASE_LAYOUT),yes)
$(error AROS_TOOLCHAIN_RELEASE=1 requires AROS_GNU_RELEASE_LAYOUT=yes)
endif
endif

GNU_CONFIGURE_PREFIX := $(CROSSTOOLSDIR)
GNU_CONFIGURE_BINDIR := $(CROSSTOOLSDIR)
GNU_BINUTILS_CONFIGURE_BINDIR = $(GNU_CONFIGURE_BINDIR)
GNU_BINUTILS_BUILD_OPTS :=
GNU_BINUTILS_ZSTD_ENV :=
GNU_BINUTILS_ZSTD_OPTS :=
GNU_CONFIGURE_LIBDIR := $(CROSSTOOLSDIR)/lib
GNU_CONFIGURE_SYSROOT := --with-sysroot=$(AROS_DEVELOPER)
GNU_GCC_BUILD_SYSROOT :=
GNU_INSTALL_ENV :=
GNU_BUILDDEPDIR := $(CROSSTOOLSDIR)
GNU_HOST_PACKAGES_USE_CROSSTOOLSDIR := yes
GNU_BUILDDEPS_PKG_CONFIG_ENV :=
GNU_GMP_EXTRA_OPTS = --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --disable-shared
GNU_ISL_EXTRA_OPTS = --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --without-piplib --disable-shared
GNU_MPFR_EXTRA_OPTS = --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --disable-shared
GNU_MPC_EXTRA_OPTS = --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --disable-shared
GNU_BINUTILS_LAYOUT_OPTS = $(strip --bindir=$(GNU_BINUTILS_CONFIGURE_BINDIR) --libdir=$(GNU_CONFIGURE_LIBDIR) $(GNU_CONFIGURE_SYSROOT))
GNU_GCC_LAYOUT_OPTS = $(strip $(GNU_CONFIGURE_SYSROOT) $(GNU_GCC_BUILD_SYSROOT) --with-native-system-header-dir=/include --bindir=$(GNU_CONFIGURE_BINDIR) --libdir=$(GNU_CONFIGURE_LIBDIR) --with-isl=$(GNU_BUILDDEPDIR))
GNU_GCC_MATH_OPTS :=
GNU_GCC_LAYOUT_OVERRIDE_OPTS :=
GNU_GDB_LAYOUT_OPTS = --bindir=$(GNU_CONFIGURE_BINDIR) --libdir=$(GNU_CONFIGURE_LIBDIR)
GNU_REPRO_HOST_ENV :=
GNU_RECORDED_METADATA_ENV :=
GNU_REPRO_TARGET_ENV :=
GNU_GMP_REPRO_HOST_ENV = $(GNU_REPRO_HOST_ENV)

ifeq ($(AROS_GNU_RELEASE_LAYOUT),yes)
# A root prefix produces STANDARD_BINDIR_PREFIX=// in GCC, which cannot
# relocate against /lib/gcc. Use a canonical neutral configure namespace;
# the install overrides below still produce the flat candidate layout.
GNU_CONFIGURE_PREFIX := /aros-toolchain
GNU_CONFIGURE_BINDIR := $(GNU_CONFIGURE_PREFIX)
# libiberty compares directory components including their trailing separator.
# Without this slash, flat ld looks outside the relocated package for scripts.
# Autoconf strips trailing slashes; preserve it at compile time instead. The
# separate install overrides still select bindir=/ for flat staging.
GNU_BINUTILS_BUILD_OPTS := bindir=$(GNU_CONFIGURE_PREFIX)/
GNU_CONFIGURE_LIBDIR := $(GNU_CONFIGURE_PREFIX)/lib
GNU_CONFIGURE_SYSROOT := --with-sysroot=yes
GNU_GCC_BUILD_SYSROOT := --with-build-sysroot=$(AROS_DEVELOPER)
GNU_RECORDED_METADATA_ENV = AROS_GCC_RELEASE_LAYOUT=yes AROS_GCC_RELEASE_BUILD_ROOT="$(TOP)" AROS_GCC_RELEASE_SOURCE_ROOT="$(SRCDIR)"
GNU_INSTALL_ENV := prefix=/ exec_prefix=/ bindir=/ libdir=/lib DESTDIR=$(CROSSTOOLSDIR)
GNU_BUILDDEPDIR := $(HOSTGENDIR)/$(CURDIR)/gnu-builddeps
GNU_ZSTD_PREFIX := $(GNU_BUILDDEPDIR)/zstd
# BFD is configured later by make, not by top-level configure. Forward the
# private static library selection across both boundaries. Normal library
# flags avoid nesting an archive inside libbfd.a through Libtool.
GNU_BINUTILS_ZSTD_ENV = ZSTD_CFLAGS="-I$(GNU_ZSTD_PREFIX)/include" ZSTD_LIBS="-L$(GNU_ZSTD_PREFIX)/lib -lzstd"
GNU_BINUTILS_BUILD_OPTS += $(GNU_BINUTILS_ZSTD_ENV)
GNU_BINUTILS_ZSTD_OPTS := --with-zstd
GNU_HOST_PACKAGES_USE_CROSSTOOLSDIR := no
GNU_BUILDDEPS_PKG_CONFIG_ENV = PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=$(GNU_BUILDDEPDIR)/lib/pkgconfig
GNU_ISL_EXTRA_OPTS = --with-gmp=system --with-gmp-prefix=$(GNU_BUILDDEPDIR) --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --without-piplib --disable-shared
GNU_MPFR_EXTRA_OPTS = --with-gmp=$(GNU_BUILDDEPDIR) --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --disable-shared
GNU_MPC_EXTRA_OPTS = --with-gmp=$(GNU_BUILDDEPDIR) --with-mpfr=$(GNU_BUILDDEPDIR) --bindir=$(GNU_BUILDDEPDIR) --libdir=$(GNU_BUILDDEPDIR)/lib --disable-shared
GNU_GCC_MATH_OPTS := --with-gmp=$(GNU_BUILDDEPDIR) --with-mpfr=$(GNU_BUILDDEPDIR) --with-mpc=$(GNU_BUILDDEPDIR)
GNU_GCC_LAYOUT_OVERRIDE_OPTS := --prefix=$(GNU_CONFIGURE_PREFIX) --bindir=$(GNU_CONFIGURE_BINDIR) --libdir=$(GNU_CONFIGURE_LIBDIR) --with-native-system-header-dir=/include --with-sysroot=yes --with-build-sysroot=$(AROS_DEVELOPER) --with-isl=$(GNU_BUILDDEPDIR) $(GNU_GCC_MATH_OPTS)

# Keep producer remaps separate from the AROS target's ambient CFLAGS. GCC's
# target-library flags are added below from GCC_TARGET_CFLAGS/CXXFLAGS only.
GNU_REPRO_HOST_ENV = CFLAGS="$(strip $(AROS_TOOLCHAIN_REPRO_FLAGS))" CXXFLAGS="$(strip $(AROS_TOOLCHAIN_REPRO_FLAGS))"
GNU_REPRO_TARGET_ENV = CFLAGS_FOR_TARGET="$(strip $(AROS_TOOLCHAIN_REPRO_FLAGS) $(GCC_TARGET_CFLAGS))" CXXFLAGS_FOR_TARGET="$(strip $(AROS_TOOLCHAIN_REPRO_FLAGS) $(GCC_TARGET_CXXFLAGS))"
GNU_GMP_REPRO_HOST_ENV = CFLAGS="$(strip $(AROS_TOOLCHAIN_REPRO_FLAGS) -std=gnu11)" CXXFLAGS="$(strip $(AROS_TOOLCHAIN_REPRO_FLAGS))"
ifneq ($(filter --with-sysroot% --with-build-sysroot%,$(GCC_EXTRA_OPTS)),)
$(error AROS_GNU_RELEASE_LAYOUT=yes cannot be combined with GCC_EXTRA_OPTS containing a sysroot option)
endif
endif
