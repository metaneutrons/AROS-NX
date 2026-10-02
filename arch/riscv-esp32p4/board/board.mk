# ESP32-P4 SoC builds select one board profile. Keep D1001 as the default
# until another board has passed its own boot gate.
P4_BOARD ?= d1001

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
P4_BOARD_PARTITION_CSV := $(SRCDIR)/arch/riscv-esp32p4/bootloader/partition-table.csv
P4_BOARD_SDKCONFIG := $(SRCDIR)/arch/riscv-esp32p4/bootloader/project/sdkconfig.defaults
else
$(error Unsupported ESP32-P4 board '$(P4_BOARD)'; supported: d1001)
endif
