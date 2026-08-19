/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: AROS SANA-II Network Device Header for ESP32-C6 (Wi-Fi 6 / 802.15.4 / Zigbee).
*/

#ifndef DEVS_NETWORKS_ESP32C6_H
#define DEVS_NETWORKS_ESP32C6_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>

#define ESP32C6_DEV_NAME       "esp32c6.device"
#define ESP32C6_DEV_VERSION    0
#define ESP32C6_DEV_REVISION   1

/* Unit IDs */
#define ESP32C6_UNIT_WIFI      0   /* Standard Wi-Fi 6 (802.11ax) Ethernet */
#define ESP32C6_UNIT_THREAD    1   /* IEEE 802.15.4 / 6LoWPAN / Matter */
#define ESP32C6_UNIT_ZIGBEE    2   /* Zigbee 3.0 (EZSP Clusters) */
#define ESP32C6_MAX_UNITS      3

struct esp32c6_unit
{
    struct Unit         unit;
    ULONG               unit_num;
    BOOL                online;
    UBYTE               mac_addr[8];
    ULONG               mtu;
    ULONG               wire_type;
    struct MsgPort     *rx_port;
    struct MsgPort     *tx_port;
    struct MinList      read_queue;
    struct MinList      write_queue;
    struct SignalSemaphore lock;
    struct Sana2DeviceStats stats;
};

struct esp32c6_base
{
    struct Library      lib;
    UWORD               pad;
    BPTR                seg_list;
    struct ExecBase    *sys_base;
    struct esp32c6_unit units[ESP32C6_MAX_UNITS];
};

/* Internal Functions */
LIBFUNC_P2_DECL(struct esp32c6_base *, init_device, D0, struct esp32c6_base *, a6, struct ExecBase *);
LIBFUNC_P3_DECL(struct esp32c6_base *, open_device, D0, ULONG, A1, struct IOSana2Req *, a6, struct esp32c6_base *);
LIBFUNC_P2_DECL(BPTR, close_device, A1, struct IOSana2Req *, a6, struct esp32c6_base *);
LIBFUNC_P2_DECL(BPTR, expunge_device, D0, struct esp32c6_base *, a6, struct ExecBase *);

void begin_io(struct esp32c6_base *base, struct IOSana2Req *ios2);
void abort_io(struct esp32c6_base *base, struct IOSana2Req *ios2);
void process_sana2_cmd(struct esp32c6_unit *u, struct IOSana2Req *ios2);

#endif /* DEVS_NETWORKS_ESP32C6_H */
