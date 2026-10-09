# Isolated bootloader console comparison

Diagnostic only; the normal bootloader project does not include these components.
No hardware fix or default configuration change is claimed.

`no_usb_secondary` wraps the second-stage caller of
`esp_rom_install_uart_printf`, preserves the original UART setup, then removes
only ROM output channel2 through `esp_rom_install_channel_putc(2, NULL)`.
UART INFO/error output, image checks, flash setup, partition selection and
watchdog configuration remain unchanged. ROM output before this hook cannot be
removed by it; AROS's direct-register USB console is independent of the hook.

With the existing IDF toolchain/Python environment active, run:

```sh
bash arch/riscv-esp32p4/tools/build-bootloader-console-comparison.sh \
  <patched-idf-root> <existing-board-idf-project> <new-evidence-directory>
```

The helper makes fresh baseline/candidate project and build directories, copies
the existing sdkconfig/defaults and rollback image, fixes compile-time metadata
with SOURCE_DATE_EPOCH, checks matching configs and source/reference hashes,
and checks size against the partition table. It does not edit the shared IDF
or reference build and contains no flashing operation. Failed directories must
be retained; use a new evidence directory for a corrected clean repeat.

Check final candidate disassembly: `bootloader_console_init` must call
`__wrap_esp_rom_install_uart_printf`; the wrapper must call the original ROM
setup first, then channel2 removal with a null callback. The fresh baseline
must retain the direct ROM setup call. WHOLE_ARCHIVE is necessary: a plain
component archive was compiled but failed to link the wrapped symbol.

Lost secondary USB bootloader narration is intentional; it is not proof that
the bootloader did not run. AROS retained phase reports and a first-byte warm
capture remain separate evidence. Only a freshly synchronized no-reader
physical comparison can test the cold-start hypothesis.

Bootloader writes are not covered by routine core/package authorization.
For JC1060, request explicit permission for the bootloader at0x2000, including
sector erasure0x2000..0x7fff. Before any approved write, identify the MAC,
read back the full affected range, verify the original image, preserve the
rollback and confirm the partition table at0x8000 stays untouched. A successful
build alone is not hardware acceptance.
