/*
 * WPA Supplicant - AmigaOS/MorphOS/AROS SANA-II driver interface
 * Copyright (c) 2010-2013, Neil Cafferkey
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "includes.h"

#include "common.h"
#include "driver.h"
#include "eloop.h"

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/sana2.h>
#include <devices/sana2wireless.h>
#include <devices/sana2wireless_fullmac.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/dos.h>

#ifndef UPINT
#ifdef __AROS__
typedef IPTR UPINT;
typedef SIPTR PINT;
#else
typedef ULONG UPINT;
typedef LONG PINT;
#endif
#endif

#define ETH_MTU 1500
#define MAX_SSID_LEN 32

#define MAX_TX_POWER 15

#define PUDDLE_SIZE 2000
#define MLME_REQ_COUNT 5
#define NULL_INTERVAL 30

int using_wep = 0;

void wpa_scan_results_free(struct wpa_scan_results *res);

static const char device_dir[] = "Networks/";

static const u8 broadcast_addr[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

#ifdef  __AROS__
static BOOL wpa_driver_sana2_buffer_hook(UBYTE *dest, UBYTE *src,
	ULONG size)
{
	CopyMem(src, dest, size);
	return TRUE;
}
#else
/* m68k code to do the same as above */
static const ULONG wpa_driver_sana2_buffer_hook[] =
	{0xC1492C78, 0x00044EAE, 0xFD907001, 0x4E754E71};
#endif

static const struct TagItem buffer_tags[] = {
	{S2_CopyToBuff, (UPINT)wpa_driver_sana2_buffer_hook},
	{S2_CopyFromBuff, (UPINT)wpa_driver_sana2_buffer_hook},
	{TAG_END, 0}
};

struct wpa_driver_sana2_data {
	void *ctx;
	char *device_name;
	struct MsgPort *port;
	struct IOSana2Req *request;
	struct IOSana2Req *event_request;
	struct IOSana2Req *eapol_request;
	struct IOSana2Req *mlme_requests[MLME_REQ_COUNT];
	ULONG event_mask;
	u8 addr[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	char ssid[MAX_SSID_LEN + 1];
	int freq;
	int device_opened;
	int signal_registered;
	int hard_mac;
	int scanning;
	int online;
	/* An attempted WEP install may have changed hardware even on failure. */
	int wep_key_attempted;
	/* Queried and retained per open device instance; never use global defaults. */
	int wireless_caps_queried;
	int wireless_caps_valid;
	struct Sana2WirelessCapabilities wireless_caps;
	int fullmac_caps_valid;
	struct Sana2FullMACCapabilities fullmac_caps;
	ULONG next_association_cookie;
	ULONG active_association_cookie;
};

static int b_rates[] = {
	10,
	20,
	55,
	110
};

static int g_rates[] = {
	10,
	20,
	55,
	110,
	60,
	90,
	120,
	180,
	240,
	360,
	480,
	540
};


static int wpa_driver_sana2_get_ssid(void *priv, u8 *ssid)
{
	struct wpa_driver_sana2_data *drv = priv;
	u16 len;

	len = strlen(drv->ssid);
	os_memcpy(ssid, drv->ssid, len);

	return len;
}


static int wpa_driver_sana2_get_bssid(void *priv, u8 *bssid)
{
	struct wpa_driver_sana2_data *drv = priv;
	u8 *cur_bssid = NULL;
	struct TagItem *tag_list;
	struct IOSana2Req *request = drv->request;
	APTR pool;

	request->ios2_Req.io_Command = S2_GETNETWORKINFO;
	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	request->ios2_Data = pool;
	if (pool != NULL && DoIO((APTR)request) == 0) {
		tag_list = request->ios2_StatData;
		cur_bssid = (u8 *)GetTagData(S2INFO_BSSID, (UPINT)NULL,
			tag_list);
	}
	if (cur_bssid != NULL) {
		os_memcpy(bssid, cur_bssid, ETH_ALEN);
		os_memcpy(drv->bssid, cur_bssid, ETH_ALEN);
	}
	DeletePool(pool);

	return (cur_bssid != NULL) ? 0 : 1;
}


static int wpa_driver_sana2_scan(void *priv,
	struct wpa_driver_scan_params *params)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err = 0;
	struct IOSana2Req *request = drv->request;
	const u8 *ssid;
	size_t ssid_len;
	TEXT *ssid_str = NULL;
	struct TagItem *tag_list;
	APTR pool;

	if (drv->scanning) {
		printf("[scan] MULTIPLE CONCURRENT SCAN REQUESTS!\n");
		Delay(200);
		return 1;
	}

	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	if (pool == NULL)
		err = 1;

	if (err == 0) {
		ssid = params->ssids[0].ssid;
		ssid_len = params->ssids[0].ssid_len;
		if (ssid != NULL) {
			ssid_str = AllocPooled(pool, ssid_len + 1);
			if (ssid_str != NULL) {
				CopyMem(ssid, ssid_str, ssid_len);
				ssid_str[ssid_len] = '\0';
			} else
				err = 1;
		}
	}

	if (err == 0) {
		tag_list = AllocPooled(pool, sizeof(struct TagItem) * 2);
		if (tag_list == NULL)
			err = 1;
	}

	if (err == 0) {
		tag_list[0].ti_Tag = S2INFO_SSID;
		tag_list[0].ti_Data = (UPINT)ssid_str;
		tag_list[1].ti_Tag = TAG_END;

		request->ios2_Req.io_Command = S2_GETNETWORKS;
		request->ios2_Data = pool;
		request->ios2_StatData = tag_list;
		SendIO((APTR)request);
		drv->scanning = 1;
	}

	return err;
}


static void wpa_driver_sana2_set_security_summary(
	struct wpa_driver_sana2_data *drv, struct wpa_scan_res *res,
	struct TagItem **tag_lists, size_t index)
{
	struct TagItem *tag;
	const struct Sana2WirelessSecuritySummary *source;
	struct wpa_security_summary *summary = &res->security_summary;
	const ULONG known_flags = S2WIRELESS_SUMF_KNOWN |
		S2WIRELESS_SUMF_PRIVACY;
	const ULONG no_suites = 0;

	if (!drv->wireless_caps_valid ||
	    !(drv->wireless_caps.Flags & S2WIRELESS_CAPF_SECURITY_SUMMARY))
		return;

	/* A missing or unusable summary from a summary-capable device is not
	 * evidence that the network is open. */
	os_memset(summary, 0, sizeof(*summary));
	summary->flags = WPA_SECURITY_SUMMARY_PRESENT |
		WPA_SECURITY_SUMMARY_PRIVACY;
	res->caps |= IEEE80211_CAP_PRIVACY;
	tag = FindTagItem(S2INFO_SecuritySummary, tag_lists[index]);
	if (tag == NULL || tag->ti_Data == 0)
		return;

	source = (const struct Sana2WirelessSecuritySummary *) (UPINT)
		tag->ti_Data;
	if (source->Size != (ULONG) sizeof(*source) ||
	    source->Version != S2WIRELESS_SUMMARY_VERSION ||
	    (source->Flags & ~known_flags) != 0)
		return;

	if (source->Flags == S2WIRELESS_SUMF_KNOWN &&
	    source->Protocols == no_suites &&
	    source->KeyManagement == no_suites &&
	    source->PairwiseCiphers == no_suites &&
	    source->GroupCiphers == no_suites) {
		summary->flags = WPA_SECURITY_SUMMARY_PRESENT |
			WPA_SECURITY_SUMMARY_KNOWN;
		res->caps &= ~IEEE80211_CAP_PRIVACY;
		return;
	}

	if (source->Flags == (S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY) &&
	    source->Protocols == S2WIRELESS_PROTO_RSN_SAE &&
	    source->KeyManagement == S2WIRELESS_KEY_SAE &&
	    source->PairwiseCiphers == S2WIRELESS_CIPHER_CCMP &&
	    source->GroupCiphers == S2WIRELESS_CIPHER_CCMP) {
		summary->flags |= WPA_SECURITY_SUMMARY_KNOWN;
		summary->proto = WPA_PROTO_RSN;
		summary->key_mgmt = WPA_KEY_MGMT_SAE;
		summary->pairwise_cipher = WPA_CIPHER_CCMP;
		summary->group_cipher = WPA_CIPHER_CCMP;
		return;
	}

	if (source->Flags != (S2WIRELESS_SUMF_KNOWN |
			      S2WIRELESS_SUMF_PRIVACY) ||
	    source->KeyManagement != S2WIRELESS_KEY_PSK ||
	    (source->Protocols != S2WIRELESS_PROTO_WPA &&
	     source->Protocols != S2WIRELESS_PROTO_RSN) ||
	    (source->PairwiseCiphers != S2WIRELESS_CIPHER_TKIP &&
	     source->PairwiseCiphers != S2WIRELESS_CIPHER_CCMP) ||
	    (source->GroupCiphers != S2WIRELESS_CIPHER_TKIP &&
	     source->GroupCiphers != S2WIRELESS_CIPHER_CCMP))
		return;

	summary->flags = WPA_SECURITY_SUMMARY_PRESENT |
		WPA_SECURITY_SUMMARY_KNOWN | WPA_SECURITY_SUMMARY_PRIVACY;
	summary->proto = source->Protocols == S2WIRELESS_PROTO_WPA ?
		WPA_PROTO_WPA : WPA_PROTO_RSN;
	summary->key_mgmt = WPA_KEY_MGMT_PSK;
	summary->pairwise_cipher =
		source->PairwiseCiphers == S2WIRELESS_CIPHER_TKIP ?
		WPA_CIPHER_TKIP : WPA_CIPHER_CCMP;
	summary->group_cipher =
		source->GroupCiphers == S2WIRELESS_CIPHER_TKIP ?
		WPA_CIPHER_TKIP : WPA_CIPHER_CCMP;
}


static struct wpa_scan_results * wpa_driver_sana2_get_scan_results(void *priv)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err = 0;
	struct IOSana2Req *request = drv->request;
	struct wpa_scan_results *results;
	size_t i, count;
	struct TagItem **tag_lists, *tag;
	TEXT *str;
	UPINT num;
	u8 *ie;
	u16 ie_len, ssid_len, *data;

	if (request->ios2_Req.io_Command != S2_GETNETWORKS) return NULL;

	count = request->ios2_DataLength;
	tag_lists = request->ios2_StatData;

	results = os_zalloc(sizeof(struct wpa_scan_results));
	if (results == NULL) {
		err = 1;
	}

	if (err == 0) {
		results->res =
			os_zalloc(count * sizeof(struct wpa_scan_res *));
		if (results->res != NULL)
			results->num = count;
		else
			err = 1;
	}

	for (i = 0; i < count && err == 0; i++) {
		struct wpa_scan_res *res;
		data = (u16 *)GetTagData(S2INFO_InfoElements,
			(UPINT)NULL, tag_lists[i]);
		if (data != NULL)
			ie_len = data[0];
		else
			ie_len = MAX_SSID_LEN + 2;
		res = results->res[i] =
			os_zalloc(sizeof(struct wpa_scan_res) + ie_len);
		if (res == NULL)
			err = 1;

		if (err == 0) {
			tag = FindTagItem(S2INFO_BSSID, tag_lists[i]);
			if (tag != NULL)
				os_memcpy(res->bssid, (void *)tag->ti_Data,
					ETH_ALEN);

			num = GetTagData(S2INFO_Channel, 0, tag_lists[i]);
			if (num == 14)
				res->freq = 2484;
			else if (num != 0)
				res->freq = 2407 + num * 5;

			res->beacon_int = GetTagData(S2INFO_BeaconInterval,
				0, tag_lists[i]);

			res->caps = GetTagData(S2INFO_Capabilities, 0,
				tag_lists[i]);

			res->level = GetTagData(S2INFO_Signal, 0, tag_lists[i]);

			res->noise = GetTagData(S2INFO_Noise, 0, tag_lists[i]);

			if (res->level > res->noise)
				res->qual = res->level - res->noise;

			wpa_driver_sana2_set_security_summary(drv, res,
				tag_lists, i);

			/* Copy all IEs if available, or make fake IEs
			 * containing just SSID */
			ie = (u8 *)(res + 1);
			if (data != NULL) {
				res->ie_len = ie_len;
				os_memcpy(ie, data + 1, ie_len);
			} else {
				str = (TEXT *)GetTagData(S2INFO_SSID,
					(UPINT)NULL, tag_lists[i]);
				if (str != NULL) {
					ssid_len = strlen((char *)str);
					res->ie_len = ssid_len + 2;
					ie[0] = 0;
					ie[1] = ssid_len;
					os_memcpy(ie + 2, str, ssid_len);
				}
			}

		}
	}
	DeletePool(request->ios2_Data);

	if (err != 0) {
		wpa_scan_results_free(results);
		results = NULL;
	}

	drv->scanning = 0;
	return results;
}


static int wpa_driver_sana2_cipher(int cipher)
{
	switch (cipher) {
	case CIPHER_NONE:
		return S2ENC_NONE;
	case CIPHER_WEP40:
	case CIPHER_WEP104:
		return S2ENC_WEP;
	case CIPHER_TKIP:
		return S2ENC_TKIP;
	case CIPHER_CCMP:
		return S2ENC_CCMP;
	default:
		return 0;
	}
}


static int wpa_driver_sana2_band(enum hostapd_hw_mode mode)
{
	switch (mode) {
	case HOSTAPD_MODE_IEEE80211B:
		return S2BAND_B;
	case HOSTAPD_MODE_IEEE80211G:
		return S2BAND_G;
	case HOSTAPD_MODE_IEEE80211A:
		return S2BAND_A;
	default:
		return 0;
	}
}


static int wpa_driver_sana2_set_key(const char *ifname, void *priv,
	enum wpa_alg alg, const u8 *addr, int key_idx, int set_tx,
	const u8 *seq, size_t seq_len, const u8 *key, size_t key_len)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err = 0;
	struct IOSana2Req *request = drv->request;

	request->ios2_Req.io_Command = S2_SETKEY;
	request->ios2_WireError = key_idx;
	request->ios2_PacketType = wpa_driver_sana2_cipher(alg);
	request->ios2_DataLength = key_len;
	request->ios2_Data = (u8 *) key;
	request->ios2_StatData = (u8 *) seq;
	/* Which key this is - pairwise or group - is not in the arguments, only
	 * in who it is for, so the address has to travel with it. */
	if (addr != NULL)
		CopyMem((APTR)addr, request->ios2_DstAddr, ETH_ALEN);
	else
		os_memset(request->ios2_DstAddr, 0, ETH_ALEN);

	/* Work-around for a bug in MLME code */
	if (wpa_driver_sana2_cipher(alg) == S2ENC_WEP)
		using_wep = 1;
	if (alg == WPA_ALG_WEP && key_len != 0)
		drv->wep_key_attempted = 1;

	err = DoIO((APTR)request);
	if (request->ios2_Req.io_Error != 0)
		err = -1;
	request->ios2_Data = NULL;
	request->ios2_StatData = NULL;

	return err == 0 ? 0 : -1;
}


static int wpa_driver_sana2_associate_fullmac(
	struct wpa_driver_sana2_data *drv,
	struct wpa_driver_associate_params *params)
{
	struct Sana2FullMACAssociation selection;
	struct Sana2WirelessCredential credential, *secret = NULL;
	struct IOSana2Req *request;
	char ssid[MAX_SSID_LEN + 1];
	size_t length, i;
	ULONG cookie, channel = 0;
	int err;

	if (!drv || !params || !drv->request || !drv->hard_mac ||
	    !drv->fullmac_caps_valid || !drv->wireless_caps_valid ||
	    !(drv->wireless_caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED) ||
	    !(drv->event_mask & S2EVENT_CONNECT_FAILED) ||
	    params->mode != IEEE80211_MODE_INFRA ||
	    !params->ssid || !params->ssid_len ||
	    params->ssid_len > MAX_SSID_LEN || params->wpa_ie_len ||
	    params->wpa_ie || params->fullmac_allow_diagnostic > 1 ||
	    params->auth_alg != WPA_AUTH_ALG_OPEN ||
	    drv->next_association_cookie == 0xffffffffUL)
		return -1;
	for (i = 0; i < params->ssid_len; ++i)
		if (!params->ssid[i])
			return -1;
	for (i = 0; i < 4; ++i)
		if (params->wep_key_len[i] || params->wep_key[i])
			return -1;
	if (params->freq) {
		if (params->freq == 2484)
			channel = 14;
		else if (params->freq >= 2412 && params->freq <= 2472 &&
			 (params->freq - 2407) % 5 == 0)
			channel = (params->freq - 2407) / 5;
		else
			return -1;
	}
	os_memset(&selection, 0, sizeof(selection));
	os_memset(&credential, 0, sizeof(credential));
	selection.Size = sizeof(selection);
	selection.Version = S2FULLMAC_VERSION;
	selection.Profile = params->fullmac_profile;
	selection.Flags = params->fullmac_allow_diagnostic ?
		S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0;
	selection.SAEPweMethod = params->fullmac_sae_pwe_mode;
	credential.Size = sizeof(credential);
	credential.Version = S2WIRELESS_CREDENTIAL_VERSION;
	if (selection.Profile == S2FULLMAC_PROFILE_OPEN) {
		if (params->key_mgmt_suite != KEY_MGMT_NONE || params->passphrase ||
		    params->psk || params->sae_password || params->sae_password_len ||
		    params->pairwise_suite != CIPHER_NONE ||
		    params->group_suite != CIPHER_NONE)
			return -1;
	} else {
		if (params->pairwise_suite != CIPHER_CCMP ||
		    params->group_suite != CIPHER_CCMP)
			return -1;
		secret = &credential;
		if (selection.Profile == S2FULLMAC_PROFILE_SAE_CCMP_PMF) {
			if (params->key_mgmt_suite != KEY_MGMT_SAE ||
			    params->passphrase || params->psk ||
			    params->sae_password_len > 63)
				return -1;
			selection.PMFPolicy = S2FULLMAC_PMF_REQUIRED;
			credential.Kind = S2WIRELESS_CRED_SAE_PASSWORD;
			credential.Data = params->sae_password;
			credential.Length = params->sae_password_len;
		} else if (selection.Profile == S2FULLMAC_PROFILE_WPA2_CCMP) {
			if (params->key_mgmt_suite != KEY_MGMT_PSK ||
			    params->sae_password || params->sae_password_len ||
			    (params->passphrase && params->psk))
				return -1;
			if (params->passphrase) {
				for (length = 0; length < 64 && params->passphrase[length];
				     ++length) {}
				credential.Kind = S2WIRELESS_CRED_WPA2_PASSPHRASE;
				credential.Data = (const UBYTE *)params->passphrase;
				credential.Length = length;
			} else {
				credential.Kind = S2WIRELESS_CRED_WPA2_PSK;
				credential.Data = params->psk;
				credential.Length = S2WIRELESS_PSK_SIZE;
			}
		} else
			return -1;
	}
	if (!Sana2FullMACAssociationValid(&drv->fullmac_caps, &selection, secret))
		return -1;
	CopyMem(params->ssid, ssid, params->ssid_len);
	ssid[params->ssid_len] = '\0';
	cookie = ++drv->next_association_cookie;
	struct TagItem tags[] = {
		{S2INFO_SSID, (UPINT)ssid},
		{params->bssid ? S2INFO_BSSID : TAG_IGNORE, (UPINT)params->bssid},
		{S2INFO_PortType, S2PORT_MANAGED},
		{S2INFO_Channel, channel},
		{S2INFO_FullMACAssociation, (UPINT)&selection},
		{secret ? S2INFO_WirelessCredential : TAG_IGNORE, (UPINT)secret},
		{S2INFO_AssociationCookie, cookie},
		{TAG_END, 0}
	};
	request = drv->request;
	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_StatData = NULL;
	request->ios2_Data = tags;
	request->ios2_DataLength = 0;
	/* Borrowed objects live through synchronous Reply only. Provider copies
	 * and owns any async secret; ACK is never a CONNECT or completion event. */
	err = DoIO((APTR)request);
	if (request->ios2_Req.io_Error)
		err = -1;
	request->ios2_Data = NULL;
	request->ios2_StatData = NULL;
	if (!err) {
		strcpy(drv->ssid, ssid);
		drv->active_association_cookie = cookie;
	}
	return err ? -1 : 0;
}


static int wpa_driver_sana2_associate(
	void *priv, struct wpa_driver_associate_params *params)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err, i, alg, offload, has_wep = 0;
	ULONG cookie = 0;
	size_t passphrase_len;
	struct IOSana2Req *request;
	char ssid[MAX_SSID_LEN + 1];

	if (params && params->fullmac_profile)
		return wpa_driver_sana2_associate_fullmac(drv, params);
	if (params && (params->sae_password || params->sae_password_len ||
		params->fullmac_allow_diagnostic || params->fullmac_sae_pwe_mode ||
		params->key_mgmt_suite == KEY_MGMT_SAE))
		return -1;

	/* SANA SSID tags are C strings, not length-delimited byte strings.
	 * Reject identities that cannot be represented before any device I/O. */
	if (drv == NULL || params == NULL || drv->request == NULL ||
	    params->ssid == NULL || params->ssid_len == 0 ||
	    params->ssid_len > MAX_SSID_LEN ||
	    (params->wpa_ie_len != 0 && params->wpa_ie == NULL))
		return -1;
	/* The WPA/RSN/WPS contract describes one complete IE. Validate the
	 * caller's readable extent before looking at its embedded length, and
	 * before any device I/O (including legacy key clears). */
	if (params->wpa_ie_len != 0 &&
	    (params->wpa_ie_len < 2 || params->wpa_ie_len > 257 ||
	     (size_t)params->wpa_ie[1] + 2 != params->wpa_ie_len))
		return -1;
	for (i = 0; i < params->ssid_len; i++) {
		if (params->ssid[i] == '\0')
			return -1;
	}
	for (i = 0; i < 4; i++) {
		if (params->wep_key_len[i] != 0 && params->wep_key[i] == NULL)
			return -1;
		if (params->wep_key_len[i] != 0)
			has_wep = 1;
	}
	offload = drv->wireless_caps_valid &&
		(drv->wireless_caps.Flags & S2WIRELESS_CAPF_FOURWAY);
	if (offload) {
		for (i = 0; i < 4; i++) {
			if (params->wep_key_len[i] != 0)
				return -1;
		}
	}
	/* Never forward credentials based only on a caller supplying them.
	 * Raw PSK additionally requires its explicit binary credential capability;
	 * never reinterpret its bytes as a passphrase or silently drop it. */
	if ((params->passphrase != NULL || params->psk != NULL) &&
	    (!offload || params->key_mgmt_suite != KEY_MGMT_PSK ||
	     params->mode != IEEE80211_MODE_INFRA))
		return -1;
	if (offload && params->key_mgmt_suite == KEY_MGMT_PSK &&
	    params->passphrase == NULL &&
	    (params->psk == NULL ||
	     !(drv->wireless_caps.Flags & S2WIRELESS_CAPF_RAW_PSK)))
		return -1;
	if (params->passphrase != NULL) {
		for (passphrase_len = 0; passphrase_len < 64 &&
		     params->passphrase[passphrase_len] != '\0'; passphrase_len++) {
			if ((unsigned char)params->passphrase[passphrase_len] < 32 ||
			    (unsigned char)params->passphrase[passphrase_len] > 126)
				return -1;
		}
		if (passphrase_len < 8 || passphrase_len > 63)
			return -1;
	}
	if (drv->hard_mac && drv->wireless_caps_valid &&
	    (drv->wireless_caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED)) {
		if (drv->next_association_cookie == 0xffffffffUL)
			return -1;
		cookie = ++drv->next_association_cookie;
	}
	request = drv->request;
	CopyMem(params->ssid, ssid, params->ssid_len);
	ssid[params->ssid_len] = '\0';
	/* DoIO waits for Reply. The device must consume/copy these borrowed
	 * options before replying; no allocation survives the call. */
	struct TagItem tag_list[] =
		{{S2INFO_SSID, (UPINT)ssid},
		{(params->bssid != NULL) ? S2INFO_BSSID : TAG_IGNORE,
			(UPINT)params->bssid},
		{S2INFO_PortType, (params->mode == IEEE80211_MODE_IBSS) ?
			S2PORT_ADHOC : S2PORT_MANAGED},
		{S2INFO_Channel, params->freq == 2484 ?
			14 : (params->freq - 2407) / 5},
		{S2INFO_WPAInfo, (params->wpa_ie_len > 0) ?
			(UPINT)params->wpa_ie : (UPINT)NULL},
		{S2INFO_AuthTypes, params->auth_alg},
		/* Only sent if the driver claimed the 4-way offload below; when we
		 * run the handshake ourselves wpa_supplicant leaves these NULL. */
		{(params->passphrase != NULL) ? S2INFO_Passphrase : TAG_IGNORE,
			(UPINT)params->passphrase},
		/* Prefer the validated passphrase when the caller supplies both.
		 * A raw key remains binary and is borrowed only until Reply. */
		{(params->passphrase == NULL && params->psk != NULL) ?
			S2INFO_PSK : TAG_IGNORE, (UPINT)params->psk},
		{(offload && (drv->wireless_caps.Flags &
			S2WIRELESS_CAPF_IE_EXTENT)) ? S2INFO_WPAInfoLength : TAG_IGNORE,
			(UPINT)params->wpa_ie_len},
		{cookie ? S2INFO_AssociationCookie : TAG_IGNORE, (UPINT)cookie},
		{TAG_END, 0}};

	/* Legacy host-handshake devices still set/clear WEP slots. Firmware
	 * offload owns its key state and does not imply S2_SETKEY support. */
	for (i = 0; !offload && i < 4; i++) {
		if (params->wep_key_len[i] > 0)
			alg = WPA_ALG_WEP;
		else
			alg = WPA_ALG_NONE;
		err = wpa_driver_sana2_set_key(NULL, priv, alg, broadcast_addr, i,
			i == params->wep_tx_keyidx, NULL, 0,
			params->wep_key[i], params->wep_key_len[i]);
		if (err != 0 || request->ios2_Req.io_Error != 0) {
			request->ios2_Data = NULL;
			request->ios2_StatData = NULL;
			/* A device with no WEP history may legitimately not implement
			 * empty WEP-slot clears. Never ignore a real install error or
			 * a failure to remove possibly installed WEP keys. */
			if (!has_wep && !drv->wep_key_attempted &&
			    (request->ios2_Req.io_Error == S2ERR_NOT_SUPPORTED ||
			     request->ios2_Req.io_Error == IOERR_NOCMD))
				continue;
			return -1;
		}
	}
	if (!offload && !has_wep)
		drv->wep_key_attempted = 0;

	/* Set other options */
	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_StatData = NULL;
	request->ios2_Data = tag_list;
	err = DoIO((APTR)request);
	if (request->ios2_Req.io_Error != 0)
		err = -1;
	request->ios2_Data = NULL;
	request->ios2_StatData = NULL;

	/* Store current SSID and BSSID */
	if (err == 0) {
		strcpy(drv->ssid, ssid);
		drv->active_association_cookie = cookie;
	}

	return err == 0 ? 0 : -1;
}


static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx);

static int wpa_driver_sana2_disassociate(void *priv, const u8 *addr,
	int reason_code)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request;
	struct TagItem tags[] = {{S2INFO_Disassociate, 1}, {TAG_END, 0}};
	int err;

	/* This is local cancellation, not an assertion about an AP's reason.
	 * CONNECT_FAILED devices must support the standalone leave tag. Keep
	 * legacy devices' existing unsupported-callback behavior unchanged. */
	(void)addr;
	(void)reason_code;
	if (drv == NULL || drv->request == NULL || !drv->hard_mac ||
	    !drv->wireless_caps_valid ||
	    !(drv->wireless_caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED))
		return -1;

	/* Retire locally even if the device refuses leave: canceled work must
	 * not reach the supplicant as a failure of an active attempt. Do not
	 * fabricate DISCONNECT or claim confirmed radio teardown on Reply. */
	drv->active_association_cookie = 0;
	eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);
	request = drv->request;
	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_DataLength = 0;
	request->ios2_WireError = 0;
	request->ios2_StatData = NULL;
	request->ios2_Data = tags;
	err = DoIO((APTR)request);
	if (request->ios2_Req.io_Error != 0)
		err = -1;
	request->ios2_Data = NULL;
	request->ios2_StatData = NULL;
	return err == 0 ? 0 : -1;
}


static int wpa_driver_sana2_wireless_caps_valid(
	const struct Sana2WirelessCapabilities *caps)
{
	const ULONG known_flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_SECURITY_SUMMARY | S2WIRELESS_CAPF_RAW_PSK |
		S2WIRELESS_CAPF_IE_EXTENT | S2WIRELESS_CAPF_CONNECT_FAILED;
	const ULONG known_protocols = S2WIRELESS_PROTO_WPA |
		S2WIRELESS_PROTO_RSN | S2WIRELESS_PROTO_RSN_SAE;
	const ULONG known_key_mgmt = S2WIRELESS_KEY_PSK |
		S2WIRELESS_KEY_EAP | S2WIRELESS_KEY_SAE;
	const ULONG known_ciphers = S2WIRELESS_CIPHER_TKIP |
		S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_GCMP;
	const ULONG fourway_protocols = S2WIRELESS_PROTO_WPA |
		S2WIRELESS_PROTO_RSN;
	const ULONG fourway_ciphers = S2WIRELESS_CIPHER_TKIP |
		S2WIRELESS_CIPHER_CCMP;

	if (caps->SizeAvailable != (ULONG) sizeof(*caps) ||
	    caps->SizeSupplied != (ULONG) sizeof(*caps) ||
	    caps->Version != S2WIRELESS_CAPABILITIES_VERSION ||
	    (caps->Flags & ~known_flags) != 0 ||
	    (caps->Protocols & ~known_protocols) != 0 ||
	    (caps->KeyManagement & ~known_key_mgmt) != 0 ||
	    (caps->PairwiseCiphers & ~known_ciphers) != 0 ||
	    (caps->GroupCiphers & ~known_ciphers) != 0)
		return 0;

	/* The masks describe offload suites. Reject incomplete or unsupported
	 * declarations instead of projecting EAP/SAE/GCMP onto older WPA fields. */
	if (caps->Flags & S2WIRELESS_CAPF_FOURWAY) {
		if (caps->Protocols == 0 ||
		    (caps->Protocols & ~fourway_protocols) != 0 ||
		    caps->KeyManagement != S2WIRELESS_KEY_PSK ||
		    caps->PairwiseCiphers == 0 ||
		    (caps->PairwiseCiphers & ~fourway_ciphers) != 0 ||
		    caps->GroupCiphers == 0 ||
		    (caps->GroupCiphers & ~fourway_ciphers) != 0)
			return 0;
	} else if ((caps->Flags & (S2WIRELESS_CAPF_RAW_PSK |
				 S2WIRELESS_CAPF_IE_EXTENT)) != 0 ||
		   caps->Protocols != 0 || caps->KeyManagement != 0 ||
		   caps->PairwiseCiphers != 0 || caps->GroupCiphers != 0) {
		return 0;
	}

	return 1;
}


static int wpa_driver_sana2_query_wireless_caps(
	struct wpa_driver_sana2_data *drv)
{
	struct Sana2WirelessCapabilities caps;
	struct IOSana2Req *request = drv->request;

	if (request == NULL)
		return 0;

	os_memset(&caps, 0, sizeof(caps));
	caps.SizeAvailable = sizeof(caps);
	caps.Version = S2WIRELESS_CAPABILITIES_VERSION;
	request->ios2_Req.io_Command = S2_GETWIRELESSCAPABILITIES;
	request->ios2_StatData = (APTR) &caps;
	request->ios2_Data = NULL;
	request->ios2_DataLength = 0;
	if (DoIO((APTR) request) != 0 || request->ios2_Req.io_Error != 0 ||
	    !wpa_driver_sana2_wireless_caps_valid(&caps)) {
		request->ios2_StatData = NULL;
		return 0;
	}
	request->ios2_StatData = NULL;

	drv->wireless_caps = caps;
	drv->wireless_caps_valid = 1;
	/* Negotiate independently; legacy FOURWAY and its masks retain v1 meaning.
	 * Require truthful summary and correlated failure/cancel semantics before
	 * making the optional typed path available to any native caller. */
	if (drv->hard_mac && (caps.Flags & S2WIRELESS_CAPF_SECURITY_SUMMARY) &&
	    (caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED)) {
		struct Sana2FullMACCapabilities fullmac;
		os_memset(&fullmac, 0, sizeof(fullmac));
		fullmac.SizeAvailable = sizeof(fullmac);
		fullmac.Version = S2FULLMAC_VERSION;
		request->ios2_Req.io_Command = S2_GETFULLMACCAPABILITIES;
		request->ios2_StatData = &fullmac;
		if (DoIO((APTR)request) == 0 && !request->ios2_Req.io_Error &&
		    Sana2FullMACCapabilitiesValid(&fullmac)) {
			drv->fullmac_caps = fullmac;
			drv->fullmac_caps_valid = 1;
		}
		request->ios2_StatData = NULL;
	}
	return 1;
}


static ULONG wpa_driver_sana2_initial_event_mask(
	struct wpa_driver_sana2_data *drv)
{
	ULONG mask = S2EVENT_OFFLINE;
	if (drv->hard_mac)
		mask |= S2EVENT_CONNECT | S2EVENT_DISCONNECT;
	/* Capability negotiation must precede the first ONEVENT subscription.
	 * get_capa runs later and must not silently change an already parked mask. */
	if (!drv->wireless_caps_queried) {
		drv->wireless_caps_queried = 1;
		wpa_driver_sana2_query_wireless_caps(drv);
	}
	if (drv->hard_mac && drv->wireless_caps_valid &&
	    (drv->wireless_caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED))
		mask |= S2EVENT_CONNECT_FAILED;
	return mask;
}


static int wpa_driver_sana2_get_capa(void *priv, struct wpa_driver_capa *capa)
{
	struct wpa_driver_sana2_data *drv = priv;
	u8 *enc_list = NULL;
	int i;
	struct IOSana2Req *request = drv->request;
	APTR pool;

	os_memset(capa, 0, sizeof(*capa));

	request->ios2_Req.io_Command = S2_GETCRYPTTYPES;
	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	request->ios2_Data = pool;
	if (pool != NULL && DoIO((APTR)request) == 0) {
		enc_list = request->ios2_StatData;
		for (i = 0; i < request->ios2_DataLength; i++)
		{
			switch (enc_list[i]) {
			case S2ENC_WEP:
				capa->enc |= WPA_DRIVER_CAPA_ENC_WEP40
					| WPA_DRIVER_CAPA_ENC_WEP104;
				break;
			case S2ENC_TKIP:
				capa->enc |= WPA_DRIVER_CAPA_ENC_TKIP;
				break;
			case S2ENC_CCMP:
				capa->enc |= WPA_DRIVER_CAPA_ENC_CCMP;
				break;
			}
		}
	} else
		capa->enc = WPA_DRIVER_CAPA_ENC_WEP40 |
			WPA_DRIVER_CAPA_ENC_WEP104 |
			WPA_DRIVER_CAPA_ENC_TKIP |
			WPA_DRIVER_CAPA_ENC_CCMP;
	DeletePool(pool);

	if (capa->enc & WPA_DRIVER_CAPA_ENC_TKIP) {
		capa->key_mgmt |= WPA_DRIVER_CAPA_KEY_MGMT_WPA |
		WPA_DRIVER_CAPA_KEY_MGMT_WPA_PSK;
	}
	if (capa->enc & WPA_DRIVER_CAPA_ENC_CCMP) {
		capa->key_mgmt |= WPA_DRIVER_CAPA_KEY_MGMT_WPA2 |
		WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK;
	}

	if (!drv->hard_mac)
		capa->flags |= WPA_DRIVER_FLAGS_USER_SPACE_MLME;

	if (!drv->wireless_caps_queried) {
		drv->wireless_caps_queried = 1;
		wpa_driver_sana2_query_wireless_caps(drv);
	}

	if (drv->wireless_caps_valid) {
		const struct Sana2WirelessCapabilities *caps =
			&drv->wireless_caps;
		if (drv->hard_mac && (caps->Flags & S2WIRELESS_CAPF_CONNECT_FAILED))
			capa->flags |= WPA_DRIVER_FLAGS_CANCEL_PENDING;

		if (caps->Flags & S2WIRELESS_CAPF_SECURITY_SUMMARY)
			capa->flags |= WPA_DRIVER_FLAGS_SECURITY_SUMMARY;

		if (caps->Flags & S2WIRELESS_CAPF_FOURWAY) {
			unsigned int pairwise_enc = 0;
			unsigned int group_enc = 0;

			/* The legacy capa structure can represent only these two
			 * ciphers and the PSK variants of WPA and RSN. Keep the
			 * role-specific masks separate for summary-aware selection. */
			capa->key_mgmt = 0;
			capa->enc = 0;
			if (caps->Protocols & S2WIRELESS_PROTO_WPA)
				capa->key_mgmt |=
					WPA_DRIVER_CAPA_KEY_MGMT_WPA_PSK;
			if (caps->Protocols & S2WIRELESS_PROTO_RSN)
				capa->key_mgmt |=
					WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK;
			if (caps->PairwiseCiphers & S2WIRELESS_CIPHER_TKIP)
				pairwise_enc |= WPA_DRIVER_CAPA_ENC_TKIP;
			if (caps->PairwiseCiphers & S2WIRELESS_CIPHER_CCMP)
				pairwise_enc |= WPA_DRIVER_CAPA_ENC_CCMP;
			if (caps->GroupCiphers & S2WIRELESS_CIPHER_TKIP)
				group_enc |= WPA_DRIVER_CAPA_ENC_TKIP;
			if (caps->GroupCiphers & S2WIRELESS_CIPHER_CCMP)
				group_enc |= WPA_DRIVER_CAPA_ENC_CCMP;
			capa->enc = pairwise_enc | group_enc;
			if ((caps->Flags & S2WIRELESS_CAPF_SECURITY_SUMMARY) &&
			    (caps->Flags & S2WIRELESS_CAPF_FOURWAY)) {
				capa->summary_pairwise_enc = pairwise_enc;
				capa->summary_group_enc = group_enc;
			}
			capa->flags |= WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;
		}
	}

	if (drv->fullmac_caps_valid) {
		capa->flags |= WPA_DRIVER_FLAGS_FULLMAC_ASSOCIATION;
		capa->fullmac_profiles = drv->fullmac_caps.Profiles;
		capa->fullmac_sae_pwe_modes = drv->fullmac_caps.SAEPweMethods;
		capa->fullmac_diagnostic_risk =
			!!(drv->fullmac_caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK);
	}
	/* Without a valid per-device FOURWAY result, the existing host-side
	 * handshake and S2_SETKEY path remains unchanged. */

	return 0;
}


static int wpa_driver_sana2_send_eapol(void *priv, const u8 *dest, u16 proto,
			  const u8 *data, size_t data_len)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;

	request->ios2_Req.io_Command = CMD_WRITE;
	request->ios2_Req.io_Flags = 0;
	CopyMem(dest, request->ios2_DstAddr, ETH_ALEN);
	request->ios2_PacketType = proto;
	request->ios2_DataLength = data_len;
	request->ios2_Data = (APTR)data;

	return DoIO((APTR)request);
}


static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx)
{
	struct wpa_driver_sana2_data *drv = timeout_ctx;

	wpa_driver_sana2_send_eapol(drv, drv->bssid, 0, NULL, 0);
	eloop_register_timeout(NULL_INTERVAL, 0, wpa_driver_sana2_stay_alive,
			       drv->ctx, drv);
}


static const u8 *wpa_driver_sana2_get_mac_addr(void *priv)
{
	struct wpa_driver_sana2_data *drv = priv;

	return drv->addr;
}


static struct hostapd_hw_modes * wpa_driver_sana2_get_hw_feature_data(
	void *priv, u16 *num_modes, u16 *flags)
{
	int err = 0, i, mode_count, channel_count, rate_count;
	struct hostapd_hw_modes *modes;

	/* IEEE 802.11b */
	mode_count = 2;
	channel_count = 14;
	rate_count = 4;

	modes = os_zalloc(sizeof(struct hostapd_hw_modes) * mode_count);
	if (modes == NULL) {
		err = 1;
	}

	if (err == 0) {
		modes[0].mode = HOSTAPD_MODE_IEEE80211B;

		modes[0].num_channels = channel_count;
		modes[0].channels = os_zalloc(
			channel_count * sizeof(struct hostapd_channel_data));
		if (modes[0].channels == NULL)
			err = 1;
	}

	if (err == 0) {
		for (i = 0; i < 13; i++) {
			modes[0].channels[i].chan = i + 1;
			modes[0].channels[i].freq = 2407 + (i + 1) * 5;
			modes[0].channels[i].flag = 0;
			modes[0].channels[i].max_tx_power = MAX_TX_POWER;
		}
		modes[0].channels[i].chan = 14;
		modes[0].channels[i].freq = 2484;
		modes[0].channels[i].flag = 0;
		modes[0].channels[i].max_tx_power = MAX_TX_POWER;
	}

	if (err == 0) {
		modes[0].num_rates = rate_count;
		modes[0].rates = os_zalloc(rate_count * sizeof(int));
		if (modes[0].rates == NULL)
			err = 1;
	}

	if (err == 0) {
		os_memcpy(modes[0].rates, b_rates, sizeof(b_rates));
	}

	/* IEEE 802.11g */
	channel_count = 13;
	rate_count = 12;

	if (err == 0) {
		modes[1].mode = HOSTAPD_MODE_IEEE80211G;

		modes[1].num_channels = channel_count;
		modes[1].channels = os_zalloc(
			channel_count * sizeof(struct hostapd_channel_data));
		if (modes[1].channels == NULL)
			err = 1;
	}

	if (err == 0) {
		for (i = 0; i < 13; i++) {
			modes[1].channels[i].chan = i + 1;
			modes[1].channels[i].freq = 2407 + (i + 1) * 5;
			modes[1].channels[i].flag = 0;
			modes[1].channels[i].max_tx_power = MAX_TX_POWER;
		}
	}

	if (err == 0) {
		modes[1].num_rates = rate_count;
		modes[1].rates = os_zalloc(rate_count * sizeof(int));
		if (modes[1].rates == NULL)
			err = 1;
	}

	if (err == 0) {
		os_memcpy(modes[1].rates, g_rates, sizeof(g_rates));
	}

	if (err != 0) {
		os_free(modes[0].channels);
		os_free(modes[0].rates);
		os_free(modes[1].channels);
		os_free(modes[1].rates);
		modes = NULL;
	}

	*num_modes = mode_count;
	*flags = 0;
	return modes;
}


static int wpa_driver_sana2_set_channel(void *priv,
	enum hostapd_hw_mode phymode, int chan, int freq)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	struct TagItem tag_list[] =
		{{S2INFO_Band, wpa_driver_sana2_band(phymode)},
		{S2INFO_Channel, chan},
		{TAG_END, 0}};

	drv->freq = freq;
	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list;
	return DoIO((APTR)request);
}


static int wpa_driver_sana2_set_ssid(void *priv, const u8 *ssid,
	size_t ssid_len)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	TEXT ssid_str[ssid_len + 1];
	struct TagItem tag_list[] =
		{{S2INFO_SSID, (UPINT)ssid_str},
		{TAG_END, 0}};

	CopyMem(ssid, ssid_str, ssid_len);
	ssid_str[ssid_len] = '\0';

	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list;
	return DoIO((APTR)request);
}


static int wpa_driver_sana2_set_bssid(void *priv, const u8 *bssid)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	struct TagItem tag_list[] =
		{{S2INFO_BSSID, (UPINT)bssid},
		{TAG_END, 0}};

	CopyMem(bssid, drv->bssid, ETH_ALEN);

	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list;
	return DoIO((APTR)request);
}


static int wpa_driver_sana2_set_associd(void *priv, u16 aid)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	struct TagItem tag_list[] =
		{{S2INFO_AssocID, aid},
		{TAG_END, 0}};

	eloop_register_timeout(NULL_INTERVAL, 0, wpa_driver_sana2_stay_alive,
			       drv->ctx, drv);

	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list;
	return DoIO((APTR)request);
}


static int wpa_driver_sana2_set_capabilities(void *priv, u16 capab)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	struct TagItem tag_list[] =
		{{S2INFO_Capabilities, capab},
		{TAG_END, 0}};

	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list;
	return DoIO((APTR)request);
}


static int wpa_driver_sana2_send_mlme(void *priv, const u8 *data,
	size_t data_len)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;

wpa_printf(MSG_DEBUG, "sana2: sending MLME frame");
	request->ios2_Req.io_Command = S2_WRITEMGMT;
	request->ios2_Req.io_Flags = 0;
	request->ios2_DataLength = data_len;
	request->ios2_Data = (APTR)data;

	return DoIO((APTR)request);
}


static int wpa_driver_sana2_mlme_add_sta(void *priv, const u8 *addr,
	const u8 *supp_rates, size_t supp_rates_len)
{
	return 0;
}


static int wpa_driver_sana2_mlme_remove_sta(void *priv, const u8 *addr)
{
	return 0;
}


static void wpa_driver_sana2_connect_failed(
	struct wpa_driver_sana2_data *drv, const struct IOSana2Req *request)
{
	if (!drv->hard_mac || !drv->wireless_caps_valid ||
	    !(drv->wireless_caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED) ||
	    request->ios2_Req.io_Error != 0 ||
	    request->ios2_WireError != S2EVENT_CONNECT_FAILED ||
	    !drv->active_association_cookie ||
	    request->ios2_DataLength != drv->active_association_cookie)
		return;
	drv->active_association_cookie = 0;
	eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);
	wpa_supplicant_event(drv->ctx, EVENT_ASSOC_FAILED, NULL);
}


static void wpa_driver_sana2_event_handler(int sig, void *sig_ctx)
{
	struct wpa_driver_sana2_data *drv = sig_ctx;
	int i;
	ULONG events;
	struct IOSana2Req *request;
	union wpa_event_data event_data;
	struct TagItem *tag_list;
	u8 *ie;
	APTR pool;

	os_memset(&event_data, 0, sizeof(event_data));

	/* Propagate events */
	while ((request = (APTR)GetMsg(drv->port)) != NULL)
	{
		switch (request->ios2_Req.io_Command)
		{
		case CMD_READ:
			if (request->ios2_Req.io_Error == 0) {
				/* Deliver frame to supplicant */
				drv_event_eapol_rx(drv->ctx,
					request->ios2_SrcAddr,
					request->ios2_Data,
					request->ios2_DataLength);
			}
			if (request->ios2_Req.io_Error != S2ERR_OUTOFSERVICE) {
				/* Send request back for next frame */
				SendIO((APTR)request);
			}
			break;

		case S2_READMGMT:
			if (request->ios2_Req.io_Error == 0) {
				/* Deliver frame to supplicant */
				os_memset(&event_data, 0, sizeof(event_data));
				event_data.mlme_rx.buf = request->ios2_Data;
				event_data.mlme_rx.len =
					request->ios2_DataLength;
				event_data.mlme_rx.freq = drv->freq;
				wpa_supplicant_event(drv->ctx, EVENT_MLME_RX,
					&event_data);
			}
			if (request->ios2_Req.io_Error != S2ERR_OUTOFSERVICE) {
				/* Send request back for next frame */
				request->ios2_DataLength = ETH_MTU;
				SendIO((APTR)request);
			}
			break;

		case S2_ONEVENT:
			events = request->ios2_WireError;
			if (request->ios2_Req.io_Error != 0)
				events = 0;
			if ((events & S2EVENT_CONNECT) &&
			    (!(drv->wireless_caps_valid &&
			       (drv->wireless_caps.Flags & S2WIRELESS_CAPF_CONNECT_FAILED)) ||
			     drv->active_association_cookie != 0))
			{
				drv->active_association_cookie = 0;
				request->ios2_Req.io_Command =
					S2_GETNETWORKINFO;
				pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR,
					PUDDLE_SIZE, PUDDLE_SIZE);
				request->ios2_Data = pool;
				if (pool != NULL && DoIO((APTR)request) == 0)
				{
					tag_list = request->ios2_StatData;
					event_data.assoc_info.req_ies = ie =
						(u8 *)GetTagData(S2INFO_WPAInfo,
							(UPINT)NULL,
							tag_list);
				} else
					ie = NULL;
				if (ie != NULL)
					event_data.assoc_info.req_ies_len =
						ie[1] + 2;
				else
					event_data.assoc_info.req_ies_len = 0;

				wpa_supplicant_event(drv->ctx, EVENT_ASSOC,
					&event_data);

				DeletePool(pool);

				/* Start periodic sending of null-data frames */
				eloop_register_timeout(NULL_INTERVAL, 0,
					wpa_driver_sana2_stay_alive,
					drv->ctx, drv);
			}

			if (events & S2EVENT_DISCONNECT) {
				drv->active_association_cookie = 0;
				wpa_supplicant_event(drv->ctx, EVENT_DISASSOC,
					NULL);

				/* Stop periodic sending of null-data frames */
				eloop_cancel_timeout(
					wpa_driver_sana2_stay_alive,
					drv->ctx, drv);
			}

			if (events & S2EVENT_CONNECT_FAILED)
				wpa_driver_sana2_connect_failed(drv, request);

			if (events & S2EVENT_ONLINE) {
				drv->event_mask &= ~S2EVENT_ONLINE;
				drv->event_mask |= S2EVENT_OFFLINE;

				/* Send read requests back for next frame */
				SendIO((APTR)drv->eapol_request);
				if (!drv->hard_mac)
					for (i = 0; i < MLME_REQ_COUNT; i++)
						SendIO((APTR)
							drv->mlme_requests[i]);
			}

			if (events & S2EVENT_OFFLINE) {
				drv->active_association_cookie = 0;
				drv->event_mask &= ~S2EVENT_OFFLINE;
				drv->event_mask |= S2EVENT_ONLINE;
			}

			/* Send request back to receive next event */
			request->ios2_Req.io_Command = S2_ONEVENT;
			request->ios2_DataLength = 0;
			request->ios2_WireError = drv->event_mask;
			SendIO((APTR)request);
			break;

		case S2_GETNETWORKS:
			wpa_supplicant_event(drv->ctx, EVENT_SCAN_RESULTS,
				NULL);
			break;
		}
	}
}


static void wpa_driver_sana2_deinit(void *priv)
{
	struct wpa_driver_sana2_data *drv = priv;
	int i;

	/* Retire callbacks before destroying anything they may reference. */
	eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);
	if (drv->signal_registered) {
		eloop_unregister_signal(drv->port->mp_SigBit,
			wpa_driver_sana2_event_handler, drv);
		drv->signal_registered = 0;
	}

	/* Abort outstanding I/O requests */
	if (drv->eapol_request != NULL) {
		AbortIO((APTR)drv->eapol_request);
		WaitIO((APTR)drv->eapol_request);
		FreeVec(drv->eapol_request);
	}

	for (i = 0; i < MLME_REQ_COUNT; i++) {
		if (drv->mlme_requests[i] != NULL) {
			AbortIO((APTR)drv->mlme_requests[i]);
			WaitIO((APTR)drv->mlme_requests[i]);
			FreeVec(drv->mlme_requests[i]);
		}
	}

	if (drv->event_request != NULL) {
		AbortIO((APTR)drv->event_request);
		WaitIO((APTR)drv->event_request);
		FreeVec(drv->event_request);
	}

	/* Close device */
	if (drv->device_opened) {
		AbortIO((APTR)drv->request);
		WaitIO((APTR)drv->request);
		CloseDevice((APTR)drv->request);
	}
	if (drv->request != NULL)
		DeleteIORequest((APTR)drv->request);
	if (drv->port != NULL) {
		SetSignal(0, (ULONG)1 << drv->port->mp_SigBit);
		DeleteMsgPort(drv->port);
	}

	if (drv->device_name != NULL)
		FreeVec(drv->device_name);

	os_free(drv);
}


static void * wpa_driver_sana2_init(void *ctx, const char *ifname)
{
	struct wpa_driver_sana2_data *drv;
	int err = 0, i;
	char *end, *p;
	ULONG unit_no;
	struct IOSana2Req *request, *eapol_request, *mlme_request;
	struct NSDeviceQueryResult device_info =
		{0, sizeof(struct NSDeviceQueryResult)};

	drv = os_zalloc(sizeof(*drv));
	if (drv == NULL)
		return NULL;
	drv->ctx = ctx;
	drv->device_name = AllocVec(strlen(device_dir) + strlen(ifname),
		MEMF_PUBLIC);
	if (drv->device_name == NULL)
		err = 1;

	/* Split fake interface name into device and unit */
	if (err == 0) {
		end = strrchr(ifname, ':');
		if (strchr(ifname, ':') == end && strchr(ifname, '/') == NULL) {
			strcpy(drv->device_name, device_dir);
			p = drv->device_name + strlen(device_dir);
		} else
			p = drv->device_name;
		strncpy(p, ifname, end - ifname);
		p[end - ifname] = '\0';

		unit_no = atoi(end + 1);

		drv->port = CreateMsgPort();
		if (drv->port != NULL)
			drv->request = (APTR)CreateIORequest(drv->port,
				sizeof(struct IOSana2Req));
		if (drv->port == NULL || drv->request == NULL)
			err = 1;
	}

	if (err == 0) {
		/* Open device */
		drv->request->ios2_BufferManagement = (APTR)buffer_tags;
		if (OpenDevice((TEXT *)drv->device_name, unit_no,
			(APTR)drv->request, 0) != 0)
			err = 1;
	}

	/* Register a handler for the port's signal */
	if (err == 0) {
		drv->device_opened = 1;

		if (eloop_register_signal(drv->port->mp_SigBit,
			wpa_driver_sana2_event_handler, drv) != 0)
			err = 1;
		else
			drv->signal_registered = 1;
	}

	/* Check if this is a hard-MAC device */
	if (err == 0) {
		request = drv->request;
		((struct IOStdReq *)request)->io_Command = NSCMD_DEVICEQUERY;
		((struct IOStdReq *)request)->io_Data = &device_info;
		if (DoIO((APTR)request) != 0)
			err = 1;
		else {
			for (i = 0; device_info.SupportedCommands[i] != 0; i++)
				if (device_info.SupportedCommands[i]
					== S2_GETNETWORKS)
					drv->hard_mac = 1;
		}
	}

	/* Put device online */
	if (err == 0) {
		request = drv->request;
		request->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
		if (DoIO((APTR)request) != 0)
			err = 1;
	}

	if (err == 0) {
		CopyMem(request->ios2_DstAddr, drv->addr, ETH_ALEN);

		request->ios2_Req.io_Command = S2_CONFIGINTERFACE;
		CopyMem(drv->addr, request->ios2_SrcAddr, ETH_ALEN);

		if (DoIO((APTR)request) != 0) {
			if (request->ios2_WireError == S2WERR_IS_CONFIGURED) {
				request->ios2_Req.io_Command = S2_ONLINE;
				DoIO((APTR)request);
			} else
				err = 1;
		}
		drv->online = 1;
	}

	/* Create and queue up EAPOL frame read request */
	if (err == 0) {
		request = drv->request;
		request->ios2_Req.io_Command = CMD_READ;
		request->ios2_PacketType = ETH_P_EAPOL;

		eapol_request = AllocVec(sizeof(struct IOSana2Req) + ETH_MTU,
			MEMF_PUBLIC | MEMF_CLEAR);
		if (eapol_request == NULL)
			err = 1;
	}

	if (err == 0) {
		CopyMem(drv->request, eapol_request,
			sizeof(struct IOSana2Req));
		eapol_request->ios2_Data = eapol_request + 1;
		drv->eapol_request = eapol_request;

		SendIO((APTR)eapol_request);
	}

	/* Create and queue up management frame read requests */
	if (err == 0 && !drv->hard_mac) {
		request = drv->request;
		request->ios2_Req.io_Command = S2_READMGMT;
		request->ios2_DataLength = ETH_MTU;
		for (i = 0; i < MLME_REQ_COUNT; i++) {
			mlme_request =
				AllocVec(sizeof(struct IOSana2Req) + ETH_MTU,
					MEMF_PUBLIC | MEMF_CLEAR);
			if (mlme_request == NULL)
				err = 1;

			if (err == 0) {
				CopyMem(drv->request, mlme_request,
					sizeof(struct IOSana2Req));
				mlme_request->ios2_Data = mlme_request + 1;
				drv->mlme_requests[i] = mlme_request;

				SendIO((APTR)mlme_request);
			}
		}
	}

	if (err == 0) {
		/* Create and send event-notification request */
		request = drv->request;
		drv->event_mask = wpa_driver_sana2_initial_event_mask(drv);
		request->ios2_Req.io_Command = S2_ONEVENT;
		request->ios2_DataLength = 0;
		request->ios2_WireError = drv->event_mask;

		drv->event_request = AllocVec(sizeof(struct IOSana2Req),
			MEMF_PUBLIC | MEMF_CLEAR);
		if (drv->event_request == NULL)
			err = 1;
	}

	if (err == 0) {
		CopyMem(drv->request, drv->event_request,
			sizeof(struct IOSana2Req));

		SendIO((APTR)drv->event_request);
	}

	if (err != 0) {
		wpa_driver_sana2_deinit(drv);
		drv = NULL;
	}

	return drv;
}


const struct wpa_driver_ops wpa_driver_sana2_ops = {
	.name = "sana2",
	.desc = "SANA-II driver",
	.get_bssid = wpa_driver_sana2_get_bssid,
	.get_ssid = wpa_driver_sana2_get_ssid,
	.set_key = wpa_driver_sana2_set_key,
	.init = wpa_driver_sana2_init,
	.deinit = wpa_driver_sana2_deinit,
	.scan2 = wpa_driver_sana2_scan,
	.associate = wpa_driver_sana2_associate,
	.disassociate = wpa_driver_sana2_disassociate,
	.deauthenticate = wpa_driver_sana2_disassociate,
	.get_capa = wpa_driver_sana2_get_capa,
	.get_mac_addr = wpa_driver_sana2_get_mac_addr,
	.send_eapol = wpa_driver_sana2_send_eapol,
	.get_scan_results2 = wpa_driver_sana2_get_scan_results,
	.get_hw_feature_data = wpa_driver_sana2_get_hw_feature_data,
	.set_channel = wpa_driver_sana2_set_channel,
	.set_ssid = wpa_driver_sana2_set_ssid,
	.set_bssid = wpa_driver_sana2_set_bssid,
	.set_associd = wpa_driver_sana2_set_associd,
	.set_capabilities = wpa_driver_sana2_set_capabilities,
	.send_mlme = wpa_driver_sana2_send_mlme,
	.mlme_add_sta = wpa_driver_sana2_mlme_add_sta,
	.mlme_remove_sta = wpa_driver_sana2_mlme_remove_sta,
};
