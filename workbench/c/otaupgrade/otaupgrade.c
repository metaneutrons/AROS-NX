/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: AROS CLI Dual-Bank OTA Firmware Flash Utility (C:OTAUpgrade).
*/

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include "../../../arch/riscv32-esp32p4/include/spiflash_ota.h"

#define BUFFER_CHUNK_SIZE   4096

static void print_progress(ULONG current, ULONG total) {
    ULONG pct = (total > 0) ? (current * 100 / total) : 0;
    PutStr("\r  [Flashing] [");
    for (ULONG i = 0; i < 25; i++) {
        if (i < pct / 4) PutStr("#");
        else PutStr("-");
    }
    PutStr("] ");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        PutStr("Usage: OTAUpgrade <firmware_file.bin> [REBOOT]\n");
        PutStr("Flashes a new AROS firmware image into the inactive OTA slot.\n");
        return RETURN_ERROR;
    }

    CONST_STRPTR filepath = argv[1];
    BOOL do_reboot = (argc >= 3 && (argv[2][0] == 'R' || argv[2][0] == 'r'));

    PutStr("\n=== AROS Dual-Bank OTA Upgrade Utility ===\n");
    int running = ota_get_running_slot();
    int target  = ota_get_target_slot();

    PutStr("Current Active Slot: ");
    PutStr((running == OTA_SLOT_A) ? "SLOT A (0x00020000)\n" : "SLOT B (0x00820000)\n");
    PutStr("Target Inactive Slot: ");
    PutStr((target == OTA_SLOT_A) ? "SLOT A (0x00020000)\n" : "SLOT B (0x00820000)\n");

    BPTR fh = Open(filepath, MODE_OLDFILE);
    if (!fh) {
        PutStr("ERROR: Could not open firmware file!\n");
        return RETURN_ERROR;
    }

    Seek(fh, 0, OFFSET_END);
    LONG filesize = Seek(fh, 0, OFFSET_BEGINNING);

    if (filesize <= 0 || filesize > (8 * 1024 * 1024)) {
        PutStr("ERROR: Invalid firmware image size!\n");
        Close(fh);
        return RETURN_ERROR;
    }

    UBYTE chunk_buf[BUFFER_CHUNK_SIZE];
    ULONG total_written = 0;

    PutStr("Writing firmware to flash partition...\n");

    while (total_written < (ULONG)filesize) {
        LONG bytes_read = Read(fh, chunk_buf, BUFFER_CHUNK_SIZE);
        if (bytes_read <= 0) break;

        if (ota_write_target_slot(total_written, chunk_buf, (ULONG)bytes_read) < 0) {
            PutStr("\nERROR: SPI Flash write error at offset ");
            Close(fh);
            return RETURN_ERROR;
        }

        total_written += bytes_read;
        print_progress(total_written, filesize);
    }

    Close(fh);
    PutStr("\n[+] Verification & Flash Write complete!\n");

    /* Activate target slot in otadata */
    if (ota_activate_target_slot() < 0) {
        PutStr("ERROR: Failed to update bootloader otadata partition!\n");
        return RETURN_ERROR;
    }

    PutStr("[+] Target slot activated successfully!\n");

    if (do_reboot) {
        PutStr("Rebooting now into new firmware...\n");
        ota_reboot();
    } else {
        PutStr("Type 'Reboot' or power-cycle to boot the new AROS version.\n");
    }

    return RETURN_OK;
}
