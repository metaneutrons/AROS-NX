/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32P4_SDMMC_REGISTERS_H
#define ESP32P4_SDMMC_REGISTERS_H

/* ESP32-P4 high-power peripheral windows. */
#define P4_HPPERIPH0_BASE              0x50000000UL
#define P4_HPPERIPH1_BASE              0x500C0000UL
#define P4_LPAON_BASE                  0x50110000UL

/* One Synopsys DesignWare MMC host, with two independently clocked slots. */
#define P4_SDMMC_BASE                  (P4_HPPERIPH0_BASE + 0x83000)
#define P4SD_CTRL                      0x0000
#define  P4SD_CTRL_RESET_ALL           0x00000007UL
#define  P4SD_CTRL_FIFO_RESET          (1UL << 1)
#define  P4SD_CTRL_DMA_RESET           (1UL << 2)
#define  P4SD_CTRL_DMA_ENABLE          (1UL << 5)
/* Bit 25, not 26.  ESP-IDF's own register description for this peripheral
   (components/soc/esp32p4/register/hw_ver1/soc/sdmmc_struct.h) places
   card_voltage_a on 16..19, card_voltage_b on 20..23, enable_od_pullup on 24,
   use_internal_dma on 25 and reserved3 from 26 up.  With bit 26 the internal
   DMAC was never selected on the controller side while BMOD.DE still ran the
   descriptor engine. */
#define  P4SD_CTRL_USE_INTERNAL_DMA    (1UL << 25)
#define P4SD_CLKDIV                    0x0008
#define P4SD_CLKSRC                    0x000C
#define P4SD_CLKENA                    0x0010
#define P4SD_TMOUT                     0x0014
#define P4SD_CTYPE                     0x0018
#define  P4SD_CTYPE_4BIT_SLOT0         (1UL << 0)
#define  P4SD_CTYPE_8BIT_SLOT0         (1UL << 16)
#define P4SD_BLKSIZ                    0x001C
#define P4SD_BYTCNT                    0x0020
#define P4SD_INTMASK                   0x0024
#define P4SD_CMDARG                    0x0028
#define P4SD_CMD                       0x002C
#define  P4SD_CMD_RESP                 (1UL << 6)
#define  P4SD_CMD_RESP_LONG            (1UL << 7)
#define  P4SD_CMD_RESP_CRC             (1UL << 8)
#define  P4SD_CMD_DATA                 (1UL << 9)
#define  P4SD_CMD_WRITE                (1UL << 10)
#define  P4SD_CMD_SEND_AUTO_STOP        (1UL << 12)
#define  P4SD_CMD_STOP_ABORT           (1UL << 14)
#define  P4SD_CMD_WAIT_DATA            (1UL << 13)
#define  P4SD_CMD_SEND_INIT            (1UL << 15)
#define  P4SD_CMD_UPDATE_CLK           (1UL << 21)
#define  P4SD_CMD_USE_HOLD             (1UL << 29)
#define  P4SD_CMD_START                (1UL << 31)
#define P4SD_RESP0                     0x0030
#define P4SD_RESP1                     0x0034
#define P4SD_RESP2                     0x0038
#define P4SD_RESP3                     0x003C
#define P4SD_MINTSTS                   0x0040
#define P4SD_RINTSTS                   0x0044
/* Command/data W1C bits are separate from the two SDIO card interrupts.
 * Storage must never acknowledge bits16/17 belonging to the radio client. */
#define P4SD_INT_CMD_DATA_MASK          0x0000FFFFUL
#define  P4SD_INT_RESP_ERR             (1UL << 1)
#define  P4SD_INT_CMD_DONE             (1UL << 2)
#define  P4SD_INT_DATA_OVER            (1UL << 3)
#define  P4SD_INT_RXDR                 (1UL << 5)
#define  P4SD_INT_RCRC                 (1UL << 6)
#define  P4SD_INT_DCRC                 (1UL << 7)
#define  P4SD_INT_RTO                  (1UL << 8)
#define  P4SD_INT_DTO                  (1UL << 9)
#define  P4SD_INT_HTO                  (1UL << 10)
#define  P4SD_INT_FRUN                 (1UL << 11)
#define  P4SD_INT_HLE                  (1UL << 12)
#define  P4SD_INT_SBE                  (1UL << 13)
#define  P4SD_INT_EBE                  (1UL << 15)
#define  P4SD_INT_CMD_ERRORS           (P4SD_INT_RESP_ERR | P4SD_INT_RCRC | \
                                        P4SD_INT_RTO | P4SD_INT_HLE)
#define  P4SD_INT_DATA_ERRORS          (P4SD_INT_DCRC | P4SD_INT_DTO | \
                                        P4SD_INT_HTO | P4SD_INT_FRUN | \
                                        P4SD_INT_SBE | P4SD_INT_EBE)
#define P4SD_STATUS                    0x0048
#define  P4SD_STATUS_FIFO_EMPTY        (1UL << 2)
#define  P4SD_STATUS_FIFO_FULL         (1UL << 3)
#define  P4SD_STATUS_DATA_BUSY         (1UL << 9)
#define  P4SD_STATUS_FIFO_COUNT_S      17
#define  P4SD_STATUS_FIFO_COUNT_M      0x1FFFUL
#define P4SD_FIFOTH                    0x004C
#define  P4SD_FIFOTH_RX_WMARK          (127UL << 16)
#define  P4SD_FIFOTH_DMA_MSIZE_8       (2UL << 28)
#define P4SD_RST_N                     0x0078
#define P4SD_TCBCNT                    0x005C
#define P4SD_TBBCNT                    0x0060
#define P4SD_BMOD                      0x0080
#define  P4SD_BMOD_SWR                 (1UL << 0)
#define  P4SD_BMOD_FB                  (1UL << 1)
#define  P4SD_BMOD_DE                  (1UL << 7)
#define  P4SD_BMOD_PBL_8               (2UL << 8)
#define P4SD_PLDMND                    0x0084
#define P4SD_DBADDR                    0x0088
#define P4SD_IDSTS                     0x008C
#define  P4SD_IDSTS_TI                 (1UL << 0)
#define  P4SD_IDSTS_RI                 (1UL << 1)
#define  P4SD_IDSTS_FBE                (1UL << 2)
#define  P4SD_IDSTS_DU                 (1UL << 4)
#define  P4SD_IDSTS_CES                (1UL << 5)
#define  P4SD_IDSTS_NI                 (1UL << 8)
#define  P4SD_IDSTS_ERRORS             (P4SD_IDSTS_FBE | P4SD_IDSTS_DU | \
                                         P4SD_IDSTS_CES)
#define P4SD_IDINTEN                   0x0090
#define  P4SD_IDINTEN_TI                (1UL << 0)
#define  P4SD_IDINTEN_RI                (1UL << 1)
#define  P4SD_IDINTEN_NI                (1UL << 8)
#define P4SD_DSCADDR                   0x0094
#define P4SD_BUFADDR                   0x0098
#define P4SD_CARDTHRCTL                0x0100
#define P4SD_BUFFIFO                   0x0200
#define P4SD_CLK_EDGE_SEL              0x0800
/* SDMMC module gate, PLL160M divider and reset. */
#define P4_HP_SYS_CLKRST_BASE          (P4_HPPERIPH1_BASE + 0x26000)
#define P4_HP_SOC_CLK_CTRL1            0x0018
#define  P4_HP_SDMMC_CLK_EN            (1UL << 14)
#define P4_HP_REF_CLK_CTRL2            0x002C
#define  P4_HP_REF_160M_CLK_EN         (1UL << 0)
#define P4_HP_PERI_CLK_CTRL01          0x0034
#define  P4_HP_SDIO_HS_MODE            (1UL << 22)
#define  P4_HP_SDIO_CLK_SRC            (1UL << 23)
#define  P4_HP_SDIO_CLK_EN             (1UL << 24)
#define P4_HP_PERI_CLK_CTRL02          0x0038
#define  P4_HP_SDIO_UPDATE             (1UL << 8)
#define  P4_HP_SDIO_EDGE_L_S           9
#define  P4_HP_SDIO_EDGE_H_S           13
#define  P4_HP_SDIO_EDGE_N_S           17
#define  P4_HP_SDIO_DRV_EDGE_S         23
#define  P4_HP_SDIO_SLF_EN             (1UL << 27)
#define  P4_HP_SDIO_DRV_EN             (1UL << 28)
#define  P4_HP_SDIO_SAM_EN             (1UL << 29)
#define  P4_HP_SDIO_FIELDS_M           0x3FFFFF00UL

#define P4_LP_CLKRST_BASE              (P4_LPAON_BASE + 0x1000)
#define P4_LP_SDMMC_RST_CTRL           0x004C
#define  P4_LP_SDMMC_RST_EN            (1UL << 28)


#define P4SD_CMD_SLOT_S 16

#endif
