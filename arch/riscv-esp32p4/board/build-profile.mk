include $(SRCDIR)/arch/riscv-esp32p4/board/board.mk

# mmake does not invalidate objects when board/layout CPP flags change.
# Bind real ESP32-P4 build trees once. Source-only SDK inventory maps this
# wrapper to the pure board.mk profile through its sealed include projection.
ifeq ($(AROS_TARGET_ARCH)-$(AROS_TARGET_CPU),esp32p4-riscv)
ifneq ($(strip $(TOP)),)
P4_BUILD_PROFILE_CHECK := $(shell $(PYTHON) "$(SRCDIR)/arch/riscv-esp32p4/tools/check-build-profile.py" "$(TOP)" "$(P4_BOARD)" "$(P4_FLASH_LAYOUT)" 2>&1)
ifneq ($(strip $(P4_BUILD_PROFILE_CHECK)),OK)
$(error $(P4_BUILD_PROFILE_CHECK))
endif
endif
endif
