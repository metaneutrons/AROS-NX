/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <aros/asmcall.h>
#include <aros/io.h>
#include <aros/macros.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <stddef.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <devices/newstyle.h>

#include "device_bridge.h"
#include "sana_device.h"
#include "scan_projection.h"

#include LC_LIBDEFS_FILE

struct ExecBase *SysBase;

#define P4WLAN_TAG_LIMIT 128u

static const UWORD p4wlan_supported_commands[] = {
    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_BROADCAST,
    S2_MULTICAST,
    S2_GETGLOBALSTATS,
    S2_ONEVENT,
    S2_ONLINE,
    S2_OFFLINE,
    S2_GETNETWORKS,
    S2_GETWIRELESSCAPABILITIES,
    CMD_READ,
    CMD_WRITE,
    CMD_FLUSH,
    NSCMD_DEVICEQUERY,
    0
};

static BOOL p4wlan_mac_valid(const UBYTE *mac)
{
    ULONG i;
    UBYTE nonzero = 0;
    for (i = 0; i < 6; ++i)
        nonzero |= mac[i];
    return nonzero != 0 && (mac[0] & 1u) == 0;
}

static BOOL p4wlan_mac_equal(const UBYTE *a, const UBYTE *b)
{
    ULONG i;
    UBYTE different = 0;
    for (i = 0; i < 6; ++i)
        different |= (UBYTE)(a[i] ^ b[i]);
    return different == 0;
}

static void p4wlan_copy_bytes(UBYTE *dst, const UBYTE *src, ULONG length)
{
    ULONG i;
    for (i = 0; i < length; ++i)
        dst[i] = src[i];
}

static void p4wlan_zero_bytes(UBYTE *dst, ULONG length)
{
    ULONG i;
    for (i = 0; i < length; ++i)
        dst[i] = 0;
}

static void p4wlan_wipe_bytes(UBYTE *data, ULONG length)
{
    volatile UBYTE *bytes = data;
    while (length--)
        *bytes++ = 0;
}

static BOOL p4wlan_copy_open_tags(struct p4wlan_opener *opener,
                                  const struct TagItem *tags)
{
    ULONG visited = 0;
    BOOL terminated = FALSE;

    if (!tags)
        return FALSE;

    while (tags && visited < P4WLAN_TAG_LIMIT) {
        ULONG tag = tags->ti_Tag;
        IPTR data = tags->ti_Data;
        ++tags;
        ++visited;

        if (tag == TAG_DONE) {
            terminated = TRUE;
            break;
        }
        if (tag == TAG_IGNORE)
            continue;
        if (tag == TAG_MORE) {
            tags = (const struct TagItem *)data;
            continue;
        }
        if (tag == TAG_SKIP) {
            IPTR address = (IPTR)tags;
            IPTR max_address = ~(IPTR)0;
            IPTR skip = (IPTR)data;
            if (skip > (max_address - address) / sizeof(*tags) ||
                skip > P4WLAN_TAG_LIMIT - visited)
                return FALSE;
            tags = (const struct TagItem *)(address + skip * sizeof(*tags));
            visited += (ULONG)skip;
            continue;
        }

        switch (tag) {
        case S2_CopyToBuff:
            opener->copy_to = (p4wlan_copy_fn)data;
            break;
        case S2_CopyToBuff16:
            opener->copy_to16 = (p4wlan_copy_fn)data;
            break;
        case S2_CopyToBuff32:
            opener->copy_to32 = (p4wlan_copy_fn)data;
            break;
        case S2_CopyFromBuff:
            opener->copy_from = (p4wlan_copy_fn)data;
            break;
        case S2_CopyFromBuff16:
            opener->copy_from16 = (p4wlan_copy_fn)data;
            break;
        case S2_CopyFromBuff32:
            opener->copy_from32 = (p4wlan_copy_fn)data;
            break;
        case S2_PacketFilter:
            opener->filter = (struct Hook *)data;
            break;
        default:
            break;
        }
    }

    return terminated &&
           (opener->copy_to || opener->copy_to16 || opener->copy_to32) &&
           (opener->copy_from || opener->copy_from16 || opener->copy_from32);
}

static p4wlan_copy_fn p4wlan_rx_copy(struct p4wlan_opener *opener)
{
    if (opener->copy_to16)
        return opener->copy_to16;
    if (opener->copy_to)
        return opener->copy_to;
    return opener->copy_to32;
}

static p4wlan_copy_fn p4wlan_tx_copy(struct p4wlan_opener *opener)
{
    if (opener->copy_from32)
        return opener->copy_from32;
    if (opener->copy_from16)
        return opener->copy_from16;
    return opener->copy_from;
}

static struct p4wlan_opener *p4wlan_find_opener_locked(
    struct p4wlan_unit *unit, APTR candidate)
{
    struct MinNode *node;
    for (node = unit->openers.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        struct p4wlan_opener *opener =
            (struct p4wlan_opener *)((UBYTE *)node -
                offsetof(struct p4wlan_opener, unit_node));
        if ((APTR)opener == candidate && !opener->closing)
            return opener;
    }
    return NULL;
}

static void p4wlan_add_done_locked(struct MinList *done,
                                   struct p4wlan_request *item,
                                   BYTE error, ULONG wire_error)
{
    item->state = P4WLAN_REQUEST_COMPLETING;
    item->completion_error = error;
    item->completion_wire = wire_error;
    item->completion_sets_length = FALSE;
    ADDTAIL((struct List *)done, (struct Node *)&item->done_node);
}

static struct p4wlan_request *p4wlan_first_done(struct MinList *done)
{
    if (IsMinListEmpty(done))
        return NULL;
    return (struct p4wlan_request *)((UBYTE *)done->mlh_Head -
        offsetof(struct p4wlan_request, done_node));
}

static void p4wlan_finish_request(struct p4wlan_unit *unit,
                                  struct p4wlan_request *item)
{
    struct IOSana2Req *request = item->request;
    struct p4wlan_opener *opener = item->opener;
    BYTE error = item->completion_error;
    ULONG wire_error = item->completion_wire;
    BOOL set_length = item->completion_sets_length;
    ULONG length = item->completion_length;
    struct Task *signal_task = NULL;
    ULONG signal_mask = 0;

    /* Remove the descriptor before ReplyMsg so immediate IO reuse cannot
     * observe stale ownership. Keep the opener ref until ReplyMsg returns so
     * Close cannot free request-owned context while it is still being sent. */
    ObtainSemaphore(&unit->lock);
    REMOVE((struct Node *)&item->owner_node);
    if (opener->request_count)
        --opener->request_count;
    if (unit->request_count)
        --unit->request_count;
    if (item->kind == P4WLAN_REQUEST_READ) {
        if (opener->read_count)
            --opener->read_count;
        if (unit->read_count)
            --unit->read_count;
    } else if (item->kind == P4WLAN_REQUEST_WRITE) {
        if (opener->write_count)
            --opener->write_count;
        if (unit->write_count)
            --unit->write_count;
    } else if (item->kind == P4WLAN_REQUEST_EVENT) {
        if (opener->event_count)
            --opener->event_count;
        if (unit->event_count)
            --unit->event_count;
    } else if (item->kind == P4WLAN_REQUEST_SCAN) {
        if (unit->scan_count)
            --unit->scan_count;
    }
    ReleaseSemaphore(&unit->lock);

    request->ios2_Req.io_Error = error;
    request->ios2_WireError = wire_error;
    if (set_length)
        request->ios2_DataLength = length;
    ReplyMsg(&request->ios2_Req.io_Message);

    ObtainSemaphore(&unit->lock);
    if (opener->refs)
        --opener->refs;
    if (opener->closing && opener->close_waiter && opener->refs == 1) {
        signal_task = opener->close_task;
        signal_mask = opener->close_signal;
    }
    if (signal_task && signal_mask)
        Signal(signal_task, signal_mask);
    ReleaseSemaphore(&unit->lock);

    FreeVec(item);
}

static void p4wlan_finish_done(struct p4wlan_unit *unit, struct MinList *done)
{
    struct p4wlan_request *item;
    while ((item = p4wlan_first_done(done)) != NULL) {
        REMOVE((struct Node *)&item->done_node);
        p4wlan_finish_request(unit, item);
    }
}

static void p4wlan_queue_done_locked(struct p4wlan_unit *unit,
                                     struct p4wlan_opener *opener,
                                     struct p4wlan_request *item,
                                     struct MinList *done,
                                     BYTE error, ULONG wire_error)
{
    (void)unit;
    (void)opener;
    if (item->state == P4WLAN_REQUEST_COMPLETING)
        return;
    if ((item->kind == P4WLAN_REQUEST_WRITE ||
         item->kind == P4WLAN_REQUEST_SCAN) &&
        item->state == P4WLAN_REQUEST_QUEUED)
        REMOVE((struct Node *)&item->write_node);
    p4wlan_add_done_locked(done, item, error, wire_error);
}

static void p4wlan_flush_opener_locked(struct p4wlan_unit *unit,
                                       struct p4wlan_opener *opener,
                                       struct MinList *done,
                                       BYTE error, ULONG wire_error,
                                       BOOL flush_events)
{
    struct MinNode *node = opener->requests.mlh_Head;
    while (node->mln_Succ) {
        struct MinNode *next = node->mln_Succ;
        struct p4wlan_request *item =
            (struct p4wlan_request *)((UBYTE *)node -
                offsetof(struct p4wlan_request, owner_node));
        if (item->state == P4WLAN_REQUEST_ACTIVE) {
            item->cancelled = TRUE;
        } else if (item->state == P4WLAN_REQUEST_QUEUED &&
                   (item->kind != P4WLAN_REQUEST_EVENT || flush_events)) {
            p4wlan_queue_done_locked(unit, opener, item, done, error,
                                     wire_error);
        }
        node = next;
    }
}

static void p4wlan_flush_all_locked(struct p4wlan_unit *unit,
                                    struct MinList *done,
                                    BYTE error, ULONG wire_error,
                                    BOOL flush_events)
{
    struct MinNode *node;
    for (node = unit->openers.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        struct p4wlan_opener *opener =
            (struct p4wlan_opener *)((UBYTE *)node -
                offsetof(struct p4wlan_opener, unit_node));
        p4wlan_flush_opener_locked(unit, opener, done, error, wire_error,
                                   flush_events);
    }
}

static void p4wlan_event_locked(struct p4wlan_unit *unit, ULONG event,
                                struct MinList *done)
{
    struct MinNode *onode;
    for (onode = unit->openers.mlh_Head; onode->mln_Succ;
         onode = onode->mln_Succ) {
        struct p4wlan_opener *opener =
            (struct p4wlan_opener *)((UBYTE *)onode -
                offsetof(struct p4wlan_opener, unit_node));
        struct MinNode *rnode;
        for (rnode = opener->requests.mlh_Head; rnode->mln_Succ;
             rnode = rnode->mln_Succ) {
            struct p4wlan_request *item =
                (struct p4wlan_request *)((UBYTE *)rnode -
                    offsetof(struct p4wlan_request, owner_node));
            if (item->kind == P4WLAN_REQUEST_EVENT &&
                item->state == P4WLAN_REQUEST_QUEUED &&
                (item->event_mask & event)) {
                item->state = P4WLAN_REQUEST_COMPLETING;
                item->completion_error = 0;
                item->completion_wire = item->event_mask & event;
                item->completion_sets_length = FALSE;
                ADDTAIL((struct List *)done,
                        (struct Node *)&item->done_node);
            }
        }
    }
}

static void p4wlan_set_offline_locked(struct p4wlan_unit *unit,
                                      struct MinList *done,
                                      BOOL hardware_stopped)
{
    BOOL changed = unit->online;
    unit->online = FALSE;
    if (hardware_stopped)
        unit->hardware_ready = FALSE;
    if (changed)
        p4wlan_event_locked(unit, S2EVENT_OFFLINE, done);
    p4wlan_flush_all_locked(unit, done, S2ERR_OUTOFSERVICE,
                            S2WERR_UNIT_OFFLINE, TRUE);
}

static struct p4wlan_request *p4wlan_alloc_request(
    struct p4wlan_opener *opener, struct IOSana2Req *request, ULONG kind)
{
    struct p4wlan_request *item =
        AllocVec(sizeof(*item), MEMF_PUBLIC | MEMF_CLEAR);
    if (!item)
        return NULL;
    item->opener = opener;
    item->request = request;
    item->kind = kind;
    item->state = P4WLAN_REQUEST_QUEUED;
    return item;
}

static void p4wlan_enqueue_locked(struct p4wlan_unit *unit,
                                  struct p4wlan_opener *opener,
                                  struct p4wlan_request *item)
{
    ADDTAIL((struct List *)&opener->requests,
            (struct Node *)&item->owner_node);
    ++opener->refs;
    ++opener->request_count;
    ++unit->request_count;
    if (item->kind == P4WLAN_REQUEST_READ) {
        ++opener->read_count;
        ++unit->read_count;
    } else if (item->kind == P4WLAN_REQUEST_WRITE) {
        ++opener->write_count;
        ++unit->write_count;
        ADDTAIL((struct List *)&unit->write_queue,
                (struct Node *)&item->write_node);
    } else if (item->kind == P4WLAN_REQUEST_SCAN) {
        ++unit->scan_count;
        ADDTAIL((struct List *)&unit->scan_queue,
                (struct Node *)&item->write_node);
    } else {
        ++opener->event_count;
        ++unit->event_count;
    }
    item->request->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    item->request->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
}

static void p4wlan_term_io(struct IOSana2Req *request)
{
    if (!(request->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&request->ios2_Req.io_Message);
}

static BOOL p4wlan_resource_valid(struct P4WifiBase *radio)
{
    return radio && radio->bridge_abi == P4_WIFI_DEVICE_BRIDGE_ABI &&
           radio->bridge_size >= sizeof(*radio) && radio->attach &&
           radio->detach && radio->send_frame && radio->scan;
}

static void p4wlan_ready_callback(APTR client, const UBYTE *mac)
{
    struct p4wlan_unit *unit = client;
    if (!unit || !mac || !p4wlan_mac_valid(mac)) {
        if (unit)
            __sync_lock_test_and_set(&unit->callback_fault, TRUE);
        return;
    }

    /* attach invokes ready synchronously; never wait or recurse into resource
     * locking from this metadata-only callback. */
    if (!AttemptSemaphore(&unit->lock)) {
        __sync_lock_test_and_set(&unit->callback_fault, TRUE);
        return;
    }
    if (unit->hardware_ready && !p4wlan_mac_equal(unit->mac, mac)) {
        unit->hardware_ready = FALSE;
        __sync_lock_test_and_set(&unit->callback_fault, TRUE);
    } else {
        p4wlan_copy_bytes(unit->mac, mac, 6);
        unit->hardware_ready = TRUE;
    }
    ReleaseSemaphore(&unit->lock);
}

static void p4wlan_stopped_callback(APTR client)
{
    struct p4wlan_unit *unit = client;
    struct MinList done;
    if (!unit)
        return;
    NEWLIST(&done);
    /* Owner callbacks may wait only for this short metadata lock. No unit
     * holder calls back into the bridge, and Close never takes client_lock. */
    ObtainSemaphore(&unit->lock);
    p4wlan_set_offline_locked(unit, &done, TRUE);
    ReleaseSemaphore(&unit->lock);
    p4wlan_finish_done(unit, &done);
}

static BOOL p4wlan_frame_callback(APTR client, const UBYTE *frame,
                                  ULONG length)
{
    struct p4wlan_unit *unit = client;
    ULONG packet_type;
    ULONG sequence;
    BOOL delivered = FALSE;

    if (!unit || !frame)
        return FALSE;

    if (length < P4WLAN_ETH_HEADER || length > P4WLAN_MAX_FRAME) {
        if (AttemptSemaphore(&unit->lock)) {
            ++unit->stats.BadData;
            __sync_fetch_and_add(&unit->rx_dropped_frames, 1);
            ReleaseSemaphore(&unit->lock);
        } else {
            __sync_fetch_and_add(&unit->rx_dropped_frames, 1);
        }
        return TRUE;
    }

    if (!AttemptSemaphore(&unit->lock)) {
        __sync_fetch_and_add(&unit->rx_dropped_frames, 1);
        return TRUE;
    }
    if (!unit->hardware_ready || !unit->online || unit->callback_fault) {
        __sync_fetch_and_add(&unit->rx_dropped_frames, 1);
        ReleaseSemaphore(&unit->lock);
        return TRUE;
    }

    ++unit->stats.PacketsReceived;
    unit->rx_bytes += length;
    packet_type = ((ULONG)frame[12] << 8) | frame[13];
    sequence = unit->rx_sequence + 1;
    if (sequence == 0) {
        struct MinNode *node;
        sequence = 1;
        for (node = unit->openers.mlh_Head; node->mln_Succ;
             node = node->mln_Succ) {
            struct p4wlan_opener *opener =
                (struct p4wlan_opener *)((UBYTE *)node -
                    offsetof(struct p4wlan_opener, unit_node));
            opener->last_rx_frame = 0;
        }
    }
    unit->rx_sequence = sequence;
    ReleaseSemaphore(&unit->lock);

    for (;;) {
        struct p4wlan_opener *opener = NULL;
        struct p4wlan_request *item = NULL;
        struct MinNode *onode;
        const UBYTE *payload;
        ULONG payload_length;
        BOOL raw;
        BOOL requeue = FALSE;
        BOOL is_broadcast = TRUE;
        ULONG i;

        if (!AttemptSemaphore(&unit->lock))
            return TRUE;
        if (!unit->hardware_ready || !unit->online || unit->callback_fault) {
            ReleaseSemaphore(&unit->lock);
            return TRUE;
        }

        for (onode = unit->openers.mlh_Head; onode->mln_Succ;
             onode = onode->mln_Succ) {
            struct MinNode *rnode;
            struct p4wlan_opener *candidate_opener =
                (struct p4wlan_opener *)((UBYTE *)onode -
                    offsetof(struct p4wlan_opener, unit_node));
            if (candidate_opener->last_rx_frame == sequence)
                continue;
            for (rnode = candidate_opener->requests.mlh_Head;
                 rnode->mln_Succ; rnode = rnode->mln_Succ) {
                struct p4wlan_request *candidate =
                    (struct p4wlan_request *)((UBYTE *)rnode -
                        offsetof(struct p4wlan_request, owner_node));
                if (candidate->kind == P4WLAN_REQUEST_READ &&
                    candidate->state == P4WLAN_REQUEST_QUEUED &&
                    candidate->packet_type == packet_type) {
                    opener = candidate_opener;
                    item = candidate;
                    break;
                }
            }
            if (item)
                break;
            /* An opener gets at most one matching read for this frame. */
            candidate_opener->last_rx_frame = sequence;
        }
        if (!item) {
            if (!delivered) {
                ++unit->rx_unmatched_frames;
                __sync_fetch_and_add(&unit->rx_dropped_frames, 1);
            }
            ReleaseSemaphore(&unit->lock);
            return TRUE;
        }
        opener->last_rx_frame = sequence;
        item->state = P4WLAN_REQUEST_ACTIVE;
        ReleaseSemaphore(&unit->lock);

        raw = (item->io_flags & SANA2IOF_RAW) != 0;
        payload = raw ? frame : frame + P4WLAN_ETH_HEADER;
        payload_length = raw ? length : length - P4WLAN_ETH_HEADER;
        for (i = 0; i < 6; ++i)
            if (frame[i] != 0xff)
                is_broadcast = FALSE;

        item->request->ios2_Req.io_Flags &=
            (UBYTE)~(SANA2IOF_BCAST | SANA2IOF_MCAST);
        if (is_broadcast)
            item->request->ios2_Req.io_Flags |= SANA2IOF_BCAST;
        else if (frame[0] & 1u)
            item->request->ios2_Req.io_Flags |= SANA2IOF_MCAST;
        p4wlan_zero_bytes(item->request->ios2_SrcAddr,
                          SANA2_MAX_ADDR_BYTES);
        p4wlan_zero_bytes(item->request->ios2_DstAddr,
                          SANA2_MAX_ADDR_BYTES);
        p4wlan_copy_bytes(item->request->ios2_SrcAddr, frame + 6, 6);
        p4wlan_copy_bytes(item->request->ios2_DstAddr, frame, 6);
        item->request->ios2_PacketType = packet_type;
        item->request->ios2_DataLength = payload_length;

        if ((!raw && payload_length > P4WLAN_MTU) ||
            payload_length > item->capacity) {
            item->completion_error = S2ERR_MTU_EXCEEDED;
            item->completion_wire = S2WERR_BUFF_ERROR;
            item->completion_sets_length = FALSE;
        } else if (opener->filter &&
                   !CALLHOOKPKT(opener->filter, item->request,
                                (APTR)payload)) {
            requeue = TRUE;
            item->request->ios2_DataLength = item->capacity;
        } else {
            p4wlan_copy_fn copy = p4wlan_rx_copy(opener);
            if (!copy || !copy(item->data, (APTR)payload, payload_length)) {
                item->completion_error = S2ERR_NO_RESOURCES;
                item->completion_wire = S2WERR_BUFF_ERROR;
                item->completion_sets_length = FALSE;
            } else {
                item->completion_error = 0;
                item->completion_wire = 0;
                item->completion_length = payload_length;
                item->completion_sets_length = TRUE;
            }
        }

        ObtainSemaphore(&unit->lock);
        if (item->cancelled || opener->closing || !unit->online ||
            !unit->hardware_ready || unit->callback_fault) {
            item->completion_error = IOERR_ABORTED;
            item->completion_wire = S2WERR_GENERIC_ERROR;
            item->completion_sets_length = FALSE;
            requeue = FALSE;
        }
        if (requeue) {
            item->state = P4WLAN_REQUEST_QUEUED;
            ReleaseSemaphore(&unit->lock);
            continue;
        }
        item->state = P4WLAN_REQUEST_COMPLETING;
        if (item->completion_error == 0)
            delivered = TRUE;
        ReleaseSemaphore(&unit->lock);
        p4wlan_finish_request(unit, item);
    }
    return TRUE;
}

static BOOL p4wlan_send_one(struct p4wlan_unit *unit,
                            struct P4WifiBase *radio)
{
    struct p4wlan_request *item;
    struct p4wlan_opener *opener;
    struct MinList done;
    p4wlan_copy_fn copy;
    ULONG length;
    BOOL raw;
    BOOL copied;
    BOOL sent = FALSE;
    BOOL cancelled = FALSE;
    BYTE error;
    ULONG wire_error;

    if (!AttemptSemaphore(&unit->lock))
        return TRUE;
    NEWLIST(&done);
    if (!unit->online || !unit->hardware_ready || unit->callback_fault) {
        p4wlan_flush_all_locked(unit, &done, S2ERR_OUTOFSERVICE,
                                S2WERR_UNIT_OFFLINE, TRUE);
        ReleaseSemaphore(&unit->lock);
        p4wlan_finish_done(unit, &done);
        return TRUE;
    }
    {
        struct MinNode *node = unit->write_queue.mlh_Head;
        if (!node->mln_Succ) {
            ReleaseSemaphore(&unit->lock);
            return TRUE;
        }
        item = (struct p4wlan_request *)((UBYTE *)node -
            offsetof(struct p4wlan_request, write_node));
        REMOVE((struct Node *)&item->write_node);
    }
    item->state = P4WLAN_REQUEST_ACTIVE;
    opener = item->opener;
    copy = p4wlan_tx_copy(opener);
    raw = (item->io_flags & SANA2IOF_RAW) != 0;
    length = item->capacity;
    ReleaseSemaphore(&unit->lock);

    if (raw) {
        copied = copy && copy(unit->tx_frame, item->data, length);
    } else {
        p4wlan_copy_bytes(unit->tx_frame, item->destination, 6);
        p4wlan_copy_bytes(unit->tx_frame + 6, unit->mac, 6);
        unit->tx_frame[12] = (UBYTE)(item->packet_type >> 8);
        unit->tx_frame[13] = (UBYTE)item->packet_type;
        copied = copy && copy(unit->tx_frame + P4WLAN_ETH_HEADER,
                              item->data, length);
        length += P4WLAN_ETH_HEADER;
    }

    ObtainSemaphore(&unit->lock);
    cancelled = item->cancelled || opener->closing;
    /* Linearize irreversible TX against AbortIO under the same lock. Once
     * committed, AbortIO must refuse rather than promise a suppressed frame.
     * Close/offline still cancel completion and drain the active reference. */
    if (copied && !cancelled)
        item->tx_committed = TRUE;
    ReleaseSemaphore(&unit->lock);

    if (copied && !cancelled) {
        if (radio && radio == unit->radio && radio->send_frame)
            sent = radio->send_frame(radio, unit->tx_frame, length);
        else
            sent = FALSE;
    }
    p4wlan_wipe_bytes(unit->tx_frame, sizeof(unit->tx_frame));

    if (!copied)
        error = S2ERR_NO_RESOURCES;
    else if (!sent)
        error = S2ERR_TX_FAILURE;
    else
        error = 0;
    wire_error = (error == S2ERR_NO_RESOURCES) ? S2WERR_BUFF_ERROR :
                 (error == 0) ? 0 : S2WERR_GENERIC_ERROR;

    ObtainSemaphore(&unit->lock);
    cancelled = item->cancelled || opener->closing;
    if (sent) {
        ++unit->stats.PacketsSent;
        unit->tx_bytes += length;
    } else if (!cancelled) {
        ++unit->tx_dropped_frames;
    }
    item->state = P4WLAN_REQUEST_COMPLETING;
    if (cancelled) {
        error = IOERR_ABORTED;
        wire_error = S2WERR_GENERIC_ERROR;
    }
    item->completion_error = error;
    item->completion_wire = wire_error;
    item->completion_sets_length = FALSE;
    ReleaseSemaphore(&unit->lock);
    p4wlan_finish_request(unit, item);
    return TRUE;
}

struct p4wlan_scan_public_record {
    struct TagItem tags[7];
    UBYTE ssid[33];
    UBYTE bssid[6];
    struct Sana2WirelessSecuritySummary security;
};

struct p4wlan_scan_publication {
    struct TagItem *lists[P4_HOSTED_SCAN_MAX_RECORDS];
    struct p4wlan_scan_public_record records[P4_HOSTED_SCAN_MAX_RECORDS];
};

/* Existing SANA SSID tags are C strings. Refuse an unrepresentable binary
 * SSID rather than publish a truncated identity. Hidden empty SSIDs survive. */
static BOOL p4wlan_scan_record_matches(const struct p4_hosted_scan_record *ap,
                                       const struct p4wlan_request *item)
{
    ULONG i;
    if (ap->ssid_size > 32 || ap->primary < 1 || ap->primary > 14)
        return FALSE;
    for (i = 0; i < ap->ssid_size; ++i) {
        if (!ap->ssid[i] ||
            (item->scan_filter_set && ap->ssid[i] != item->scan_filter[i]))
            return FALSE;
    }
    return !item->scan_filter_set || item->scan_filter[ap->ssid_size] == 0;
}

static BOOL p4wlan_scan_one(struct p4wlan_unit *unit, struct P4WifiBase *radio)
{
    struct p4wlan_request *item;
    struct p4wlan_scan_publication *publication = NULL;
    struct MinNode *node;
    ULONG count = 0, i;
    BOOL success, cancelled;

    if (!AttemptSemaphore(&unit->lock))
        return FALSE;
    node = unit->scan_queue.mlh_Head;
    if (!node->mln_Succ) {
        ReleaseSemaphore(&unit->lock);
        return FALSE;
    }
    item = (struct p4wlan_request *)((UBYTE *)node -
        offsetof(struct p4wlan_request, write_node));
    REMOVE((struct Node *)&item->write_node);
    item->state = P4WLAN_REQUEST_ACTIVE;
    success = unit->hardware_ready && !unit->callback_fault &&
              !item->opener->closing && !item->cancelled;
    ReleaseSemaphore(&unit->lock);

    p4wlan_zero_bytes((UBYTE *)&unit->scan_result, sizeof(unit->scan_result));
    success = success && radio == unit->radio && radio->scan &&
              radio->scan(radio, &unit->scan_result);
    if (success && (unit->scan_result.aps.status != 0 ||
                   unit->scan_result.aps.record_count >
                       P4_HOSTED_SCAN_MAX_RECORDS))
        success = FALSE;
    if (success) {
        publication = AllocPooled(item->data, sizeof(*publication));
        if (publication) {
            p4wlan_zero_bytes((UBYTE *)publication, sizeof(*publication));
            for (i = 0; i < unit->scan_result.aps.record_count; ++i) {
                const struct p4_hosted_scan_record *ap =
                    &unit->scan_result.aps.records[i];
                struct p4wlan_scan_public_record *record;
                if (!p4wlan_scan_record_matches(ap, item))
                    continue;
                record = &publication->records[count];
                p4wlan_copy_bytes(record->ssid, ap->ssid, ap->ssid_size);
                p4wlan_copy_bytes(record->bssid, ap->bssid, 6);
                p4wlan_scan_security(ap, &record->security);
                record->tags[0].ti_Tag = S2INFO_SSID;
                record->tags[0].ti_Data = (IPTR)record->ssid;
                record->tags[1].ti_Tag = S2INFO_BSSID;
                record->tags[1].ti_Data = (IPTR)record->bssid;
                record->tags[2].ti_Tag = S2INFO_Channel;
                record->tags[2].ti_Data = ap->primary;
                record->tags[3].ti_Tag = S2INFO_Signal;
                record->tags[3].ti_Data = (IPTR)(LONG)ap->rssi;
                record->tags[4].ti_Tag = S2INFO_Capabilities;
                record->tags[4].ti_Data =
                    (record->security.Flags & S2WIRELESS_SUMF_PRIVACY) ?
                        0x0010u : 0;
                record->tags[5].ti_Tag = S2INFO_SecuritySummary;
                record->tags[5].ti_Data = (IPTR)&record->security;
                record->tags[6].ti_Tag = TAG_DONE;
                publication->lists[count++] = record->tags;
            }
        }
    }
    p4wlan_wipe_bytes((UBYTE *)&unit->scan_result, sizeof(unit->scan_result));

    ObtainSemaphore(&unit->lock);
    cancelled = item->cancelled || item->opener->closing ||
                !unit->hardware_ready || unit->callback_fault;
    item->state = P4WLAN_REQUEST_COMPLETING;
    item->completion_sets_length = FALSE;
    item->completion_error = cancelled ? IOERR_ABORTED :
        !success ? S2ERR_OUTOFSERVICE :
        !publication ? S2ERR_NO_RESOURCES : 0;
    item->completion_wire = item->completion_error ? S2WERR_GENERIC_ERROR : 0;
    if (item->completion_error == 0) {
        /* Atomic publication with respect to AbortIO/Close; the pool owns
         * the whole result until its caller deletes that pool after Reply. */
        item->request->ios2_StatData = publication->lists;
        item->completion_sets_length = TRUE;
        item->completion_length = count;
        publication = NULL;
    }
    ReleaseSemaphore(&unit->lock);
    if (publication)
        FreePooled(item->data, publication, sizeof(*publication));
    p4wlan_finish_request(unit, item);
    return TRUE;
}

static void p4wlan_service_callback(APTR client, struct P4WifiBase *radio)
{
    struct p4wlan_unit *unit = client;
    if (!unit || !radio)
        return;
    if (!p4wlan_scan_one(unit, radio))
        (void)p4wlan_send_one(unit, radio);
}

static const struct p4_wifi_device_callbacks p4wlan_callbacks = {
    p4wlan_service_callback,
    p4wlan_frame_callback,
    p4wlan_ready_callback,
    p4wlan_stopped_callback
};

static void p4wlan_immediate_error(struct IOSana2Req *request,
                                   BYTE error, ULONG wire_error)
{
    request->ios2_Req.io_Error = error;
    request->ios2_WireError = wire_error;
}

static void p4wlan_device_query(struct p4wlan_unit *unit,
                                struct IOSana2Req *request)
{
    struct Sana2DeviceQuery *query =
        (struct Sana2DeviceQuery *)request->ios2_StatData;
    ULONG available;
    if (!query) {
        p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                               S2WERR_BAD_STATDATA);
        return;
    }
    available = query->SizeAvailable;
    if (available < 2 * sizeof(ULONG)) {
        p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                               S2WERR_BAD_STATDATA);
        return;
    }
    if (available > sizeof(*query))
        available = sizeof(*query);
    CopyMem(&unit->query, query, available);
    query->SizeSupplied = available;
}

static void p4wlan_newstyle_query(struct IOSana2Req *request)
{
    struct IOStdReq *std = (struct IOStdReq *)request;
    struct NSDeviceQueryResult *query =
        (struct NSDeviceQueryResult *)std->io_Data;
    if (!query || std->io_Length < sizeof(*query)) {
        p4wlan_immediate_error(request, IOERR_BADADDRESS,
                               S2WERR_BAD_STATDATA);
        return;
    }
    query->DevQueryFormat = 0;
    query->SizeAvailable = sizeof(*query);
    query->DeviceType = NSDEVTYPE_SANA2;
    query->DeviceSubType = 0;
    query->SupportedCommands = (UWORD *)p4wlan_supported_commands;
    std->io_Actual = sizeof(*query);
}

static BOOL p4wlan_validate_write(struct IOSana2Req *request,
                                 ULONG *length, BOOL *raw)
{
    *length = request->ios2_DataLength;
    *raw = (request->ios2_Req.io_Flags & SANA2IOF_RAW) != 0;
    if (!request->ios2_Data)
        return FALSE;
    if (*raw)
        return *length >= P4WLAN_ETH_HEADER &&
               *length <= P4WLAN_MAX_FRAME;
    return request->ios2_PacketType <= 0xffffu &&
           *length <= P4WLAN_MTU;
}

static BOOL p4wlan_scan_copy_filter(struct p4wlan_request *item,
                                    const struct TagItem *tags)
{
    ULONG visited = 0;
    BOOL ssid_seen = FALSE;
    if (!tags)
        return TRUE;
    while (tags && visited < P4WLAN_TAG_LIMIT) {
        ULONG tag = tags->ti_Tag;
        IPTR data = tags->ti_Data;
        ++tags;
        ++visited;
        if (tag == TAG_DONE)
            return TRUE;
        if (tag == TAG_IGNORE)
            continue;
        if (tag == TAG_MORE) {
            tags = (const struct TagItem *)data;
            if (!tags)
                return TRUE;
            continue;
        }
        if (tag == TAG_SKIP) {
            IPTR address = (IPTR)tags;
            if (data > (~(IPTR)0 - address) / sizeof(*tags) ||
                data > P4WLAN_TAG_LIMIT - visited)
                return FALSE;
            tags = (const struct TagItem *)(address + data * sizeof(*tags));
            visited += (ULONG)data;
            continue;
        }
        if (tag != S2INFO_SSID || ssid_seen)
            return FALSE;
        ssid_seen = TRUE;
        if (data) {
            const UBYTE *ssid = (const UBYTE *)data;
            ULONG length;
            for (length = 0; length <= 32; ++length) {
                item->scan_filter[length] = ssid[length];
                if (!ssid[length])
                    break;
            }
            if (length > 32)
                return FALSE;
            item->scan_filter_set = TRUE;
        }
    }
    return FALSE;
}

static void p4wlan_begin_sana(struct p4wlan_unit *unit,
                              struct IOSana2Req *request)
{
    struct p4wlan_opener *opener;
    struct p4wlan_request *item = NULL;
    struct MinList done;
    ULONG command = request->ios2_Req.io_Command;
    ULONG event_mask = request->ios2_WireError;

    NEWLIST(&done);
    request->ios2_Req.io_Error = 0;
    request->ios2_WireError = 0;

    if (!unit || !unit->initialized) {
        p4wlan_immediate_error(request, IOERR_BADADDRESS,
                               S2WERR_GENERIC_ERROR);
        p4wlan_term_io(request);
        return;
    }
    if (command == S2_DEVICEQUERY) {
        p4wlan_device_query(unit, request);
        p4wlan_term_io(request);
        return;
    }
    if (command == NSCMD_DEVICEQUERY) {
        p4wlan_newstyle_query(request);
        p4wlan_term_io(request);
        return;
    }

    ObtainSemaphore(&unit->lock);
    opener = p4wlan_find_opener_locked(unit,
                                       request->ios2_BufferManagement);
    if (!opener) {
        ReleaseSemaphore(&unit->lock);
        p4wlan_finish_done(unit, &done);
        p4wlan_immediate_error(request, IOERR_BADADDRESS,
                               S2WERR_GENERIC_ERROR);
        p4wlan_term_io(request);
        return;
    }

    switch (command) {
    case S2_GETWIRELESSCAPABILITIES:
    {
        struct Sana2WirelessCapabilities *caps = request->ios2_StatData;
        if (!caps || caps->SizeAvailable < sizeof(*caps) ||
            caps->Version != S2WIRELESS_CAPABILITIES_VERSION) {
            p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                                   S2WERR_BAD_STATDATA);
        } else {
            ULONG available = caps->SizeAvailable;
            p4wlan_zero_bytes((UBYTE *)caps, sizeof(*caps));
            caps->SizeAvailable = available;
            caps->SizeSupplied = sizeof(*caps);
            caps->Version = S2WIRELESS_CAPABILITIES_VERSION;
            caps->Flags = S2WIRELESS_CAPF_SECURITY_SUMMARY;
            /* Never advertise handshake offload before association and the
             * verified credential-safe firmware lineage are implemented. */
        }
        break;
    }

    case S2_GETNETWORKS:
        if (!unit->hardware_ready || unit->callback_fault) {
            p4wlan_immediate_error(request, S2ERR_OUTOFSERVICE,
                                   S2WERR_UNIT_OFFLINE);
            break;
        }
        if (!request->ios2_Data) {
            p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                                   S2WERR_NULL_POINTER);
            break;
        }
        if (unit->scan_count ||
            unit->request_count >= P4WLAN_MAX_REQUESTS ||
            opener->request_count >= P4WLAN_MAX_REQUESTS) {
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        item = p4wlan_alloc_request(opener, request, P4WLAN_REQUEST_SCAN);
        if (!item) {
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        if (!p4wlan_scan_copy_filter(item, request->ios2_StatData)) {
            p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        item->data = request->ios2_Data; /* caller pool lives through Reply */
        p4wlan_enqueue_locked(unit, opener, item);
        ReleaseSemaphore(&unit->lock);
        p4wlan_finish_done(unit, &done);
        return;

    case S2_GETSTATIONADDRESS:
        if (!unit->hardware_ready || unit->callback_fault) {
            p4wlan_immediate_error(request, S2ERR_OUTOFSERVICE,
                                   S2WERR_UNIT_OFFLINE);
        } else {
            p4wlan_zero_bytes(request->ios2_SrcAddr,
                              SANA2_MAX_ADDR_BYTES);
            p4wlan_zero_bytes(request->ios2_DstAddr,
                              SANA2_MAX_ADDR_BYTES);
            p4wlan_copy_bytes(request->ios2_SrcAddr, unit->mac, 6);
            p4wlan_copy_bytes(request->ios2_DstAddr, unit->mac, 6);
        }
        break;

    case S2_CONFIGINTERFACE:
        if (!unit->hardware_ready || unit->callback_fault) {
            p4wlan_immediate_error(request, S2ERR_OUTOFSERVICE,
                                   S2WERR_UNIT_OFFLINE);
        } else if (!p4wlan_mac_equal(request->ios2_SrcAddr, unit->mac)) {
            p4wlan_immediate_error(request, S2ERR_BAD_ADDRESS,
                                   S2WERR_SRC_ADDRESS);
        } else if (unit->online) {
            p4wlan_immediate_error(request, S2ERR_BAD_STATE,
                                   S2WERR_UNIT_ONLINE);
        } else {
            unit->configured = TRUE;
        }
        break;

    case S2_ONLINE:
        if (!unit->hardware_ready || unit->callback_fault) {
            p4wlan_immediate_error(request, S2ERR_OUTOFSERVICE,
                                   S2WERR_UNIT_OFFLINE);
        } else if (!unit->configured) {
            p4wlan_immediate_error(request, S2ERR_BAD_STATE,
                                   S2WERR_NOT_CONFIGURED);
        } else if (!unit->online) {
            unit->online = TRUE;
            p4wlan_event_locked(unit, S2EVENT_ONLINE, &done);
        }
        break;

    case S2_OFFLINE:
        p4wlan_set_offline_locked(unit, &done, FALSE);
        break;

    case S2_ONEVENT:
        if (!event_mask || (event_mask &
            ~(S2EVENT_ONLINE | S2EVENT_OFFLINE))) {
            p4wlan_immediate_error(request, S2ERR_NOT_SUPPORTED,
                                   S2WERR_BAD_EVENT);
            break;
        }
        if (event_mask &
            (unit->online ? S2EVENT_ONLINE : S2EVENT_OFFLINE)) {
            request->ios2_WireError = event_mask &
                (unit->online ? S2EVENT_ONLINE : S2EVENT_OFFLINE);
            break;
        }
        item = p4wlan_alloc_request(opener, request,
                                    P4WLAN_REQUEST_EVENT);
        if (!item) {
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        if (opener->request_count >= P4WLAN_MAX_REQUESTS ||
            opener->event_count >= P4WLAN_MAX_EVENTS ||
            unit->event_count >= P4WLAN_MAX_EVENTS) {
            FreeVec(item);
            item = NULL;
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        item->event_mask = event_mask;
        p4wlan_enqueue_locked(unit, opener, item);
        item = NULL;
        break;

    case CMD_READ:
        if (!unit->online || !unit->hardware_ready || unit->callback_fault) {
            p4wlan_immediate_error(request, S2ERR_OUTOFSERVICE,
                                   S2WERR_UNIT_OFFLINE);
            break;
        }
        if (!request->ios2_Data ||
            request->ios2_PacketType > 0xffffu ||
            (request->ios2_Req.io_Flags & SANA2IOF_RAW &&
             request->ios2_DataLength > P4WLAN_MAX_FRAME) ||
            (!(request->ios2_Req.io_Flags & SANA2IOF_RAW) &&
             request->ios2_DataLength > P4WLAN_MTU)) {
            p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                                   S2WERR_NULL_POINTER);
            break;
        }
        item = p4wlan_alloc_request(opener, request,
                                    P4WLAN_REQUEST_READ);
        if (!item) {
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        if (opener->request_count >= P4WLAN_MAX_REQUESTS ||
            opener->read_count >= P4WLAN_MAX_READS ||
            unit->read_count >= P4WLAN_MAX_READS) {
            FreeVec(item);
            item = NULL;
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        item->capacity = request->ios2_DataLength;
        item->packet_type = request->ios2_PacketType;
        item->io_flags = request->ios2_Req.io_Flags;
        item->data = request->ios2_Data;
        p4wlan_enqueue_locked(unit, opener, item);
        item = NULL;
        ReleaseSemaphore(&unit->lock);
        p4wlan_finish_done(unit, &done);
        return;

    case CMD_WRITE:
    case S2_BROADCAST:
    case S2_MULTICAST:
    {
        ULONG length = 0;
        BOOL raw = FALSE;
        UBYTE destination[6];
        ULONG i;
        if (!unit->online || !unit->hardware_ready || unit->callback_fault) {
            p4wlan_immediate_error(request, S2ERR_OUTOFSERVICE,
                                   S2WERR_UNIT_OFFLINE);
            break;
        }
        if (!p4wlan_validate_write(request, &length, &raw)) {
            p4wlan_immediate_error(request,
                request->ios2_Data ? S2ERR_MTU_EXCEEDED : S2ERR_BAD_ARGUMENT,
                request->ios2_Data ? S2WERR_GENERIC_ERROR : S2WERR_NULL_POINTER);
            break;
        }
        if (command == S2_BROADCAST) {
            for (i = 0; i < 6; ++i)
                destination[i] = 0xff;
        } else {
            p4wlan_copy_bytes(destination, request->ios2_DstAddr, 6);
        }
        if (!raw && command == S2_MULTICAST &&
            (!(destination[0] & 1u) ||
             (destination[0] == 0xff && destination[1] == 0xff &&
              destination[2] == 0xff && destination[3] == 0xff &&
              destination[4] == 0xff && destination[5] == 0xff))) {
            p4wlan_immediate_error(request, S2ERR_BAD_ADDRESS,
                                   S2WERR_BAD_MULTICAST);
            break;
        }
        item = p4wlan_alloc_request(opener, request,
                                    P4WLAN_REQUEST_WRITE);
        if (!item) {
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        if (opener->request_count >= P4WLAN_MAX_REQUESTS ||
            opener->write_count >= P4WLAN_MAX_WRITES ||
            unit->write_count >= P4WLAN_MAX_WRITES) {
            FreeVec(item);
            item = NULL;
            p4wlan_immediate_error(request, S2ERR_NO_RESOURCES,
                                   S2WERR_GENERIC_ERROR);
            break;
        }
        item->capacity = length;
        item->packet_type = request->ios2_PacketType;
        item->io_flags = request->ios2_Req.io_Flags;
        item->data = request->ios2_Data;
        p4wlan_copy_bytes(item->destination, destination, 6);
        p4wlan_enqueue_locked(unit, opener, item);
        item = NULL;
        ReleaseSemaphore(&unit->lock);
        p4wlan_finish_done(unit, &done);
        return;
    }

    case CMD_FLUSH:
        p4wlan_flush_opener_locked(unit, opener, &done, IOERR_ABORTED,
                                   S2WERR_GENERIC_ERROR, TRUE);
        break;

    case S2_GETGLOBALSTATS:
        if (!request->ios2_StatData) {
            p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                                   S2WERR_BAD_STATDATA);
        } else {
            CopyMem(&unit->stats, request->ios2_StatData,
                    sizeof(unit->stats));
        }
        break;

    default:
        p4wlan_immediate_error(request, S2ERR_NOT_SUPPORTED,
                               S2WERR_GENERIC_ERROR);
        break;
    }

    ReleaseSemaphore(&unit->lock);
    p4wlan_finish_done(unit, &done);
    if (item)
        FreeVec(item);
    p4wlan_term_io(request);
}

static BOOL GM_UNIQUENAME(Init)(LIBBASETYPEPTR LIBBASE)
{
    struct p4wlan_unit *unit = &LIBBASE->unit;
    SysBase = LIBBASE->sysbase;
    InitSemaphore(&unit->lock);
    NEWLIST(&unit->openers);
    NEWLIST(&unit->write_queue);
    NEWLIST(&unit->scan_queue);
    unit->query.SizeAvailable = sizeof(unit->query);
    unit->query.SizeSupplied = sizeof(unit->query);
    unit->query.DevQueryFormat = 0;
    unit->query.DeviceLevel = 0;
    unit->query.AddrFieldSize = 48;
    unit->query.MTU = P4WLAN_MTU;
    unit->query.BPS = 0;
    unit->query.HardwareType = S2WireType_Ethernet;
    unit->initialized = TRUE;
    return TRUE;
}

static BOOL GM_UNIQUENAME(Open)(LIBBASETYPEPTR LIBBASE,
                                struct IOSana2Req *request,
                                ULONG unit_number, ULONG flags)
{
    struct p4wlan_unit *unit = &LIBBASE->unit;
    struct p4wlan_opener *opener = NULL;
    struct P4WifiBase *radio = NULL;
    BOOL first_attach = FALSE;
    BOOL attached_ok = TRUE;
    APTR tag_list = NULL;

    if (!request || request->ios2_Req.io_Message.mn_Length <
                    sizeof(struct IOSana2Req)) {
        if (request)
            request->ios2_Req.io_Error = IOERR_OPENFAIL;
        return FALSE;
    }
    tag_list = request->ios2_BufferManagement;
    request->ios2_Req.io_Unit = NULL;
    request->ios2_BufferManagement = NULL;

    if (unit_number != 0 || (flags & SANA2OPF_PROM)) {
        request->ios2_Req.io_Error = IOERR_OPENFAIL;
        return FALSE;
    }
    opener = AllocVec(sizeof(*opener), MEMF_PUBLIC | MEMF_CLEAR);
    if (!opener) {
        request->ios2_Req.io_Error = IOERR_OPENFAIL;
        return FALSE;
    }
    if (!p4wlan_copy_open_tags(opener, (const struct TagItem *)tag_list)) {
        FreeVec(opener);
        request->ios2_Req.io_Error = IOERR_OPENFAIL;
        return FALSE;
    }
    NEWLIST(&opener->requests);
    opener->refs = 1;
    opener->exclusive = (flags & SANA2OPF_MINE) != 0;

    /* OpenDevice runs under Forbid.  AttemptSemaphore is deliberate: Open
     * never waits for another task or begins radio/transport work under it. */
    if (!AttemptSemaphore(&unit->lock)) {
        FreeVec(opener);
        request->ios2_Req.io_Error = IOERR_UNITBUSY;
        return FALSE;
    }
    if (!unit->initialized || unit->attaching ||
        unit->open_count >= P4WLAN_MAX_OPENERS ||
        (unit->exclusive_open || (opener->exclusive && unit->open_count))) {
        ReleaseSemaphore(&unit->lock);
        FreeVec(opener);
        request->ios2_Req.io_Error = IOERR_UNITBUSY;
        return FALSE;
    }
    if (!unit->attached) {
        unit->attaching = TRUE;
        unit->attached = TRUE; /* excludes concurrent first-open attempts */
        first_attach = TRUE;
    }
    radio = unit->radio;
    ReleaseSemaphore(&unit->lock);

    if (first_attach) {
        radio = (struct P4WifiBase *)OpenResource(
            (CONST_STRPTR)P4_WIFI_DEVICE_BRIDGE_NAME);
        if (!p4wlan_resource_valid(radio)) {
            attached_ok = FALSE;
        } else {
            unit->radio = radio;
            attached_ok = radio->attach(radio, unit, &p4wlan_callbacks);
        }
        if (!attached_ok) {
            /* A failed attach has no callback client; revert the provisional
             * state.  No client lock/detach or hardware action occurs here. */
            if (AttemptSemaphore(&unit->lock)) {
                unit->attached = FALSE;
                unit->attaching = FALSE;
                unit->radio = NULL;
                unit->hardware_ready = FALSE;
                p4wlan_zero_bytes(unit->mac, sizeof(unit->mac));
                ReleaseSemaphore(&unit->lock);
            } else {
                __sync_lock_test_and_set(&unit->attached, FALSE);
                __sync_lock_test_and_set(&unit->attaching, FALSE);
                unit->radio = NULL;
            }
            FreeVec(opener);
            request->ios2_Req.io_Error = IOERR_OPENFAIL;
            return FALSE;
        }
        __sync_lock_test_and_set(&unit->attaching, FALSE);
    }

    if (!AttemptSemaphore(&unit->lock)) {
        FreeVec(opener);
        request->ios2_Req.io_Error = IOERR_UNITBUSY;
        return FALSE;
    }
    if (!unit->attached || unit->radio != radio || !unit->hardware_ready ||
        unit->callback_fault || !p4wlan_mac_valid(unit->mac) ||
        unit->open_count >= P4WLAN_MAX_OPENERS || unit->attaching ||
        (unit->exclusive_open || (opener->exclusive && unit->open_count))) {
        ReleaseSemaphore(&unit->lock);
        FreeVec(opener);
        request->ios2_Req.io_Error = IOERR_OPENFAIL;
        return FALSE;
    }
    ADDTAIL((struct List *)&unit->openers, (struct Node *)&opener->unit_node);
    ++unit->open_count;
    if (opener->exclusive)
        unit->exclusive_open = TRUE;
    request->ios2_Req.io_Unit = (struct Unit *)unit;
    request->ios2_BufferManagement = opener;
    request->ios2_Req.io_Error = 0;
    ReleaseSemaphore(&unit->lock);
    return TRUE;
}

static BOOL GM_UNIQUENAME(Close)(LIBBASETYPEPTR LIBBASE,
                                 struct IORequest *iorequest)
{
    struct p4wlan_unit *unit = &LIBBASE->unit;
    struct IOSana2Req *request = (struct IOSana2Req *)iorequest;
    struct p4wlan_opener *opener;
    struct MinList done;
    BYTE signal_bit = -1;
    ULONG signal_mask = 0;
    struct Task *task = FindTask(NULL);

    if (!request || request->ios2_Req.io_Message.mn_Length <
                    sizeof(struct IOSana2Req))
        return TRUE;
    opener = (struct p4wlan_opener *)request->ios2_BufferManagement;
    if (!opener)
        return TRUE;
    NEWLIST(&done);
    signal_bit = (BYTE)AllocSignal(-1);
    if (signal_bit >= 0)
        signal_mask = 1UL << signal_bit;
    else
        signal_mask = SIGF_SINGLE;

    ObtainSemaphore(&unit->lock);
    opener = p4wlan_find_opener_locked(unit, opener);
    if (!opener) {
        ReleaseSemaphore(&unit->lock);
        if (signal_bit >= 0)
            FreeSignal((ULONG)signal_bit);
        request->ios2_Req.io_Unit = NULL;
        request->ios2_BufferManagement = NULL;
        return TRUE;
    }
    opener->closing = TRUE;
    REMOVE((struct Node *)&opener->unit_node);
    if (unit->open_count)
        --unit->open_count;
    if (opener->exclusive)
        unit->exclusive_open = FALSE;
    p4wlan_flush_opener_locked(unit, opener, &done, IOERR_ABORTED,
                               S2WERR_GENERIC_ERROR, TRUE);
    opener->close_task = task;
    opener->close_signal = signal_mask;
    opener->close_waiter = TRUE;
    ReleaseSemaphore(&unit->lock);

    request->ios2_Req.io_Unit = NULL;
    request->ios2_BufferManagement = NULL;
    p4wlan_finish_done(unit, &done);

    for (;;) {
        BOOL complete;
        ObtainSemaphore(&unit->lock);
        complete = opener->refs <= 1;
        if (complete) {
            /* The one remaining reference is the Close-owned opener pin.
             * Clear the signal while the same lock excludes finalizers. */
            SetSignal(0, signal_mask);
            if (opener->refs == 1)
                opener->refs = 0;
            opener->close_waiter = FALSE;
            opener->close_task = NULL;
            opener->close_signal = 0;
            ReleaseSemaphore(&unit->lock);
            break;
        }

        /* Clear-and-test under the unit lock, then wait without it. The last
         * descriptor decrements and signals under this same lock, so it cannot
         * signal between the reference check and the clear. SIGF_SINGLE is
         * the documented Exec fallback when AllocSignal has no free bit. */
        SetSignal(0, signal_mask);
        ReleaseSemaphore(&unit->lock);
        Wait(signal_mask); /* Exec drops Forbid while this Close waits. */
    }

    if (signal_bit >= 0)
        FreeSignal((ULONG)signal_bit);
    FreeVec(opener);
    return TRUE;
}

static void p4wlan_begin(LIBBASETYPEPTR LIBBASE,
                         struct IOSana2Req *request)
{
    if (!request) return;
    request->ios2_Req.io_Error = 0;
    if (request->ios2_Req.io_Message.mn_Length < sizeof(*request)) {
        request->ios2_Req.io_Error = IOERR_BADLENGTH;
        p4wlan_term_io(request);
        return;
    }
    if (request->ios2_Req.io_Device != (struct Device *)LIBBASE ||
        request->ios2_Req.io_Unit != (struct Unit *)&LIBBASE->unit) {
        request->ios2_Req.io_Error = IOERR_BADADDRESS;
        p4wlan_term_io(request);
        return;
    }
    p4wlan_begin_sana(&LIBBASE->unit, request);
}

ADD2INITLIB(GM_UNIQUENAME(Init), 0)
ADD2OPENDEV(GM_UNIQUENAME(Open), 0)
ADD2CLOSEDEV(GM_UNIQUENAME(Close), 0)

AROS_LH1(void, begin_io,
         AROS_LHA(struct IORequest *, iorequest, A1),
         LIBBASETYPEPTR, LIBBASE, 5, esp32p4wlan)
{
    AROS_LIBFUNC_INIT
    p4wlan_begin(LIBBASE, (struct IOSana2Req *)iorequest);
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, abort_io,
         AROS_LHA(struct IORequest *, iorequest, A1),
         LIBBASETYPEPTR, LIBBASE, 6, esp32p4wlan)
{
    AROS_LIBFUNC_INIT
    struct IOSana2Req *request = (struct IOSana2Req *)iorequest;
    struct p4wlan_unit *unit = &LIBBASE->unit;
    struct p4wlan_opener *opener;
    struct p4wlan_request *item = NULL;
    BOOL complete = FALSE;
    struct MinList done;

    if (!request || request->ios2_Req.io_Message.mn_Length <
                    sizeof(struct IOSana2Req))
        return -1;
    if (request->ios2_Req.io_Device != (struct Device *)LIBBASE ||
        request->ios2_Req.io_Unit != (struct Unit *)unit ||
        !unit->initialized)
        return -1;

    NEWLIST(&done);
    ObtainSemaphore(&unit->lock);
    opener = p4wlan_find_opener_locked(unit,
                                       request->ios2_BufferManagement);
    if (opener) {
        struct MinNode *node;
        for (node = opener->requests.mlh_Head; node->mln_Succ;
             node = node->mln_Succ) {
            struct p4wlan_request *candidate =
                (struct p4wlan_request *)((UBYTE *)node -
                    offsetof(struct p4wlan_request, owner_node));
            if (candidate->request == request &&
                candidate->state != P4WLAN_REQUEST_COMPLETING) {
                item = candidate;
                break;
            }
        }
    }
    if (item) {
        if (item->kind == P4WLAN_REQUEST_WRITE && item->tx_committed) {
            ReleaseSemaphore(&unit->lock);
            return -1;
        }
        if (item->state == P4WLAN_REQUEST_QUEUED) {
            item->cancelled = TRUE;
            if (item->kind == P4WLAN_REQUEST_WRITE ||
                item->kind == P4WLAN_REQUEST_SCAN)
                REMOVE((struct Node *)&item->write_node);
            p4wlan_add_done_locked(&done, item,
                                   IOERR_ABORTED,
                                   S2WERR_GENERIC_ERROR);
            complete = TRUE;
        } else {
            item->cancelled = TRUE;
        }
    }
    ReleaseSemaphore(&unit->lock);

    if (complete) {
        /* The descriptor is already marked COMPLETING and cannot be claimed
         * by frame/service/Close.  Finish it through the regular ownership
         * release path. */
        p4wlan_finish_done(unit, &done);
        return 0;
    }
    return item ? 0 : -1;
    AROS_LIBFUNC_EXIT
}
