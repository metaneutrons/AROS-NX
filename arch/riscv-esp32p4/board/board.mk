# ESP32-P4 SoC builds select one board profile. D1001 stays the default
# until another board has passed its own boot gate.
#
# mmake does not notice a changed P4_BOARD, so each board needs its own
# build tree (README.md, "Board profiles").
P4_BOARD ?= d1001
P4_FLASH_LAYOUT ?= legacy

ifeq ($(filter $(P4_FLASH_LAYOUT),legacy ab),)
$(error Unsupported P4_FLASH_LAYOUT '$(P4_FLASH_LAYOUT)'; supported: legacy, ab)
endif
ifneq ($(words $(P4_FLASH_LAYOUT)),1)
$(error P4_FLASH_LAYOUT must be exactly one of: legacy, ab)
endif

P4_BOARD_LAYOUT_FLAGS :=
P4_BOARD_BOOTLOADER_OFFSET := 0x2000

# Guition JC1060P470C (shops: JC1060WP470C) comes with two JD9165 panel
# batches that need different init tables, so the board ID names the batch:
#   jc1060p470c-v1  the vendor's "Old_Panel" (its V2.x firmware images)
#   jc1060p470c-v2  the vendor's "New_Panel" (its V3.x firmware images)
# The vendor's "V2" sticker is not a reliable test: the first board here has
# none and its factory firmware holds the new table. Read the batch from the
# factory firmware. The bare name is refused so nobody gets a table by guess.
ifneq ($(filter jc1060p470c jc1060wp470c,$(P4_BOARD)),)
$(error P4_BOARD=$(P4_BOARD) is ambiguous: use jc1060p470c-v1 (old panel) or jc1060p470c-v2 (new panel))
endif

P4_BOARD_DIR := $(SRCDIR)/arch/riscv-esp32p4/bootloader/boards/$(P4_BOARD)

ifeq ($(P4_BOARD),d1001)
P4_BOARD_CPPFLAGS := -DP4_BOARD_D1001=1
P4_BOARD_FLASH_SIZE := 32MB
P4_BOARD_REV_MIN := 100
P4_BOARD_REV_MAX := 199
P4_BOARD_PARTITION_OFFSET := 0x8000
P4_BOARD_PKG_LIMIT := 4063232
P4_BOARD_CORE_LIMIT := 0x800000
P4_BOARD_FLASHDISK_SIZE_MB := 4
P4_BOARD_FLASHDISK_SIZE_BYTES := 0x00400000
P4_BOARD_FLASHDISK_OFFSET := 0xc00000
P4_BOARD_TOUCH_FW_FILENAME := gsl3670-d1001.fw
# Portable driver in the package (workbench/hidds/gsl3670), wired up with
# its expander reset and firmware by esp32p4board.resource.
P4_BOARD_TOUCH_MODULE := gsl3670
# The board's wiring code in esp32p4board.resource (board/d1001.c).
P4_BOARD_SETUP := d1001
P4_BOARD_PANEL_TABLE := jd9365_init
# Burst video, as the vendor firmware drives this panel (2026-10-04).
P4_DSI_BURST ?= 1
P4_BOARD_PARTITION_CSV := $(P4_BOARD_DIR)/partition-table.csv
P4_BOARD_SDKCONFIG := $(P4_BOARD_DIR)/sdkconfig.defaults
else ifneq ($(filter jc1060p470c-v1 jc1060p470c-v2,$(P4_BOARD)),)
# Both batches share the profile, the partition table and the sdkconfig.
P4_BOARD_DIR := $(SRCDIR)/arch/riscv-esp32p4/bootloader/boards/jc1060p470c
ifeq ($(P4_BOARD),jc1060p470c-v1)
P4_BOARD_CPPFLAGS := -DP4_BOARD_JC1060P470C=1 -DP4_JC1060_PANEL_V1=1
else
P4_BOARD_CPPFLAGS := -DP4_BOARD_JC1060P470C=1 -DP4_JC1060_PANEL_V2=1
endif
P4_BOARD_FLASH_SIZE := 16MB
P4_BOARD_REV_MIN := 100
P4_BOARD_REV_MAX := 199
P4_BOARD_PARTITION_OFFSET := 0x8000
P4_BOARD_PKG_LIMIT := 4063232
P4_BOARD_CORE_LIMIT := 0x800000
P4_BOARD_FLASHDISK_SIZE_MB := 4
P4_BOARD_FLASHDISK_SIZE_BYTES := 0x00400000
P4_BOARD_FLASHDISK_OFFSET := 0xc00000
# The GT911 runs from its own ROM; nothing to install.
P4_BOARD_TOUCH_FW_FILENAME :=
# Portable driver in the package (workbench/hidds/gt911), wired up by
# esp32p4board.resource.
P4_BOARD_TOUCH_MODULE := gt911
# The board's wiring code in esp32p4board.resource (board/jc1060p470c.c).
P4_BOARD_SETUP := jc1060p470c
P4_BOARD_PANEL_TABLE := jd9165_init
# Burst video, as ESP-IDF drives this panel. Non-burst shows the image
# displaced on this board (2026-10-03); P4_DSI_BURST=0 still selects it.
P4_DSI_BURST ?= 1
P4_BOARD_PARTITION_CSV := $(P4_BOARD_DIR)/partition-table.csv
P4_BOARD_SDKCONFIG := $(P4_BOARD_DIR)/sdkconfig.defaults
else
$(error Unsupported ESP32-P4 board '$(P4_BOARD)'; supported: d1001, jc1060p470c-v1, jc1060p470c-v2)
endif

# A/B remains opt-in. It relocates the partition table and selects a matched
# table/config pair; legacy keeps every existing value above unchanged.
ifeq ($(P4_FLASH_LAYOUT),ab)
P4_BOARD_LAYOUT_FLAGS := -DP4_FLASH_LAYOUT_AB=1
P4_BOARD_CPPFLAGS += $(P4_BOARD_LAYOUT_FLAGS)
P4_BOARD_PARTITION_OFFSET := 0x20000
P4_BOARD_BOOT_REGION_BYTES := 0x40000
P4_BOARD_KERNEL_SLOT_BYTES := 0x80000
P4_BOARD_BSP_SLOT_BYTES := 0x600000
P4_BOARD_PKG_LIMIT := 0x600000
P4_BOARD_CORE_LIMIT := 0x80000
P4_BOARD_FLASHDISK_SIZE_BYTES := 0x002c0000
# Informational MiB value only; formatter callers must use exact bytes.
P4_BOARD_FLASHDISK_SIZE_MB := 2.75
P4_BOARD_FLASHDISK_OFFSET := 0xd40000
P4_BOARD_PARTITION_CSV := $(P4_BOARD_DIR)/partition-table-ab.csv
P4_BOARD_SDKCONFIG := $(P4_BOARD_DIR)/ab-sdkconfig.defaults
else
P4_BOARD_BOOT_REGION_BYTES := 0
P4_BOARD_KERNEL_SLOT_BYTES := 0
P4_BOARD_BSP_SLOT_BYTES := 0
endif

# mmake does not invalidate objects when board/layout CPP flags change.
# Bind real build trees once; profile-inspection Make snippets have no TOP.
# Other targets also visit these public headers through includes-copy. Their
# build trees must not acquire an ESP32-P4 board/layout identity.
ifeq ($(AROS_TARGET_ARCH)-$(AROS_TARGET_CPU),esp32p4-riscv)
ifneq ($(strip $(TOP)),)
P4_BUILD_PROFILE_CHECK := $(shell $(PYTHON) "$(SRCDIR)/arch/riscv-esp32p4/tools/check-build-profile.py" "$(TOP)" "$(P4_BOARD)" "$(P4_FLASH_LAYOUT)" 2>&1)
ifneq ($(strip $(P4_BUILD_PROFILE_CHECK)),OK)
$(error $(P4_BUILD_PROFILE_CHECK))
endif
endif
endif
