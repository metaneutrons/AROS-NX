/*
    Copyright (C) 2026, The AROS Development Team & metaneutrons.
    Author: metaneutrons <436979+metaneutrons@users.noreply.github.com>

    Desc: AROS CLI Dual-Bank OTA Firmware Flash Utility (C:OTAUpgrade).
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include "../../../arch/riscv32-esp32p4/include/spiflash_ota.h"

#define TEMPLATE            "FILE/A,REBOOT/S,FORCE/S"
#define BUFFER_CHUNK_SIZE   4096

enum {
    ARG_FILE,
    ARG_REBOOT,
    ARG_FORCE,
    NUM_ARGS
};

static const char version_tag[] = "$VER: OTAUpgrade 1.0 (19.08.2026) by metaneutrons\r\n";

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
    struct RDArgs *rdargs;
    IPTR args[NUM_ARGS] = {0};
    LONG ret = RETURN_OK;

    rdargs = ReadArgs(TEMPLATE, args, NULL);
    if (!rdargs) {
        PrintFault(IoErr(), "OTAUpgrade");
        return RETURN_ERROR;
    }

    CONST_STRPTR filepath = (CONST_STRPTR)args[ARG_FILE];
    BOOL do_reboot = (args[ARG_REBOOT] != 0);
    BOOL force_mode = (args[ARG_FORCE] != 0);

    PutStr("\n=== AROS Dual-Bank OTA Upgrade Utility ===\n");
    int running = ota_get_running_slot();
    int target  = ota_get_target_slot();

    PutStr("Current Active Slot: ");
    PutStr((running == OTA_SLOT_A) ? "SLOT A (0x00020000)\n" : "SLOT B (0x00820000)\n");
    PutStr("Target Inactive Slot: ");
    PutStr((target == OTA_SLOT_A) ? "SLOT A (0x00020000)\n" : "SLOT B (0x00820000)\n");

    BPTR fh = Open(filepath, MODE_OLDFILE);
    if (!fh) {
        PutStr("ERROR: Could not open firmware file: ");
        PutStr(filepath);
        PutStr("\n");
        FreeArgs(rdargs);
        return RETURN_ERROR;
    }

    Seek(fh, 0, OFFSET_END);
    LONG filesize = Seek(fh, 0, OFFSET_BEGINNING);

    if (filesize <= 0 || filesize > (8 * 1024 * 1024)) {
        PutStr("ERROR: Invalid firmware image size (must be >0 and <= 8MB)!\n");
        Close(fh);
        FreeArgs(rdargs);
        return RETURN_ERROR;
    }

    /* Verify ESP32-P4 Image Header Magic byte 0xE9 */
    UBYTE header_magic = 0;
    if (Read(fh, &header_magic, 1) != 1 || (header_magic != 0xE9 && !force_mode)) {
        PutStr("ERROR: File is not a valid ESP32-P4 application image (Magic 0xE9 mismatch)!\n");
        PutStr("Use FORCE switch to bypass header check.\n");
        Close(fh);
        FreeArgs(rdargs);
        return RETURN_ERROR;
    }
    Seek(fh, 0, OFFSET_BEGINNING);

    UBYTE chunk_buf[BUFFER_CHUNK_SIZE];
    ULONG total_written = 0;

    PutStr("Writing firmware to flash partition...\n");

    while (total_written < (ULONG)filesize) {
        LONG bytes_read = Read(fh, chunk_buf, BUFFER_CHUNK_SIZE);
        if (bytes_read <= 0) break;

        if (ota_write_target_slot(total_written, chunk_buf, (ULONG)bytes_read) < 0) {
            PutStr("\nERROR: SPI Flash write error at offset!\n");
            ret = RETURN_ERROR;
            break;
        }

        total_written += bytes_read;
        print_progress(total_written, filesize);
    }

    Close(fh);

    if (ret == RETURN_OK) {
        PutStr("\n[+] Verification & Flash Write complete!\n");

        /* Activate target slot in otadata */
        if (ota_activate_target_slot() < 0) {
            PutStr("ERROR: Failed to update bootloader otadata partition!\n");
            ret = RETURN_ERROR;
        } else {
            PutStr("[+] Target slot activated successfully!\n");
            if (do_reboot) {
                PutStr("Rebooting now into new firmware...\n");
                ota_reboot();
            } else {
                PutStr("Type 'Reboot' or power-cycle to boot the new AROS version.\n");
            }
        }
    }

    FreeArgs(rdargs);
    return ret;
}
