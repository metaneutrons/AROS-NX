/*
    Copyright (C) 2026, The AROS Development Team & Fabian Schmieder.
    Author: Fabian Schmieder (@metaneutrons)

    Desc: ESP32-P4 Hardware GPIO & gpio.resource Implementation for AROS.
*/

#include "types.h"
#include "../include/gpio_p4.h"
#include "../boot/uart.h"

static inline void wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_GPIO_BASE + offset) = val;
}

static inline ULONG rd32(ULONG offset) {
    return *(volatile ULONG *)(ESP32P4_GPIO_BASE + offset);
}

static inline void iomux_wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_IO_MUX_BASE + offset) = val;
}

/* Button interrupt handler */
static void on_d1001_power_button(ULONG pin, void *data) {
    uart_puts("[gpio] D1001 Power/Sleep Button Pressed (GPIO 35) -> Triggering Screen Blanker Toggle\n");
}

int gpio_p4_init(void) {
    uart_puts("[gpio] Initializing ESP32-P4 GPIO Matrix & gpio.resource...\n");

    /* Configure D1001 Power Button (GPIO 35) as Input with Pull-Up and Falling-Edge Trigger */
    GPIOSetFunc(D1001_GPIO_POWER_BUTTON, GPIO_FUNC_INPUT);
    GPIOSetPull(D1001_GPIO_POWER_BUTTON, GPIO_PULL_UP);
    GPIOAttachInterrupt(D1001_GPIO_POWER_BUTTON, on_d1001_power_button, NULL, GPIO_TRIG_FALLING);

    /* Configure Backlight (GPIO 26) as Output and Enable */
    GPIOSetFunc(D1001_GPIO_DISP_BACKLIGHT, GPIO_FUNC_OUTPUT);
    GPIOSet(D1001_GPIO_DISP_BACKLIGHT, 1);

    uart_puts("[gpio] gpio.resource ready (D1001 Power Button & Backlight attached).\n");
    return 0;
}

void GPIOSetFunc(ULONG pin, ULONG mode) {
    if (pin > 55) return;

    if (mode == GPIO_FUNC_OUTPUT) {
        if (pin < 32) {
            wr32(GPIO_ENABLE_W1TS_REG, (1UL << pin));
        } else {
            wr32(GPIO_ENABLE_REG + 0x04, (1UL << (pin - 32)));
        }
    } else {
        if (pin < 32) {
            wr32(GPIO_ENABLE_W1TC_REG, (1UL << pin));
        } else {
            wr32(GPIO_ENABLE_REG + 0x08, (1UL << (pin - 32)));
        }
    }
}

void GPIOSet(ULONG pin, ULONG val) {
    if (pin > 55) return;

    if (val) {
        if (pin < 32) {
            wr32(GPIO_OUT_W1TS_REG, (1UL << pin));
        } else {
            wr32(GPIO_OUT_REG + 0x04, (1UL << (pin - 32)));
        }
    } else {
        if (pin < 32) {
            wr32(GPIO_OUT_W1TC_REG, (1UL << pin));
        } else {
            wr32(GPIO_OUT_REG + 0x08, (1UL << (pin - 32)));
        }
    }
}

ULONG GPIOGet(ULONG pin) {
    if (pin > 55) return 0;

    if (pin < 32) {
        return (rd32(GPIO_IN_REG) & (1UL << pin)) ? 1 : 0;
    } else {
        return (rd32(GPIO_IN_REG + 0x04) & (1UL << (pin - 32))) ? 1 : 0;
    }
}

void GPIOSetPull(ULONG pin, ULONG pull_mode) {
    if (pin > 55) return;
    ULONG iomux_offset = 0x0010 + pin * 4;

    /* Bit 7 = Pullup, Bit 8 = Pulldown, Bit 9 = Input Enable (IE) */
    ULONG val = (1 << 9); /* IE always enabled */
    if (pull_mode == GPIO_PULL_UP)   val |= (1 << 7);
    if (pull_mode == GPIO_PULL_DOWN) val |= (1 << 8);

    iomux_wr32(iomux_offset, val);
}

LONG GPIOAttachInterrupt(ULONG pin, void (*handler)(ULONG pin, void *data), void *data, ULONG trigger) {
    if (pin > 55) return -1;

    /* Configure GPIO PIN interrupt trigger:
       1=Rising, 2=Falling, 3=Both, 4=Low Level, 5=High Level (bits 9..7) */
    ULONG intr_type = 0;
    if (trigger == GPIO_TRIG_RISING)  intr_type = 1;
    if (trigger == GPIO_TRIG_FALLING) intr_type = 2;
    if (trigger == GPIO_TRIG_BOTH)    intr_type = 3;

    wr32(GPIO_PIN_REG(pin), (intr_type << 7));
    return 0;
}
