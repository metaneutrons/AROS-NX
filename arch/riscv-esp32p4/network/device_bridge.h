/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_DEVICE_BRIDGE_H
#define P4_WIFI_DEVICE_BRIDGE_H

#include <exec/types.h>
#include "hosted_scan.h"

#define P4_WIFI_DEVICE_BRIDGE_ABI 6u
#define P4_WIFI_DEVICE_BRIDGE_NAME "esp32p4wifi.resource"

struct P4WifiBase;
/* Correlated current-AP observations only. UID0 hints make this UNKNOWN,
 * never DOWN. DOWN requires the peer's exact NOT_CONNECT response. */
#define P4_WIFI_LINK_UNKNOWN 0u
#define P4_WIFI_LINK_UP 1u
#define P4_WIFI_LINK_DOWN 2u
/* Credential-free confirmed AP snapshot. Only the serialized service caller
 * may copy it after associate returns TRUE; hints and requested targets are
 * not substitutes for the peer's correlated current-AP result. */
struct p4_wifi_link_info {
    UBYTE ssid[33];
    ULONG ssid_length;
    UBYTE bssid[6];
    ULONG channel;
    ULONG authmode;
};
/* Owned input; no caller tag/string pointers survive BeginIO. This first
 * profile is open or WPA2-PSK/CCMP infrastructure, never WEP/enterprise/SAE.
 * password is printable passphrase8..63 or exactly64 hexadecimal PSK bytes.
 * The caller must volatile-wipe its copy after the bounded operation. */
struct p4_wifi_association_target {
    UBYTE ssid[32];
    ULONG ssid_length;
    UBYTE password[64];
    ULONG password_length;
    UBYTE bssid[6];
    BOOL bssid_set;
    ULONG channel; /* zero permits automatic selection */
    ULONG authmode; /* zero=open, three=WPA2-PSK */
};
/* Called outside controller ownership. It must be nonblocking, may take the
 * device unit lock, and must not re-enter transport or close/detach. */
struct p4_wifi_association_cancel {
    APTR context;
    BOOL (*cancelled)(APTR);
};
/* Owned default-config scan result. At most eight APs are retained locally;
 * total_count is the peer's earlier count, not an atomic snapshot guarantee.
 * Security fields are summaries, not captured Beacon/RSN/WPA IEs. */
struct p4_wifi_scan_result {
    struct p4_hosted_scan_records aps;
    ULONG total_count;
    BOOL truncated;
    BOOL empty_records_workaround;
};
/* Private port contract, not a public SANA-II ABI. The client and callback
 * table remain live until detach returns. service/frame/stopped run in owner task context,
 * outside the SDMMC semaphore, serialized by the client's registration lock.
 * Never attach/detach from a callback. Device close must not hold its own
 * semaphore while calling detach. No callback may retain frame pointers.
 * service may call send_frame, scan, associate or disconnect; frame/ready/stopped may not
 * access transport. scan/associate suppress transport re-entry throughout.
 * No callback may wait for another client callback or invoke user code which
 * closes the device. Permanent noexpunge modules protect callback code. The
 * device registers its permanent unit once and keeps it across last Close;
 * detach is not a CloseDevice hook and may wait outside Forbid. ready can also
 * run synchronously in attach's caller: metadata only, nonblocking. */
struct p4_wifi_device_callbacks {
    void (*service)(APTR client, struct P4WifiBase *radio);
    /* TRUE means consumed or deliberately dropped (no reader/filter/full).
     * FALSE is a fatal client invariant failure and retires the session;
     * normal packet loss must update device statistics and return TRUE. */
    BOOL (*frame)(APTR client, const UBYTE *frame, ULONG length);
    void (*ready)(APTR client, const UBYTE *mac);
    void (*stopped)(APTR client);
};

/* Credential enablement requires separate, default-off C6 and host privacy/
 * recovery release assertions. A source build proves neither hardware gate.
 * Attachment does not start/reset a radio or establish association. It is
 * refused until the opt-in owner has initialized and published its MAC.
 * send_frame is owner-task-only and returns after bounded transmission, not
 * after IP delivery. Request ownership and cancellation stay in the device.
 * scan is also owner-service-only, bounded by one aggregate eight-second
 * deadline. It runs without a client unit lock, returns owned results only on
 * success, and leaves output unchanged on failure. The output must not alias
 * resource/transport state. No scan filters or credentials are supplied.
 * associate is owner-service-only and uses one aggregate eight-second budget.
 * It disconnects/verifies absence before configuration, then confirms the
 * current AP against the target and revalidates the live host epoch. TRUE is a
 * current-target snapshot, not causal attribution of UID0 events or IP proof.
 * associate never opens the traffic gate. ABI5 publishes confirmed_ap on TRUE
 * under client_lock; failure after entry and link hints clear that snapshot.
 * On TRUE, its service caller must
 * revalidate its owned attempt/opener/online token under the device unit lock
 * before setting associated and publishing CONNECT together. The caller owns
 * client_lock throughout that serialized service callback. Events can dispatch
 * recursively inside association RPCs, but no transport call may occur between
 * the returned snapshot's lifecycle fence and publication. Owner event dispatch
 * then cannot interleave that commit. Device RX/TX must also check its committed
 * link/token under unit lock so concurrent Close/Offline/supersession can
 * invalidate traffic immediately, without taking client_lock in reverse order.
 * After an entered operation, failed/cancelled results leave associated false;
 * invalid calls refused before entry do not mutate an existing link. A native
 * new selection must invalidate its own committed link before starting work.
 * A rejected queue completion must not set associated. ABI4 changed this success
 * semantic: an older client expecting associate to open traffic must be refused.
 * association_enabled is false until both C6 and host privacy/recovery release
 * assertions are granted; builds cannot verify those hardware gates. Even an
 * open target must not bypass either gate. Failure/cancellation
 * never permits traffic. Neither ready nor S2_ONLINE establishes link-up. */

/* ABI6 disconnect is owner-service-only, under client_lock and without a
 * device unit lock. It uses one aggregate eight-second deadline, verifies
 * NOT_CONNECT under a stable event revision and live host epoch, and leaves
 * traffic closed. Failure is not proof of radio absence. UNKNOWN/UP/DOWN and
 * observed_ap are owner-published under client_lock; the native device must
 * compare UP against its still-current accepted identity before reopening
 * traffic. Observations never create an unsolicited new association. */

#endif
