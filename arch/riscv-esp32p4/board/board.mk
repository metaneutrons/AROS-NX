# ESP32-P4 SoC builds select one board profile. D1001 stays the default
# until another board has passed its own boot gate.
#
# mmake does not notice a changed P4_BOARD, so each board needs its own
# build tree (README.md, "Board profiles").
P4_BOARD ?= d1001

# Shops list the Guition board as JC1060WP470C; its own documents say
# JC1060P470C (_I_W_Y). Accept both, build one profile.
ifeq ($(P4_BOARD),jc1060wp470c)
override P4_BOARD := jc1060p470c
endif

P4_BOARD_DIR := $(SRCDIR)/arch/riscv-esp32p4/bootloader/boards/$(P4_BOARD)

ifeq ($(P4_BOARD),d1001)
P4_BOARD_CPPFLAGS := -DP4_BOARD_D1001=1
P4_BOARD_FLASH_SIZE := 32MB
P4_BOARD_REV_MIN := 100
P4_BOARD_REV_MAX := 199
P4_BOARD_PARTITION_OFFSET := 0x8000
P4_BOARD_PKG_LIMIT := 4063232
P4_BOARD_FLASHDISK_SIZE_MB := 4
P4_BOARD_FLASHDISK_OFFSET := 0xc00000
P4_BOARD_TOUCH_FW_FILENAME := gsl3670-d1001.fw
P4_BOARD_TOUCH_DRIVER := gsl3670
P4_BOARD_PANEL_TABLE := jd9365_init
P4_BOARD_PARTITION_CSV := $(P4_BOARD_DIR)/partition-table.csv
P4_BOARD_SDKCONFIG := $(P4_BOARD_DIR)/sdkconfig.defaults
else ifeq ($(P4_BOARD),jc1060p470c)
# Two JD9165 panel batches with different init tables are fitted; a "V2"
# label means the new one. P4_JC1060_PANEL=old selects the other table.
P4_JC1060_PANEL ?= new
ifeq ($(P4_JC1060_PANEL),new)
P4_BOARD_CPPFLAGS := -DP4_BOARD_JC1060P470C=1
else ifeq ($(P4_JC1060_PANEL),old)
P4_BOARD_CPPFLAGS := -DP4_BOARD_JC1060P470C=1 -DP4_JC1060_PANEL_OLD=1
else
$(error P4_JC1060_PANEL must be 'new' or 'old', not '$(P4_JC1060_PANEL)')
endif
P4_BOARD_FLASH_SIZE := 16MB
P4_BOARD_REV_MIN := 100
P4_BOARD_REV_MAX := 199
P4_BOARD_PARTITION_OFFSET := 0x8000
P4_BOARD_PKG_LIMIT := 4063232
P4_BOARD_FLASHDISK_SIZE_MB := 4
P4_BOARD_FLASHDISK_OFFSET := 0xc00000
# The GT911 runs from its own ROM; nothing to install.
P4_BOARD_TOUCH_FW_FILENAME :=
P4_BOARD_TOUCH_DRIVER := gt911
P4_BOARD_PANEL_TABLE := jd9165_init
P4_BOARD_PARTITION_CSV := $(P4_BOARD_DIR)/partition-table.csv
P4_BOARD_SDKCONFIG := $(P4_BOARD_DIR)/sdkconfig.defaults
else
$(error Unsupported ESP32-P4 board '$(P4_BOARD)'; supported: d1001, jc1060p470c)
endif
