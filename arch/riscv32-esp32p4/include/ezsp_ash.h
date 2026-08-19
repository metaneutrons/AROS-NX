/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Clean-room ASH (Asynchronous Serial Host) & EZSP (EmberZNet Serial Protocol)
          v8/v9/v12 Parser & Zigbee Cluster Library (ZCL) for ESP32-C6 / D1001.
*/

#ifndef RISCV32_EZSP_ASH_H
#define RISCV32_EZSP_ASH_H

#include "types.h"

/*
 * ASH Frame Control Byte Constants (Silicon Labs ASH Protocol)
 */
#define ASH_FRAME_FLAG                  0x7E    /* Frame Delimiter */
#define ASH_FRAME_ESC                   0x7D    /* Byte Stuffing Escape */
#define ASH_FRAME_XON                   0x11    /* Software Flow Control XON */
#define ASH_FRAME_XOFF                  0x13    /* Software Flow Control XOFF */
#define ASH_FRAME_SUBSTITUTE            0x1A

/* ASH Frame Types */
#define ASH_CTRL_DATA                   0x00    /* 0b0xxx xxxx: Data frame with ACK/Seq */
#define ASH_CTRL_ACK                    0x80    /* 0b100x xxxx: ACK frame */
#define ASH_CTRL_NAK                    0xA0    /* 0b101x xxxx: NAK frame */
#define ASH_CTRL_RST                    0xC0    /* Reset request */
#define ASH_CTRL_RSTACK                 0xC1    /* Reset acknowledge */
#define ASH_CTRL_ERROR                  0xC2    /* Error notification */

/*
 * Standard EZSP Command IDs (Zigbee 3.0 Network Co-Processor)
 */
#define EZSP_CMD_VERSION                0x0000  /* Check NCP Protocol Version */
#define EZSP_CMD_NETWORK_INIT           0x0017  /* Resume Network from NVM */
#define EZSP_CMD_FORM_NETWORK           0x001E  /* Create Zigbee Coordinator Network */
#define EZSP_CMD_JOIN_NETWORK           0x001F  /* Join Existing Zigbee Network */
#define EZSP_CMD_PERMIT_JOINING         0x0022  /* Open Network for Pairing (e.g. 60s) */
#define EZSP_CMD_SEND_UNICAST           0x0034  /* Send ZCL Unicast Message */
#define EZSP_CMD_SEND_BROADCAST         0x0036  /* Send ZCL Broadcast Message */

/*
 * Standard Zigbee Cluster Library (ZCL) Cluster IDs
 */
#define ZCL_CLUSTER_BASIC               0x0000
#define ZCL_CLUSTER_POWER_CONFIG        0x0001
#define ZCL_CLUSTER_IDENTIFY            0x0003
#define ZCL_CLUSTER_GROUPS              0x0004
#define ZCL_CLUSTER_SCENES              0x0005
#define ZCL_CLUSTER_ON_OFF              0x0006  /* Relays, Plugs, Lights */
#define ZCL_CLUSTER_LEVEL_CONTROL       0x0008  /* Dimmers, Motor position */
#define ZCL_CLUSTER_COLOR_CONTROL       0x0300  /* RGB, CCT, Hue/Saturation */
#define ZCL_CLUSTER_ILLUMINANCE         0x0400  /* Lux Sensor */
#define ZCL_CLUSTER_TEMPERATURE         0x0402  /* Temperature Sensor */
#define ZCL_CLUSTER_HUMIDITY            0x0405  /* Relative Humidity Sensor */
#define ZCL_CLUSTER_OCCUPANCY           0x0406  /* PIR Motion Sensor */
#define ZCL_CLUSTER_IAS_ZONE            0x0500  /* Contact / Alarm Sensors */

/* ZCL Command IDs */
#define ZCL_CMD_OFF                     0x00
#define ZCL_CMD_ON                      0x01
#define ZCL_CMD_TOGGLE                  0x02
#define ZCL_CMD_MOVE_TO_LEVEL           0x00
#define ZCL_CMD_MOVE_TO_COLOR_TEMP      0x0A

/* Function Prototypes */
UWORD ash_encode_frame(const UBYTE *payload, UWORD len, UBYTE *out_buf, UWORD out_max, UBYTE control);
WORD  ash_decode_frame(const UBYTE *in_buf, UWORD in_len, UBYTE *payload, UWORD max_payload, UBYTE *out_ctrl);
UWORD zcl_build_on_off_cmd(UWORD dest_addr, UBYTE endpoint, UBYTE seq, UBYTE cmd, UBYTE *out_frame);
UWORD zcl_build_level_cmd(UWORD dest_addr, UBYTE endpoint, UBYTE seq, UBYTE level, UWORD transition_100ms, UBYTE *out_frame);

#endif /* RISCV32_EZSP_ASH_H */
