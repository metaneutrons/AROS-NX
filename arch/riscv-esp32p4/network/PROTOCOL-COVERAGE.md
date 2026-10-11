# Hosted C6 station protocol coverage

Source audit, 2026-10-11; not an installed-peer or hardware qualification.
AROS baseline: `6024ecd214269ed43e820964c441a44f80c0d6a5`.
Reference Hosted revision: `d5184217f710d54c01f932e75b4c55e3f4933fc9`,
`common/proto/esp_hosted_rpc.proto` SHA256
`bbdbc9bb69c2b1793de6441ea55051c3ceb3f7b30994227b8ff1e995b0137dec`.
Generic WiFi IDs below are pinned to IDF5.4.3, not inferred from the installed
`54aafd0-dirty` C6 firmware. Its exact lineage remains unqualified.

## Meaning of coverage

Envelope acceptance is not payload validation or state-machine support.
`hosted_rpc.c` accepts numeric request/response/event ranges, including schema
holes. `runtime_probe.c:wifi_owner_request` admits only explicit station
encoders; `hosted_wifi.c` refuses other simple requests. Pending responses
require the selected UID/response pair. Never advertise an API merely because
the generic envelope can carry it.

The reference has 82 active request IDs, their 82 paired response IDs and eight
RPC events. Each response ID is request ID +256. Commented-out261–269 and
295–296 are holes;256/350,512/606 and768/777 are sentinels. The separately gated
board-discovery extension reuses350/606 and must not be assumed from this schema.

## Requests and paired responses

Every active reference ID is assigned below. A group range includes every ID
in that range. “Not station API” means no runtime station request admission;
it does not imply that a received event may safely be ignored.

| Request ID | Reference operation(s) | AROS station coverage / policy |
| --- | --- | --- |
|257|GetMACAddress|Typed request/reply and MAC validation|
|258–259|SetMacAddress, GetWifiMode|Not station API|
|260|SetWifiMode|STA-only encoder and status reply|
|270–271|WifiSetPs, WifiGetPs|Not station API; power policy must be qualified before enabling|
|272–274|OTABegin, OTAWrite, OTAEnd|Separate gated update path, not WLAN admission; C6 update postponed|
|275–276|WifiSetMaxTxPower, WifiGetMaxTxPower|Not station API|
|277|ConfigHeartbeat|Not station API; optional watchdog protocol not configured|
|278|WifiInit|Explicit config encoder and status reply|
|279–281|WifiDeinit, WifiStart, WifiStop|Explicit simple whitelist and status reply|
|282–284|WifiConnect, WifiDisconnect, WifiSetConfig|Typed requests/status replies; ACK is not authenticated link completion|
|285|WifiGetConfig|Not station API; do not query credentials for diagnostics|
|286–289|WifiScanStart, WifiScanStop, WifiScanGetApNum, WifiScanGetApRecords|Bounded typed request/reply; blocking scan currently used|
|290–293|WifiClearApList, WifiRestore, WifiClearFastConnect, WifiDeauthSta|Not station API; review scan memory/cleanup and reconnect needs explicitly|
|294|WifiStaGetApInfo|Typed current AP query; not a negotiated-security or attempt barrier by itself|
|297–304|Set/GetProtocol, Set/GetBandwidth, Set/GetChannel, Set/GetCountry|Not station API; regulatory/default policy requires qualification|
|305–310|Set/GetPromiscuous, Set/GetPromiscuousFilter, Set/GetPromiscuousCtrlFilter|Out of station scope|
|311–312|WifiApGetStaList, WifiApGetStaAid|SoftAP, out of scope|
|313–317|SetStorage, SetVendorIe, Set/GetEventMask, Wifi80211Tx|Not station API; do not mask required station events|
|318–323|SetCsiConfig, SetCsi, Set/GetAntGpio, Set/GetAnt|CSI/antenna control, out of present scope|
|324–328|GetTsfTime, Set/GetInactiveTime, StatisDump, SetRssiThreshold|Not station API; RSSI event remains classified below|
|329–331|FtmInitiateSession, FtmEndSession, FtmRespSetOffset|FTM, out of scope|
|332–337|Config11bRate, ConnectionlessModuleSetWakeInterval, Set/GetCountryCode, Config80211TxRate, DisablePmfConfig|Not station API; never disable required PMF for SAE|
|338–341|StaGetAid, StaGetNegotiatedPhymode, SetDynamicCs, StaGetRssi|Not station API; optional telemetry|
|342–349|Set/GetProtocols, Set/GetBandwidths, Set/GetBand, Set/GetBandMode|Not station API; reference/version-dependent extensions|

## RPC events: current behavior and required disposition

All eight events now have bounded payload decoders and an explicit owner policy.
This is host/native-build coverage, not installed-peer qualification.

| ID | Event / reference payload | Current owner | Required station disposition |
| --- | --- | --- | --- |
|769|ESPInit / init_data bytes|Typed; one pre-ready marker allowed; duplicate or post-ready marker retires owner|Distinct from transport INIT TLVs; installed ordering still requires qualification.|
|770|Heartbeat / hb_num|Typed validated diagnostic|No liveness claim while ConfigHeartbeat is not enabled.|
|771|AP_StaConnected / status, MAC, AID, mesh flag|Typed then refused in STA-only owner|Unsupported mode retires session; no station association authority.|
|772|AP_StaDisconnected / same plus reason|Typed then refused in STA-only owner|Unsupported mode retires session; no station disconnect authority.|
|773|WifiEventNoArgs / status, event_id|Typed scalar; START/STOP invalidate ledger|Classify every generic ID below; critical security/loss events must not remain metadata.|
|774|StaScanDone / status, nested scan status/count/id|Typed validated diagnostic|No uncorrelated event completes another scan. Blocking RPC remains completion authority until async contract exists.|
|775|StaConnected / status, nested SSID/BSSID/channel/auth|Typed owned UP hint|Fresh event + current identity/security gate; no ACK/AP-only completion. UID0 still lacks attempt identity.|
|776|StaDisconnected / status, nested identity/reason/RSSI|Typed owned DOWN hint|Invalidate acceptance; native confirmed-link loss preserves exact opener/cookie publication in service models. Typed admission and real reconnect qualification remain open.|

## Generic WiFi event IDs (EVENT773)

Pinned IDF declares IDs0–45;46 is invalid sentinel. Hosted forwards all WiFi
events other than scan, station connect/disconnect and AP station connect/
disconnect through773. Generic serialization preserves only event_id, not
the original event data. Do not invent old/new auth, RSSI, channel or other
lost values from receipt of773.

Producer audit: `slave/main/slave_control.c:649–697` selects dedicated versus
generic events; converters are at2373–2581. ESPInit is sent by
`slave/main/app_main.c:921–925`; its converter currently leaves init_data
empty. Heartbeat conversion reads a global counter after asynchronous enqueue,
so its number is not a precise timestamp/sequence fence. Generic conversion
reads only the first byte of the int32 event ID; current0–45 values fit, but
future IDs beyond255 would not. A declared authmode-change message elsewhere
in the schema does not restore the old/new values discarded by this producer.

| ID(s) | IDF event(s) | Required station policy / present gap |
| --- | --- | --- |
|0|WIFI_READY|Validated informational event, not authenticated link|
|1|SCAN_DONE|Normally dedicated774; generic occurrence refused|
|2–3|STA_START, STA_STOP|Already invalidate observation/confirmation; not a complete producer barrier|
|4–5|STA_CONNECTED, STA_DISCONNECTED|Normally dedicated775/776; generic occurrence refused|
|6|STA_AUTHMODE_CHANGE|Valid successful773 now refuses dispatch and retires owner; no old negotiated security survives retirement. Fresh operation required; native reconnect/cookie lifecycle remains open.|
|7–11|STA_WPS_ER_SUCCESS/FAILED/TIMEOUT/PIN/PBC_OVERLAP|WPS not enabled; refused|
|12–13|AP_START, AP_STOP|Refused in STA-only session|
|14–15|AP_STACONNECTED, AP_STADISCONNECTED|Normally771/772; refused|
|16–17|AP_PROBEREQRECVED, FTM_REPORT|Out of scope; refused|
|18|STA_BSS_RSSI_LOW|Optional telemetry; threshold/data lost; do not infer disconnect|
|19–20|ACTION_TX_STATUS, ROC_DONE|No corresponding operation enabled|
|21|STA_BEACON_TIMEOUT|Suspend link publication; force fresh current AP query; refuse during pending association. No fabricated disconnect/security change.|
|22|CONNECTIONLESS_MODULE_WAKE_INTERVAL_START|No corresponding operation enabled|
|23–27|AP_WPS_RG_SUCCESS/FAILED/TIMEOUT/PIN/PBC_OVERLAP|SoftAP WPS, refused|
|28–34|ITWT_SETUP/TEARDOWN/PROBE/SUSPEND, TWT_WAKEUP, BTWT_SETUP/TEARDOWN|Power-management notifications; no liveness/security authority|
|35–42|NAN_STARTED/STOPPED/SVC_MATCH/REPLIED/RECEIVE, NDP_INDICATION/CONFIRM/TERMINATED|NAN/NDP refused|
|43|HOME_CHANNEL_CHANGE|Suspend link publication and force fresh query; actual changed channel cannot inherit exact typed acceptance. Refuse during pending association.|
|44|STA_NEIGHBOR_REP|Roaming telemetry unsupported; cannot authorize implicit reconnect|
|45|AP_WRONG_PASSWORD|SoftAP notification; refused, not a STA password failure|
|46, negative, future IDs|Invalid/unknown|Refused; no silent forward-compatible security authority|

## Implementation gate before typed provider advertisement

### INIT feature and repeated lifecycle audit (2026-10-11)

The actual `runtime_probe.c:wifi_reference_profile` requests feature_caps129
(0x81), including the pinned IDF5.4.3 SAE bit0; NVS remains disabled. Hosted
copies this field over its default config. A missing host SAE feature bit is
therefore excluded for this source profile, not for an unidentified installed
binary. The pinned protobuf oracle now extracts the runtime initializer and
validates its serialized fields rather than relying on a copied test fixture.
Host feature requests cannot prove that the C6 binary compiled SAE support.

STOP281/DEINIT279 followed by INIT278/MODE260/START280 and observed STA_START
is a candidate existing-protocol lifecycle fence. IDF's public DEINIT contract
stops the WiFi task; the separate default event loop persists. START marks
earlier events already queued on that loop and forwarded through the Hosted
FIFO. This is stronger than Disconnect/NOT_CONNECT, but requires qualification
of producer termination and the installed peer; it is not an unconditional
flush guarantee derived from an API ACK.

The pinned Hosted implementation also registers a new any-ID handler on each
INIT and never unregisters it in DEINIT. IDF permits distinct registrations
of the same callback. Repeated cycles therefore leave duplicate handlers and
overwrite the sole saved instance handle. Do not introduce automatic repeated
INIT cycles as an assumed exactly-once recovery path. This reference defect
does not prove that the unidentified installed C6 has the same implementation.
Keep C6 deployment postponed; first choose/qualify a compatible lifecycle
contract, with recovery-backed firmware work only if necessary.
The separate `tools/hosted-reference/slave-lifecycle.patch` fixes repeated
registration for the pinned source: one handler is registered before INIT and
retained across DEINIT. Registration errors propagate before driver init. This
candidate assumes serialized requests and a persistent loop; it is not applied
to the installed peer and does not resolve UID0 attempt provenance by itself.

1. Source/build verified: typed bounded validation for every accepted RPC event; explicit dispatch
   classification, including unsupported modes and unknown future IDs.
2. Source/build verified: invalidation on unexpected peer reinit/security change; beacon/channel events
   reconcile conservatively without fabricating their missing payloads.
3. Source/build verified: scan completion and optional heartbeat semantics separated from link state.
4. Owner retirement and native opener/cookie service publication are host/build
   verified, including separate confirmed teardown identity across unrelated
   new-attempt cancellation. Typed admission, actual recovery and end-to-end
   duplicate, malformed, delayed, reordered and unsolicited peer qualification
   remain open.
5. Resolve/qualify installed peer version and attempt-ordering assumptions;
   runtime receipt revisions are not C6-generated attempt IDs.
6. Typed leave protects a foreign current selection and failed old teardown
   retains the current acknowledged replacement's exact-cookie failure.
   A valid pre-ACK replacement now remains owned until acknowledgment; the
   closed fault gate forbids radio admission and service then emits its one
   exact-cookie failure. Offline/cancellation still discard it. Host-model
   and native-build evidence is not installed C6 event-order qualification.
   Standalone local leave is accepted before the health gate; unavailable
   radio state cannot prevent requester-local cancellation or introduce new
   physical teardown. Already-owed teardown and foreign selections survive.
   These are native-build/host-model repairs, not installed-peer behavior.
   Guarded BeginIO/capability integration is implemented, default-off until
   peer qualification. CONNECT_FAILED now follows that same admission gate;
   default-off configurations still advertise SUMMARY only, never FOURWAY.
   Provider/caller negotiation is host/native-build verified, not an installed
   end-to-end result. Then native WPA3/PMF, DHCP, routes,
   DNS, bidirectional data, reconnect and original sustained-load/privacy/reset
   gates. No C6 update or AP downgrade is implied by this source audit.

This inventory deliberately precedes additional interactive association tests.
The initial inventory involved no executable change. Subsequent security-change
retirement and auxiliary event integration implement the policies above; their
test/build identities and limitations are recorded in ROADMAP.md. No serial
opening, flash, SD write or credential transmission was performed.
