/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Enterprise-Grade SANA-II Protocol Dispatcher & Buffer Manager
          for ESP32-C6 (Wi-Fi 6, 6LoWPAN / Thread, Zigbee 3.0).
*/

#include <exec/types.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>
#include <proto/exec.h>

#include "esp32c6.h"
#include "../../../arch/riscv32-esp32p4/include/sixlowpan.h"
#include "../../../arch/riscv32-esp32p4/include/spinel.h"
#include "../../../arch/riscv32-esp32p4/include/ezsp_ash.h"
#include "../../../arch/riscv32-esp32p4/include/radio_802154.h"

/* Low-level TX callback for 6LoWPAN fragment transmission over Spinel */
static int sana2_spinel_raw_tx(const UBYTE *frag_data, UWORD frag_len, void *user_data)
{
    return p4radio_send_frame(frag_data, frag_len);
}

void process_sana2_cmd(struct esp32c6_unit *u, struct IOSana2Req *ios2)
{
    UWORD cmd = ios2->ios2_Req.io_Command;
    BOOL complete_now = TRUE;

    switch (cmd) {
    case S2_GETSTATIONADDRESS:
        for (int i = 0; i < 6; i++) {
            ios2->ios2_SrcAddr[i] = u->mac_addr[i];
            ios2->ios2_DstAddr[i] = u->mac_addr[i];
        }
        break;

    case S2_CONFIGINTERFACE:
        for (int i = 0; i < 6; i++) {
            u->mac_addr[i] = ios2->ios2_SrcAddr[i];
        }
        break;

    case S2_ONLINE:
        ObtainSemaphore(&u->lock);
        u->online = TRUE;
        ReleaseSemaphore(&u->lock);
        break;

    case S2_OFFLINE:
        ObtainSemaphore(&u->lock);
        u->online = FALSE;

        /* Abort all queued reads with OUTOFSERVICE error */
        struct Node *node;
        while ((node = RemHead((struct List *)&u->read_queue))) {
            struct IOSana2Req *req = (struct IOSana2Req *)node;
            req->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ReplyMsg(&req->ios2_Req.io_Message);
        }
        ReleaseSemaphore(&u->lock);
        break;

    case CMD_READ:
        ObtainSemaphore(&u->lock);
        if (!u->online) {
            ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        } else {
            AddTail((struct List *)&u->read_queue, &ios2->ios2_Req.io_Message.mn_Node);
            complete_now = FALSE;
        }
        ReleaseSemaphore(&u->lock);
        break;

    case CMD_WRITE:
    case S2_BROADCAST:
        ObtainSemaphore(&u->lock);
        if (!u->online) {
            ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        } else {
            UBYTE *pkt_data = (UBYTE *)ios2->ios2_Data;
            ULONG pkt_len  = ios2->ios2_DataLength;

            if (pkt_data && pkt_len > 0) {
                if (u->unit_num == ESP32C6_UNIT_THREAD) {
                    /* 6LoWPAN / Thread IPv6 Dispatch */
                    UBYTE comp_buf[128];
                    WORD comp_len = sixlowpan_compress_ipv6(pkt_data, (UWORD)pkt_len,
                                                           comp_buf, sizeof(comp_buf),
                                                           u->mac_addr, ios2->ios2_DstAddr);

                    if (comp_len > 0) {
                        /* Fragment if needed and send over 802.15.4 Spinel link */
                        sixlowpan_fragment_and_send(comp_buf, (UWORD)comp_len,
                                                    (UWORD)pkt_len,
                                                    sana2_spinel_raw_tx, NULL);
                    } else {
                        /* Send uncompressed with 6LoWPAN IPv6 Dispatch header (0x41) */
                        UBYTE raw_buf[SIXLOWPAN_MAX_FRAME_SIZE];
                        raw_buf[0] = SIXLOWPAN_DISPATCH_IPV6;
                        sixlowpan_fragment_and_send(pkt_data, (UWORD)pkt_len,
                                                    (UWORD)pkt_len,
                                                    sana2_spinel_raw_tx, NULL);
                    }
                } else if (u->unit_num == ESP32C6_UNIT_ZIGBEE) {
                    /* Zigbee 3.0 EZSP / ASH Dispatch */
                    p4radio_send_frame(pkt_data, (UWORD)pkt_len);
                } else {
                    /* Wi-Fi 6 Frame Dispatch */
                    p4radio_send_frame(pkt_data, (UWORD)pkt_len);
                }

                u->stats.PacketsSent++;
                u->stats.BytesSent += pkt_len;
                ios2->ios2_Req.io_Error = 0;
            } else {
                ios2->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            }
        }
        ReleaseSemaphore(&u->lock);
        break;

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
        ios2->ios2_Req.io_Error = 0;
        break;

    case S2_GETSPECIALSTATS:
        {
            struct Sana2DeviceStats *dev_stats = (struct Sana2DeviceStats *)ios2->ios2_StatData;
            if (dev_stats) {
                dev_stats->PacketsReceived = u->stats.PacketsReceived;
                dev_stats->PacketsSent     = u->stats.PacketsSent;
                dev_stats->BadData         = u->stats.BadData;
                dev_stats->Overruns        = u->stats.Overruns;
                dev_stats->Unused          = 0;
                dev_stats->UnknownTypesReceived = u->stats.UnknownTypesReceived;
                dev_stats->Reconfigures    = 0;
            }
        }
        break;

    case S2_ONEVENT:
        complete_now = FALSE;
        break;

    default:
        ios2->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

    if (complete_now) {
        if (!(ios2->ios2_Req.io_Flags & SANA2IOF_QUICK)) {
            ReplyMsg(&ios2->ios2_Req.io_Message);
        }
    }
}

/*
 * High-priority RX packet handler called when a radio packet arrives from ESP32-C6
 */
void esp32c6_dispatch_rx_packet(struct esp32c6_unit *u, const UBYTE *packet, UWORD len)
{
    if (!u || !u->online || !packet || len == 0) return;

    ObtainSemaphore(&u->lock);

    struct IOSana2Req *req = (struct IOSana2Req *)RemHead((struct List *)&u->read_queue);
    if (req) {
        UBYTE *dst = (UBYTE *)req->ios2_Data;
        ULONG max_len = req->ios2_DataLength;

        if (u->unit_num == ESP32C6_UNIT_THREAD) {
            /* 6LoWPAN Decompression / Reassembly */
            WORD decomp_len = sixlowpan_process_rx_fragment(packet, len,
                                                            u->mac_addr, 0,
                                                            dst, (UWORD)max_len);
            if (decomp_len > 0) {
                req->ios2_Req.io_Actual = decomp_len;
                req->ios2_PacketType = 0x86DD; /* IPv6 */
                req->ios2_Req.io_Error = 0;
                u->stats.PacketsReceived++;
                u->stats.BytesReceived += decomp_len;
                ReplyMsg(&req->ios2_Req.io_Message);
            } else {
                /* Incomplete fragment or error - re-insert read request */
                AddHead((struct List *)&u->read_queue, &req->ios2_Req.io_Message.mn_Node);
            }
        } else {
            /* Wi-Fi / Raw Frame copy */
            UWORD copy_len = (len < max_len) ? len : (UWORD)max_len;
            for (UWORD i = 0; i < copy_len; i++) dst[i] = packet[i];

            req->ios2_Req.io_Actual = copy_len;
            req->ios2_PacketType = (u->unit_num == ESP32C6_UNIT_WIFI) ? 0x0800 : 0xFFFF;
            req->ios2_Req.io_Error = 0;
            u->stats.PacketsReceived++;
            u->stats.BytesReceived += copy_len;
            ReplyMsg(&req->ios2_Req.io_Message);
        }
    }

    ReleaseSemaphore(&u->lock);
}
