/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Calibrate the MSPI PLL before the kernel is entered.

    The kernel brings PSRAM up itself, and every register it writes to do so
    has been read against ESP-IDF's esp_psram_impl_enable and matches it.  One
    thing it cannot do is start this PLL's calibration.  Measured on a
    cold-started board: MSPI_CAL_STOP cleared exactly as IDF clears it, every
    analogue register reading back the value written, the divider byte 0x99
    that 400 MHz asks for - and MSPI_CAL_END absent after a million polls.  The
    calibration was not failing, it was never starting, and no arrangement of
    that sequence in the kernel changed it.

    What did work was a boot that followed firmware which had already
    calibrated: the bit was still set from that run, because the PMU and this
    register survive a CPU reset.  That is not something a port may depend on.
    Nobody installing AROS should have to run another firmware first.

    So the calibration happens here, in the bootloader, with IDF's own
    implementation of it.  That is a defensible place for it rather than a
    workaround: this bootloader already calibrates the CPU and system PLLs
    before the kernel sees the machine - both arrive with their CAL_END bits
    set - and the MSPI PLL is the same kind of analogue silicon setup, left out
    only because PSRAM is off in this bootloader's configuration.  The kernel
    still configures the controller, the pins, the timing and the chip; what it
    inherits is one calibrated PLL, exactly as it inherits two already.

    The kernel's own attempt is kept.  It is harmless when the bit is already
    set - it polls once and continues - and it stays the thing to fix if the
    calibration is ever understood well enough to run it from cold there.
*/

#include "sdkconfig.h"
#include "esp_log.h"
#include "soc/soc.h"
#include "hal/clk_tree_ll.h"
#include "hal/regi2c_ctrl_ll.h"

/* What the kernel's PSRAM bring-up divides down from, and the only rate its
   dividers hit exactly for 20, 25, 40, 50, 80, 100 and 200 MHz. */
#define AROS_MPLL_HZ    400000000
#define AROS_XTAL_MHZ   40

static const char *TAG = "aros_mpll";

void bootloader_after_init(void)
{
    /* Powered from here whether or not it already was: the calibration below
       needs the block up, and IDF's own PSRAM path powers it the same way. */
    clk_ll_mpll_enable();

    /* The underscore form, because the wrapper macro needs an RCC atomic
       environment that a bootloader does not set up.  Nothing else runs here,
       so there is nothing to be atomic against. */
    _regi2c_ctrl_ll_master_enable_clock(true);
    regi2c_ctrl_ll_mpll_calibration_start();
    clk_ll_mpll_set_config(AROS_MPLL_HZ / 1000000, AROS_XTAL_MHZ);

    /*
     * Bounded, where ESP-IDF waits forever.
     *
     * IDF can afford the unbounded wait because it only ever runs this in a
     * state where the calibration completes.  On this board from cold it does
     * not - measured here with IDF's own sequence, in the bootloader, which is
     * as close to IDF's own conditions as this port gets - and a bootloader
     * that spins forever is a board that has to be recovered over USB.  So it
     * is given a bound and the kernel is left to report what it inherited.
     */
    {
        int spin = 2000000;

        while (!regi2c_ctrl_ll_mpll_calibration_is_done() && --spin)
            ;
        if (spin)
            ESP_EARLY_LOGI(TAG, "mspi pll calibrated at %d MHz for the kernel",
                           AROS_MPLL_HZ / 1000000);
        else
            ESP_EARLY_LOGW(TAG, "mspi pll calibration did not complete;"
                                " the kernel will report no PSRAM");
    }

    regi2c_ctrl_ll_mpll_calibration_stop();

    /*
     * The bus clock stays on.  Turning it off here made the rest of the
     * bootloader assert inside the ROM's regi2c helper, which checks that it
     * is enabled - IDF's own disable is balanced against a reference count
     * that a bootloader hook is not part of.  Leaving it on costs nothing and
     * the kernel enables it again anyway.
     */
}
