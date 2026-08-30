#!/usr/bin/env bash
set -euo pipefail

# One audited host contract for direct CMake builds and toolchain-consumer
# qualification.  Release payload contents remain independent of these host
# packages; they provide only the build programs and development headers that
# the checked-in CMake capabilities resolve explicitly.
case "${RUNNER_OS:-}" in
    Linux)
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            autoconf automake bison build-essential clang cmake flex gawk \
            libpng-dev libtool lld llvm make netpbm ninja-build patch python3 xz-utils
        ;;
    macOS)
        if [[ -z "${GITHUB_PATH:-}" ]]; then
            printf 'error: GITHUB_PATH is required on a macOS Actions runner\n' >&2
            exit 64
        fi
        brew install autoconf automake bison cmake coreutils gawk gettext gnu-sed libtool \
            lld llvm make netpbm ninja pkgconf python@3.14 texinfo xz
        printf '%s/bin\n' "$(brew --prefix bison)" >> "${GITHUB_PATH}"
        ;;
    *)
        printf 'error: unsupported GitHub Actions runner OS: %s\n' \
            "${RUNNER_OS:-<unset>}" >&2
        exit 64
        ;;
esac
