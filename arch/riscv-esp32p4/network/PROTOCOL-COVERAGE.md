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

All eight envelopes currently reach the owner. Only773/775/776 receive typed
payload validation there. Unknown/other event metadata is counted, not qualified.

| ID | Event / reference payload | Current owner | Required station disposition |
| --- | --- | --- | --- |
|769|ESPInit / init_data bytes|Count only|Validate bounded payload; unexpected peer reinitialization must invalidate session/link. Distinguish RPC event from transport INIT TLV handshake.|
|770|Heartbeat / hb_num|Count only|Validate/count if accepted; no liveness claim while ConfigHeartbeat is not enabled.|
|771|AP_StaConnected / status, MAC, AID, mesh flag|Count only|Explicit unexpected SoftAP policy; no station association authority.|
|772|AP_StaDisconnected / same plus reason|Count only|Explicit unexpected SoftAP policy; no station disconnect authority.|
|773|WifiEventNoArgs / status, event_id|Typed scalar; START/STOP invalidate ledger|Classify every generic ID below; critical security/loss events must not remain metadata.|
|774|StaScanDone / status, nested scan status/count/id|Count only|Bounded decode and scan bookkeeping; no uncorrelated event may complete another scan. Blocking RPC remains completion authority until async contract exists.|
|775|StaConnected / status, nested SSID/BSSID/channel/auth|Typed owned UP hint|Fresh event + current identity/security gate; no ACK/AP-only completion. UID0 still lacks attempt identity.|
|776|StaDisconnected / status, nested identity/reason/RSSI|Typed owned DOWN hint|Invalidate acceptance and publish exact opener/link loss; reconnect/cookie publication still incomplete.|

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
|1|SCAN_DONE|Normally dedicated774; generic occurrence needs explicit unexpected-event policy|
|2–3|STA_START, STA_STOP|Already invalidate observation/confirmation; not a complete producer barrier|
|4–5|STA_CONNECTED, STA_DISCONNECTED|Normally dedicated775/776; generic occurrence lacks identity and must not establish link|
|6|STA_AUTHMODE_CHANGE|Valid successful773 now refuses dispatch and retires owner; no old negotiated security survives retirement. Fresh operation required; native reconnect/cookie lifecycle remains open.|
|7–11|STA_WPS_ER_SUCCESS/FAILED/TIMEOUT/PIN/PBC_OVERLAP|WPS not enabled; explicitly unsupported, no association authority|
|12–13|AP_START, AP_STOP|Unexpected mode policy in STA-only session|
|14–15|AP_STACONNECTED, AP_STADISCONNECTED|Normally771/772; explicitly unsupported|
|16–17|AP_PROBEREQRECVED, FTM_REPORT|Out of scope; bounded informational/rejection policy|
|18|STA_BSS_RSSI_LOW|Optional telemetry; threshold/data lost; do not infer disconnect|
|19–20|ACTION_TX_STATUS, ROC_DONE|No corresponding operation enabled|
|21|STA_BEACON_TIMEOUT|Critical loss suspicion: invalidate/reconcile link; currently only counted|
|22|CONNECTIONLESS_MODULE_WAKE_INTERVAL_START|No corresponding operation enabled|
|23–27|AP_WPS_RG_SUCCESS/FAILED/TIMEOUT/PIN/PBC_OVERLAP|SoftAP WPS, unsupported|
|28–34|ITWT_SETUP/TEARDOWN/PROBE/SUSPEND, TWT_WAKEUP, BTWT_SETUP/TEARDOWN|Power-management notifications; no liveness/security authority|
|35–42|NAN_STARTED/STOPPED/SVC_MATCH/REPLIED/RECEIVE, NDP_INDICATION/CONFIRM/TERMINATED|NAN/NDP unsupported|
|43|HOME_CHANNEL_CHANGE|Reconcile current AP/channel; payload lost; currently only counted|
|44|STA_NEIGHBOR_REP|Roaming telemetry unsupported; cannot authorize implicit reconnect|
|45|AP_WRONG_PASSWORD|SoftAP notification, not STA authentication failure|
|46, negative, future IDs|Invalid/unknown|Explicit bounded refusal or diagnostic-only policy; never retain security authority by default|

## Implementation gate before typed provider advertisement

1. Typed bounded validation for every accepted RPC event; explicit dispatch
   classification, including unsupported modes and unknown future IDs.
2. Immediate invalidation on peer reinit/security change; beacon/channel events
   reconcile conservatively without fabricating their missing payloads.
3. Scan completion and optional heartbeat semantics separated from link state.
4. Coupled owner retirement/recovery and native opener/cookie publication tests;
   duplicate, malformed, delayed, reordered and unsolicited messages included.
5. Resolve/qualify installed peer version and attempt-ordering assumptions;
   runtime receipt revisions are not C6-generated attempt IDs.
6. Complete BeginIO/capability integration, then native WPA3/PMF, DHCP, routes,
   DNS, bidirectional data, reconnect and original sustained-load/privacy/reset
   gates. No C6 update or AP downgrade is implied by this source audit.

This inventory deliberately precedes additional interactive association tests.
The initial inventory involved no executable change. Subsequent security-change
retirement passes callback313/owner8200 checks each O0/O2 sanitized and native
resource build; this updates coverage for ID6 only. No serial opening, flash,
SD write or credential transmission was performed.
