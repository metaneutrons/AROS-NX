/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    Desc: ESP32-P4 GPIO Matrix & gpio.resource Definitions.
*/

#ifndef RISCV32_GPIO_P4_H
#define RISCV32_GPIO_P4_H

#include "types.h"

/* ESP32-P4 GPIO Controller Base Address */
#define ESP32P4_GPIO_BASE               0x50090000UL
#define ESP32P4_IO_MUX_BASE             0x50091000UL

/* GPIO Matrix Registers */
#define GPIO_OUT_REG                    0x0004
#define GPIO_OUT_W1TS_REG               0x0008  /* Set High */
#define GPIO_OUT_W1TC_REG               0x000C  /* Set Low (Clear) */
#define GPIO_ENABLE_REG                 0x0020  /* Output Enable */
#define GPIO_ENABLE_W1TS_REG            0x0024
#define GPIO_ENABLE_W1TC_REG            0x0028
#define GPIO_IN_REG                     0x003C  /* Input Read */
#define GPIO_STATUS_REG                 0x0044  /* Interrupt Status */
#define GPIO_STATUS_W1TC_REG            0x004C
#define GPIO_PIN_REG(n)                 (0x0088 + (n) * 4)

/* Standard AROS Function Modes */
#define GPIO_FUNC_INPUT                 0
#define GPIO_FUNC_OUTPUT                1
#define GPIO_FUNC_ALT                   2

/* Standard AROS Pull Modes */
#define GPIO_PULL_NONE                  0
#define GPIO_PULL_UP                    1
#define GPIO_PULL_DOWN                  2

/* Standard AROS Interrupt Triggers */
#define GPIO_TRIG_RISING                1
#define GPIO_TRIG_FALLING               2
#define GPIO_TRIG_BOTH                  3

/* D1001 Dedicated Pins */
#define D1001_GPIO_POWER_BUTTON         35
#define D1001_GPIO_DISP_BACKLIGHT       26
#define D1001_GPIO_BUZZER_PWM           27

/* Driver / Resource API */
int   gpio_p4_init(void);
void  GPIOSetFunc(ULONG pin, ULONG mode);
void  GPIOSet(ULONG pin, ULONG val);
ULONG GPIOGet(ULONG pin);
void  GPIOSetPull(ULONG pin, ULONG pull_mode);
LONG  GPIOAttachInterrupt(ULONG pin, void (*handler)(ULONG pin, void *data), void *data, ULONG trigger);

#endif /* RISCV32_GPIO_P4_H */
