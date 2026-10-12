.PHONY: generated-params generated-install-args generated-install-values
.PHONY: generated-child-env generated-export-values
.PHONY: generated-binutils-build-args generated-build-values generated-binutils-install-args

generated-binutils-build-args:
	@$(MAKE) --no-print-directory --silent \
	  -f $(abspath $(lastword $(MAKEFILE_LIST))) \
	  generated-build-values $(crosstools-binutils--make-env) $(crosstools-binutils--build_opts)

generated-build-values:
	@printf '%s\n' 'build-bindir=$(bindir)' 'build-destdir=$(DESTDIR)' \
	  'build-zstd-cflags=$(ZSTD_CFLAGS)' 'build-zstd-libs=$(ZSTD_LIBS)'

generated-binutils-install-args:
	@$(MAKE) --no-print-directory --silent \
	  -f $(firstword $(MAKEFILE_LIST)) \
	  -f $(lastword $(MAKEFILE_LIST)) \
	  generated-install-values $(crosstools-binutils--install_opts)

generated-child-env:
	@$(MAKE) --no-print-directory --silent \
	  -f $(abspath $(lastword $(MAKEFILE_LIST))) \
	  generated-export-values $(crosstools-gcc--make-env)

generated-export-values:
	@printf '%s\n' \
	  "metadata-layout=$$AROS_GCC_RELEASE_LAYOUT" \
	  "metadata-build-root=$$AROS_GCC_RELEASE_BUILD_ROOT" \
	  "metadata-source-root=$$AROS_GCC_RELEASE_SOURCE_ROOT"

generated-params:
	@printf '%s\n' \
	  'mode=$(AROS_GNU_RELEASE_LAYOUT)' \
	  'builddepdir=$(GNU_BUILDDEPDIR)' \
	  'host-packages-use-crosstoolsdir=$(GNU_HOST_PACKAGES_USE_CROSSTOOLSDIR)' \
	  'gcc-prefix=$(crosstools-gcc--aros_prefix)' \
	  'gcc-configure-args=$(crosstools-gcc--config_opts) $(GNU_GCC_CONFIGURE_EXTRA_OPTS)' \
	  'gcc-configure-env=$(crosstools-gcc--cfg-env)' \
	  'gcc-install-args=$(crosstools-gcc--install_opts)' \
	  'gcc-install-stamp=$(gcc-installflag)' \
	  'gcc-configure-stamp=$(crosstools-gcc--configflag)' \
	  'gmp-configure-stamp=$(crosstools-gmp--configflag)' \
	  'binutils-configure-stamp=$(crosstools-binutils--configflag)' \
	  'binutils-prefix=$(crosstools-binutils--aros_prefix)' \
	  'binutils-configure-args=$(crosstools-binutils--config_opts) $(BINUTILS_EXTRA_OPTS)' \
	  'binutils-configure-env=$(crosstools-binutils--cfg-env)' \
	  'binutils-install-args=$(crosstools-binutils--install_opts)' \
	  'binutils-build-args=$(crosstools-binutils--build_opts)' \
	  'binutils-install-stamp=$(binutils-installflag)' \
	  'gdb-prefix=$(crosstools-gdb--aros_prefix)' \
	  'gdb-configure-args=$(crosstools-gdb--config_opts) $(GDB_EXTRA_OPTS)' \
	  'gdb-install-args=$(crosstools-gdb--install_opts)' \
	  'gdb-install-stamp=$(gdb-installflag)' \
	  'gdb-configure-stamp=$(crosstools-gdb--configflag)' \
	  'libatomic-configure-stamp=$(tools-crosstools-gcc-libatomic-configflag)' \
	  'libatomic-install-stamp=$(tools-crosstools-gcc-libatomic-installflag)' \
	  'gmp-configure-args=$(crosstools-gmp--config_opts) $(GNU_GMP_EXTRA_OPTS)' \
	  'gmp-configure-env=$(crosstools-gmp--cfg-env)' \
	  'zstd-build-dir=$(GNU_ZSTD_BUILD)' \
	  'isl-configure-args=$(crosstools-isl--config_opts) $(GNU_ISL_EXTRA_OPTS)' \
	  'isl-configure-env=$(crosstools-isl--cfg-env)' \
	  'mpfr-configure-args=$(crosstools-mpfr--config_opts) $(GNU_MPFR_EXTRA_OPTS)' \
	  'mpc-configure-args=$(crosstools-mpc--config_opts) $(GNU_MPC_EXTRA_OPTS)'

generated-install-args:
	@$(MAKE) --no-print-directory --silent \
	  -f $(firstword $(MAKEFILE_LIST)) \
	  -f $(lastword $(MAKEFILE_LIST)) \
	  generated-install-values $(crosstools-gcc--install_opts)

generated-install-values:
	@printf '%s\n' \
	  'install-prefix=$(prefix)' \
	  'install-exec-prefix=$(exec_prefix)' \
	  'install-bindir=$(bindir)' \
	  'install-libdir=$(libdir)' \
	  'install-destdir=$(DESTDIR)'
