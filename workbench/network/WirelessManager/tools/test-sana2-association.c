#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sana2-association-source-constants.inc"

/* Host fixture: these are the production fields read by associate/set_key.
 * Exec I/O, TagItem, and driver request structures are deliberately minimal;
 * the public numeric tags/commands and relevant enums come from repository
 * headers extracted by the runner. DoIO and the set_key wrapper below record
 * every request and argument while delegating to the extracted production
 * set_key implementation. This is not an Amiga ABI or hardware test. */
typedef uint32_t ULONG;
typedef uintptr_t UPINT;
typedef void *APTR;
typedef unsigned char u8;

enum wpa_alg {
	WPA_ALG_NONE = TEST_WPA_ALG_NONE,
	WPA_ALG_WEP = TEST_WPA_ALG_WEP
};

#define ETH_ALEN 6
#define NUM_WEP_KEYS 4
#define os_memset memset

struct TagItem {
	ULONG ti_Tag;
	UPINT ti_Data;
};

struct IOSana2Req {
	struct {
		ULONG io_Command;
		int io_Error;
	} ios2_Req;
	ULONG ios2_WireError;
	ULONG ios2_PacketType;
	u8 ios2_DstAddr[ETH_ALEN];
	ULONG ios2_DataLength;
	APTR ios2_Data;
	APTR ios2_StatData;
};

struct Sana2WirelessCapabilities {
	ULONG Flags;
};

struct wpa_driver_sana2_data {
	struct IOSana2Req *request;
	int wireless_caps_valid;
	struct Sana2WirelessCapabilities wireless_caps;
	int wep_key_attempted;
	char ssid[MAX_SSID_LEN + 1];
};

struct wpa_driver_associate_params {
	const u8 *ssid;
	size_t ssid_len;
	const u8 *wpa_ie;
	size_t wpa_ie_len;
	size_t wep_key_len[NUM_WEP_KEYS];
	const u8 *wep_key[NUM_WEP_KEYS];
	int wep_tx_keyidx;
	const u8 *bssid;
	int mode;
	int freq;
	int auth_alg;
	const char *passphrase;
	int key_mgmt_suite;
	const u8 *psk;
};

struct doio_record {
	ULONG command;
	int result;
	int io_error;
	ULONG wire_error;
	ULONG packet_type;
	ULONG data_length;
	UPINT data_pointer;
	UPINT statdata_pointer;
	u8 dst_addr[ETH_ALEN];
	UPINT options_pointer;
	ULONG option_tags[9];
	UPINT option_data[9];
	size_t option_count;
	char option_ssid[MAX_SSID_LEN + 1];
	int option_statdata_was_null;
	UPINT psk_pointer;
	u8 psk_copy[S2WIRELESS_PSK_SIZE];
	int psk_copied;
};

struct set_key_record {
	int alg;
	int key_idx;
	int set_tx;
	UPINT ifname_pointer;
	UPINT priv_pointer;
	UPINT addr_pointer;
	UPINT seq_pointer;
	size_t seq_len;
	UPINT key_pointer;
	size_t key_len;
	u8 addr[ETH_ALEN];
	u8 key_copy[64];
};

struct io_fixture {
	struct doio_record doio[8];
	size_t doio_count;
	struct set_key_record set_key[8];
	size_t set_key_count;
	int fault_index;
	int fault_command;
	int fault_command_all;
	int fault_return;
	int fault_io_error;
};

struct fixture {
	struct wpa_driver_sana2_data drv;
	struct IOSana2Req request;
	struct wpa_driver_associate_params params;
	u8 ssid[40];
};

static struct io_fixture io;
static int using_wep;
static unsigned int scenario_count;
static unsigned int check_count;
static unsigned int failure_count;

static const u8 broadcast_addr[ETH_ALEN] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static void CopyMem(const void *source, APTR destination, size_t size)
{
	memcpy(destination, source, size);
}

/* Cipher conversion is extracted verbatim from driver_sana2.c. */
#include "sana2-cipher.inc"

static int DoIO(APTR request_arg);
static int wpa_driver_sana2_set_key(const char *ifname, void *priv,
	enum wpa_alg alg, const u8 *addr, int key_idx, int set_tx,
	const u8 *seq, size_t seq_len, const u8 *key, size_t key_len);

/* The runner inserts this function verbatim from driver_sana2.c after the
 * name is remapped, so association uses production key handling. */
#define wpa_driver_sana2_set_key wpa_driver_sana2_set_key_actual
#include "sana2-set-key.inc"
#undef wpa_driver_sana2_set_key

static int wpa_driver_sana2_set_key(const char *ifname, void *priv,
	enum wpa_alg alg, const u8 *addr, int key_idx, int set_tx,
	const u8 *seq, size_t seq_len, const u8 *key, size_t key_len)
{
	struct set_key_record *record;

	if (io.set_key_count >= sizeof(io.set_key) / sizeof(io.set_key[0])) {
		fprintf(stderr, "FAIL: set_key recorder capacity exceeded\n");
		++failure_count;
		return -1;
	}
	record = &io.set_key[io.set_key_count++];
	memset(record, 0, sizeof(*record));
	record->alg = alg;
	record->key_idx = key_idx;
	record->set_tx = set_tx;
	record->ifname_pointer = (UPINT) ifname;
	record->priv_pointer = (UPINT) priv;
	record->addr_pointer = (UPINT) addr;
	record->seq_pointer = (UPINT) seq;
	record->seq_len = seq_len;
	record->key_pointer = (UPINT) key;
	record->key_len = key_len;
	if (addr != NULL)
		memcpy(record->addr, addr, ETH_ALEN);
	if (key != NULL && key_len <= sizeof(record->key_copy))
		memcpy(record->key_copy, key, key_len);
	return wpa_driver_sana2_set_key_actual(ifname, priv, alg, addr,
					       key_idx, set_tx, seq, seq_len,
					       key, key_len);
}

#include "sana2-associate.inc"

static int DoIO(APTR request_arg)
{
	struct IOSana2Req *request = request_arg;
	struct doio_record *record;
	size_t call_index, i;
	int should_fail;

	if (io.doio_count >= sizeof(io.doio) / sizeof(io.doio[0])) {
		fprintf(stderr, "FAIL: DoIO recorder capacity exceeded\n");
		++failure_count;
		return -1;
	}
	call_index = io.doio_count++;
	record = &io.doio[call_index];
	memset(record, 0, sizeof(*record));
	record->command = request->ios2_Req.io_Command;
	record->wire_error = request->ios2_WireError;
	record->packet_type = request->ios2_PacketType;
	record->data_length = request->ios2_DataLength;
	record->data_pointer = (UPINT) request->ios2_Data;
	record->statdata_pointer = (UPINT) request->ios2_StatData;
	memcpy(record->dst_addr, request->ios2_DstAddr, ETH_ALEN);
	if (record->command == S2_SETOPTIONS) {
		struct TagItem *items = request->ios2_Data;

		record->options_pointer = (UPINT) items;
		record->option_statdata_was_null = request->ios2_StatData == NULL;
		if (items == NULL) {
			fprintf(stderr, "FAIL: S2_SETOPTIONS has NULL option list\n");
			++failure_count;
		} else {
			for (i = 0; i < sizeof(record->option_tags) /
				     sizeof(record->option_tags[0]); ++i) {
				record->option_tags[i] = items[i].ti_Tag;
				record->option_data[i] = items[i].ti_Data;
				/* Copy raw PSK bytes only while the exact active tag is
				 * inside synchronous DoIO. TAG_IGNORE may retain the caller's
				 * pointer, but that does not authorize reading it. */
				if (items[i].ti_Tag == S2INFO_PSK &&
				    items[i].ti_Data != 0) {
					record->psk_pointer = items[i].ti_Data;
					memcpy(record->psk_copy,
					       (const void *) (uintptr_t) items[i].ti_Data,
					       S2WIRELESS_PSK_SIZE);
					record->psk_copied = 1;
				}
				if (items[i].ti_Tag == TAG_END) {
					record->option_count = i;
					break;
				}
				if (i + 1 == sizeof(record->option_tags) /
					    sizeof(record->option_tags[0])) {
					fprintf(stderr,
						"FAIL: S2_SETOPTIONS has no bounded TAG_END\n");
					++failure_count;
				}
			}
			for (i = 0; i < record->option_count; ++i) {
				if (record->option_tags[i] == S2INFO_SSID) {
					const char *ssid = (const char *)
						(uintptr_t) record->option_data[i];
					size_t ssid_length = strlen(ssid);
					if (ssid_length >= sizeof(record->option_ssid)) {
						fprintf(stderr, "FAIL: SETOPTIONS SSID exceeds recorder\n");
						++failure_count;
					} else {
						memcpy(record->option_ssid, ssid, ssid_length + 1);
					}
					break;
				}
			}
		}
	}

	should_fail = ((int) call_index == io.fault_index) ||
		(io.fault_command_all &&
		 record->command == (ULONG) io.fault_command);
	request->ios2_Req.io_Error = should_fail ? io.fault_io_error : 0;
	record->io_error = request->ios2_Req.io_Error;
	record->result = should_fail ? io.fault_return : 0;
	return record->result;
}

static void check_value(const char *scenario, const char *predicate,
			int actual, int expected)
{
	++check_count;
	if (actual != expected) {
		fprintf(stderr, "FAIL: %s: %s got %d, expected %d\n",
			scenario, predicate, actual, expected);
		++failure_count;
	}
}

static void check_string(const char *scenario, const char *predicate,
			 const char *actual, const char *expected)
{
	++check_count;
	if (strcmp(actual, expected) != 0) {
		fprintf(stderr, "FAIL: %s: %s got '%s', expected '%s'\n",
			scenario, predicate, actual, expected);
		++failure_count;
	}
}

static void check_bytes(const char *scenario, const char *predicate,
			const u8 *actual, const u8 *expected, size_t len)
{
	++check_count;
	if (memcmp(actual, expected, len) != 0) {
		fprintf(stderr, "FAIL: %s: %s bytes differ\n", scenario,
			predicate);
		++failure_count;
	}
}

#define CHECK(scenario, predicate, actual, expected) \
	check_value((scenario), (predicate), (int) (actual), (int) (expected))

#define SCENARIO() (++scenario_count)

static void reset_io(void)
{
	memset(&io, 0, sizeof(io));
	io.fault_index = -1;
	io.fault_command = -1;
}

static void init_fixture(struct fixture *f, const char *ssid)
{
	memset(f, 0, sizeof(*f));
	reset_io();
	f->drv.request = &f->request;
	memcpy(f->drv.ssid, "previous", sizeof("previous"));
	f->request.ios2_Data = (APTR) (uintptr_t) 0x1111;
	f->request.ios2_StatData = (APTR) (uintptr_t) 0x2222;
	memcpy(f->ssid, ssid, strlen(ssid));
	f->params.ssid = f->ssid;
	f->params.ssid_len = strlen(ssid);
	f->params.mode = IEEE80211_MODE_INFRA;
	f->params.freq = 2412;
	f->params.auth_alg = 3;
	f->params.wep_tx_keyidx = -1;
}

static int option_index(const struct doio_record *record, ULONG tag)
{
	size_t i;

	for (i = 0; i < record->option_count; ++i) {
		if (record->option_tags[i] == tag)
			return (int) i;
	}
	return -1;
}

static void check_rejected_without_io(const char *scenario, int result,
				      const struct fixture *f)
{
	CHECK(scenario, "returns -1", result, -1);
	CHECK(scenario, "no DoIO calls", io.doio_count, 0);
	CHECK(scenario, "no set_key calls", io.set_key_count, 0);
	check_string(scenario, "SSID unchanged", f->drv.ssid, "previous");
}

static void test_null_and_ssid_validation(void)
{
	struct fixture f;
	int result;

	init_fixture(&f, "alpha");
	SCENARIO();
	result = wpa_driver_sana2_associate(NULL, &f.params);
	check_rejected_without_io("null driver", result, &f);

	init_fixture(&f, "alpha");
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, NULL);
	check_rejected_without_io("null params", result, &f);

	init_fixture(&f, "alpha");
	f.drv.request = NULL;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("null request", result, &f);

	init_fixture(&f, "alpha");
	f.params.ssid = NULL;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("null SSID", result, &f);

	init_fixture(&f, "alpha");
	f.params.ssid_len = 0;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("zero SSID length", result, &f);

	init_fixture(&f, "alpha");
	f.params.ssid_len = MAX_SSID_LEN + 1;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("SSID length 33", result, &f);

	init_fixture(&f, "abcd");
	f.ssid[2] = '\0';
	f.params.ssid_len = 4;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("embedded NUL in SSID", result, &f);

	init_fixture(&f, "alpha");
	f.params.wpa_ie = NULL;
	f.params.wpa_ie_len = 1;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("NULL WPA IE with nonzero length", result, &f);

	init_fixture(&f, "alpha");
	f.params.wep_key_len[2] = 5;
	f.params.wep_key[2] = NULL;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("NULL WEP key with nonzero length", result, &f);
}

static void test_ssid_32_and_borrowed_options(void)
{
	struct fixture f;
	struct doio_record *options;
	size_t i;
	int result;
	char expected[MAX_SSID_LEN + 1];

	init_fixture(&f, "");
	for (i = 0; i < MAX_SSID_LEN; ++i)
		f.ssid[i] = (u8) ('A' + (i % 26));
	f.ssid[MAX_SSID_LEN] = '\0';
	f.params.ssid = f.ssid;
	f.params.ssid_len = MAX_SSID_LEN;
	memcpy(expected, f.ssid, sizeof(expected));
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	CHECK("SSID length 32", "association succeeds", result, 0);
	CHECK("SSID length 32", "four key-clear calls", io.set_key_count, 4);
	CHECK("SSID length 32", "five total DoIO calls", io.doio_count, 5);
	check_string("SSID length 32", "stored SSID", f.drv.ssid, expected);
	options = &io.doio[io.doio_count - 1];
	check_string("SSID length 32", "DoIO reads copied SSID", options->option_ssid,
		     expected);
	CHECK("SSID length 32", "SSID option not borrowed from caller",
	      options->option_data[0] != (UPINT) f.params.ssid, 1);
	CHECK("SSID length 32", "request Data cleared after return",
	      f.request.ios2_Data == NULL, 1);
	CHECK("SSID length 32", "request StatData cleared after return",
	      f.request.ios2_StatData == NULL, 1);
	CHECK("SSID length 32", "SSID tag list observed during DoIO",
	      options->options_pointer != 0, 1);
	CHECK("SSID length 32", "StatData cleared before DoIO",
	      options->option_statdata_was_null, 1);
}

static void test_legacy_open_and_wep_host_wpa(void)
{
	struct fixture f;
	struct doio_record *options;
	static const u8 wep_key[] = { 1, 2, 3, 4, 5 };
	static const u8 wpa_ie[] = { 0x30, 0x02, 0xaa, 0xbb };
	int i, result;

	init_fixture(&f, "legacy-open");
	f.params.wep_tx_keyidx = 2;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	CHECK("legacy open", "association succeeds", result, 0);
	CHECK("legacy open", "four key-clear calls", io.set_key_count, 4);
	CHECK("legacy open", "five total DoIO calls", io.doio_count, 5);
	for (i = 0; i < 4; ++i) {
		char predicate[64];
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d uses NONE", i);
		CHECK("legacy open", predicate, io.set_key[i].alg, WPA_ALG_NONE);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d key index", i);
		CHECK("legacy open", predicate, io.set_key[i].key_idx, i);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d TX flag", i);
		CHECK("legacy open", predicate, io.set_key[i].set_tx, i == 2);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d has no key", i);
		CHECK("legacy open", predicate, io.set_key[i].key_pointer == 0, 1);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d has zero length", i);
		CHECK("legacy open", predicate, io.set_key[i].key_len, 0);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d has no sequence", i);
		CHECK("legacy open", predicate,
		      io.set_key[i].seq_pointer == 0 && io.set_key[i].seq_len == 0, 1);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d uses broadcast address", i);
		check_bytes("legacy open", predicate, io.set_key[i].addr,
			    broadcast_addr, ETH_ALEN);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d broadcast pointer", i);
		CHECK("legacy open", predicate,
		      io.set_key[i].addr_pointer == (UPINT) broadcast_addr, 1);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d private context", i);
		CHECK("legacy open", predicate,
		      io.set_key[i].priv_pointer == (UPINT) &f.drv, 1);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d interface name", i);
		CHECK("legacy open", predicate, io.set_key[i].ifname_pointer, 0);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d issues S2_SETKEY", i);
		CHECK("legacy open", predicate, io.doio[i].command, S2_SETKEY);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d packet type", i);
		CHECK("legacy open", predicate, io.doio[i].packet_type, S2ENC_NONE);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d request data", i);
		CHECK("legacy open", predicate, io.doio[i].data_pointer, 0);
		(void) snprintf(predicate, sizeof(predicate), "clear slot %d request statdata", i);
		CHECK("legacy open", predicate, io.doio[i].statdata_pointer, 0);
	}
	options = &io.doio[4];
	CHECK("legacy open", "options command", options->command, S2_SETOPTIONS);
	CHECK("legacy open", "options tags are exact",
	      options->option_tags[0] == S2INFO_SSID &&
	      options->option_tags[1] == TAG_IGNORE &&
	      options->option_tags[2] == S2INFO_PortType &&
	      options->option_tags[3] == S2INFO_Channel &&
	      options->option_tags[4] == S2INFO_WPAInfo &&
	      options->option_tags[5] == S2INFO_AuthTypes &&
	      options->option_tags[6] == TAG_IGNORE &&
	      options->option_tags[7] == TAG_IGNORE &&
	      options->option_tags[8] == TAG_END, 1);
	CHECK("legacy open", "passphrase omitted", options->option_tags[6], TAG_IGNORE);
	CHECK("legacy open", "SSID text", strcmp(options->option_ssid, "legacy-open") == 0, 1);
	CHECK("legacy open", "managed port option",
	      options->option_data[2], S2PORT_MANAGED);
	CHECK("legacy open", "frequency maps to channel 1",
	      options->option_data[3], 1);
	CHECK("legacy open", "NULL WPA IE maps to NULL option",
	      options->option_data[4], 0);
	CHECK("legacy open", "auth option exact", options->option_data[5], 3);
	CHECK("legacy open", "StatData cleared before DoIO",
	      options->option_statdata_was_null, 1);
	CHECK("legacy open", "WEP history remains clear", f.drv.wep_key_attempted, 0);
	CHECK("legacy open", "request Data cleared after return",
	      f.request.ios2_Data == NULL, 1);
	CHECK("legacy open", "request StatData cleared after return",
	      f.request.ios2_StatData == NULL, 1);
	check_string("legacy open", "stored SSID", f.drv.ssid, "legacy-open");

	init_fixture(&f, "legacy-host-wpa");
	f.params.wep_key_len[1] = sizeof(wep_key);
	f.params.wep_key[1] = wep_key;
	f.params.wep_tx_keyidx = 1;
	f.params.wpa_ie = wpa_ie;
	f.params.wpa_ie_len = sizeof(wpa_ie);
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	CHECK("legacy WEP + host WPA", "association succeeds", result, 0);
	CHECK("legacy WEP + host WPA", "four key calls", io.set_key_count, 4);
	CHECK("legacy WEP + host WPA", "five DoIO calls", io.doio_count, 5);
	CHECK("legacy WEP + host WPA", "WEP slot algorithm",
	      io.set_key[1].alg, WPA_ALG_WEP);
	CHECK("legacy WEP + host WPA", "WEP slot length",
	      io.set_key[1].key_len, sizeof(wep_key));
	CHECK("legacy WEP + host WPA", "WEP slot pointer exact",
	      io.set_key[1].key_pointer == (UPINT) wep_key, 1);
	check_bytes("legacy WEP + host WPA", "WEP key passed intact",
		    io.set_key[1].key_copy, wep_key, sizeof(wep_key));
	CHECK("legacy WEP + host WPA", "WEP packet type",
	      io.doio[1].packet_type, S2ENC_WEP);
	CHECK("legacy WEP + host WPA", "WEP attempt history retained",
	      f.drv.wep_key_attempted, 1);
	options = &io.doio[4];
	{
		int wpa_ie_index = option_index(options, S2INFO_WPAInfo);
	CHECK("legacy WEP + host WPA", "WPA IE tag present",
	      wpa_ie_index >= 0, 1);
	CHECK("legacy WEP + host WPA", "WPA IE pointer exact",
	      wpa_ie_index >= 0 &&
	      options->option_data[wpa_ie_index] == (UPINT) wpa_ie, 1);
	}
	CHECK("legacy WEP + host WPA", "passphrase omitted for host handshake",
	      options->option_tags[6], TAG_IGNORE);
	CHECK("legacy WEP + host WPA", "raw PSK omitted for host handshake",
	      options->option_tags[7], TAG_IGNORE);
	CHECK("legacy WEP + host WPA", "StatData cleared before DoIO",
	      options->option_statdata_was_null, 1);
	check_string("legacy WEP + host WPA", "stored SSID", f.drv.ssid,
		     "legacy-host-wpa");
}

static void enable_offload_psk(struct fixture *f, const char *passphrase)
{
	f->drv.wireless_caps_valid = 1;
	f->drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY;
	f->params.key_mgmt_suite = KEY_MGMT_PSK;
	f->params.mode = IEEE80211_MODE_INFRA;
	f->params.passphrase = passphrase;
}

static void enable_offload_raw_psk(struct fixture *f, const u8 *psk)
{
	f->drv.wireless_caps_valid = 1;
	f->drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_RAW_PSK;
	f->params.key_mgmt_suite = KEY_MGMT_PSK;
	f->params.mode = IEEE80211_MODE_INFRA;
	f->params.psk = psk;
}

static u8 *new_binary_psk(u8 expected[S2WIRELESS_PSK_SIZE],
			  UPINT *address)
{
	u8 *psk = malloc(S2WIRELESS_PSK_SIZE);
	size_t i;

	if (psk == NULL) {
		fprintf(stderr, "FAIL: could not allocate binary PSK fixture\n");
		++failure_count;
		return NULL;
	}
	for (i = 0; i < S2WIRELESS_PSK_SIZE; ++i)
		psk[i] = (u8) (0x31 + (i * 7));
	psk[0] = 0;
	psk[7] = 0;
	psk[19] = 0xff;
	psk[S2WIRELESS_PSK_SIZE - 1] = 0;
	memcpy(expected, psk, S2WIRELESS_PSK_SIZE);
	*address = (UPINT) psk;
	return psk;
}

static void test_credential_refusals_and_offload_wep(void)
{
	struct fixture f;
	static const u8 raw_psk[32] = { 0x42 };
	static const u8 wep_key[5] = { 1, 2, 3, 4, 5 };
	int result;

	init_fixture(&f, "credential");
	f.params.passphrase = "password";
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("credential without four-way offload", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY;
	f.drv.wireless_caps_valid = 0;
	f.params.passphrase = "password";
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("four-way flag ignored without valid capabilities",
				  result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps_valid = 1;
	f.drv.wireless_caps.Flags = 0;
	f.params.passphrase = "password";
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("valid capabilities without four-way flag",
				  result, &f);

	init_fixture(&f, "credential");
	f.params.psk = raw_psk;
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("raw PSK without four-way offload", result, &f);

	init_fixture(&f, "credential");
	enable_offload_psk(&f, "password");
	f.params.mode = IEEE80211_MODE_IBSS;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("offload credential outside infrastructure", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps_valid = 1;
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY;
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	f.params.psk = raw_psk;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("raw-only PSK without RAW_PSK capability", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_RAW_PSK;
	f.drv.wireless_caps_valid = 0;
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	f.params.psk = raw_psk;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("raw PSK with invalid capability response", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps_valid = 1;
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_RAW_PSK;
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	f.params.psk = raw_psk;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("RAW_PSK without FOURWAY", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps_valid = 1;
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_RAW_PSK;
	f.params.key_mgmt_suite = 0;
	f.params.psk = raw_psk;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("raw PSK with invalid key management", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps_valid = 1;
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_RAW_PSK;
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	f.params.mode = IEEE80211_MODE_IBSS;
	f.params.psk = raw_psk;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("raw PSK outside infrastructure mode", result, &f);

	init_fixture(&f, "credential");
	f.drv.wireless_caps_valid = 1;
	f.drv.wireless_caps.Flags = S2WIRELESS_CAPF_FOURWAY |
		S2WIRELESS_CAPF_RAW_PSK;
	f.params.key_mgmt_suite = KEY_MGMT_PSK;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("missing PSK credential with offload", result, &f);

	init_fixture(&f, "credential");
	enable_offload_psk(&f, "password");
	f.params.wep_key_len[0] = sizeof(wep_key);
	f.params.wep_key[0] = wep_key;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("four-way offload refuses WEP keys", result, &f);
}

static void test_passphrase_lengths_and_tags(void)
{
	struct fixture f;
	char passphrase[65];
	int length, result;
	struct doio_record *options;

	for (length = 0; length <= 7; ++length) {
		char scenario[64];
		memset(passphrase, 'a', (size_t) length);
		passphrase[length] = '\0';
		init_fixture(&f, "offload");
		enable_offload_psk(&f, passphrase);
		SCENARIO();
		result = wpa_driver_sana2_associate(&f.drv, &f.params);
		(void) snprintf(scenario, sizeof(scenario),
				"offload passphrase length %d", length);
		check_rejected_without_io(scenario, result, &f);
	}

	memset(passphrase, 'a', 64);
	passphrase[64] = '\0';
	init_fixture(&f, "offload");
	enable_offload_psk(&f, passphrase);
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("offload passphrase length 64", result, &f);

	memset(passphrase, 'a', 8);
	passphrase[3] = (char) 0xe9;
	passphrase[8] = '\0';
	init_fixture(&f, "offload");
	enable_offload_psk(&f, passphrase);
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	check_rejected_without_io("offload non-ASCII passphrase", result, &f);

	memcpy(passphrase, "abcdefgh", 9);
	init_fixture(&f, "offload-8");
	enable_offload_psk(&f, passphrase);
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	CHECK("offload passphrase length 8", "association succeeds", result, 0);
	CHECK("offload passphrase length 8", "no S2_SETKEY calls", io.set_key_count, 0);
	CHECK("offload passphrase length 8", "one SETOPTIONS call", io.doio_count, 1);
	options = &io.doio[0];
	CHECK("offload passphrase length 8", "exact passphrase tag",
	      options->option_tags[6], S2INFO_Passphrase);
	CHECK("offload passphrase length 8", "exact passphrase pointer",
	      options->option_data[6] == (UPINT) f.params.passphrase, 1);
	CHECK("offload passphrase length 8", "raw PSK tag omitted",
	      options->option_tags[7], TAG_IGNORE);
	CHECK("offload passphrase length 8", "request Data cleared",
	      f.request.ios2_Data == NULL, 1);
	CHECK("offload passphrase length 8", "request StatData cleared",
	      f.request.ios2_StatData == NULL, 1);
	check_string("offload passphrase length 8", "stored SSID", f.drv.ssid,
		     "offload-8");

	memset(passphrase, 'z', 63);
	passphrase[63] = '\0';
	init_fixture(&f, "offload-63");
	enable_offload_psk(&f, passphrase);
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	CHECK("offload passphrase length 63", "association succeeds", result, 0);
	CHECK("offload passphrase length 63", "no S2_SETKEY calls", io.set_key_count, 0);
	CHECK("offload passphrase length 63", "one SETOPTIONS call", io.doio_count, 1);
	options = &io.doio[0];
	CHECK("offload passphrase length 63", "exact passphrase tag",
	      options->option_tags[6], S2INFO_Passphrase);
	CHECK("offload passphrase length 63", "exact passphrase pointer",
	      options->option_data[6] == (UPINT) f.params.passphrase, 1);
	CHECK("offload passphrase length 63", "raw PSK tag omitted",
	      options->option_tags[7], TAG_IGNORE);
	check_string("offload passphrase length 63", "stored SSID", f.drv.ssid,
		     "offload-63");
}

static void test_raw_psk_forwarding_and_lifetime(void)
{
	struct fixture f;
	struct doio_record *options;
	u8 expected[S2WIRELESS_PSK_SIZE];
	UPINT psk_address;
	u8 *psk;
	int kind, result;
	static const char passphrase[] = "both-credentials";

	psk = new_binary_psk(expected, &psk_address);
	if (psk == NULL)
		return;
	init_fixture(&f, "raw-psk-success");
	enable_offload_raw_psk(&f, psk);
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	/* Free immediately after the synchronous reply. Remaining assertions use
	 * only the saved address and bytes copied by the DoIO callback. */
	free(psk);
	CHECK("raw PSK success", "association succeeds", result, 0);
	CHECK("raw PSK success", "fixed credential size is 32 bytes",
	      S2WIRELESS_PSK_SIZE, 32);
	CHECK("raw PSK success", "offload skips WEP key calls", io.set_key_count, 0);
	CHECK("raw PSK success", "one SETOPTIONS call", io.doio_count, 1);
	options = &io.doio[0];
	CHECK("raw PSK success", "passphrase tag omitted",
	      options->option_tags[6], TAG_IGNORE);
	CHECK("raw PSK success", "raw PSK tag exact",
	      options->option_tags[7], S2INFO_PSK);
	CHECK("raw PSK success", "tag terminator follows PSK",
	      options->option_tags[8], TAG_END);
	CHECK("raw PSK success", "borrowed pointer exact",
	      options->option_data[7] == psk_address, 1);
	CHECK("raw PSK success", "DoIO copied exactly the PSK tag bytes",
	      options->psk_copied, 1);
	CHECK("raw PSK success", "DoIO recorded borrowed pointer",
	      options->psk_pointer == psk_address, 1);
	check_bytes("raw PSK success", "all 32 binary bytes preserved",
		    options->psk_copy, expected, S2WIRELESS_PSK_SIZE);
	CHECK("raw PSK success", "embedded NUL preserved",
	      options->psk_copy[0], 0);
	CHECK("raw PSK success", "0xff byte preserved",
	      options->psk_copy[19], 0xff);
	CHECK("raw PSK success", "request Data cleared after reply",
	      f.request.ios2_Data == NULL, 1);
	CHECK("raw PSK success", "request StatData cleared after reply",
	      f.request.ios2_StatData == NULL, 1);
	CHECK("raw PSK success", "StatData cleared before DoIO",
	      options->option_statdata_was_null, 1);
	check_string("raw PSK success", "SSID updated", f.drv.ssid,
		     "raw-psk-success");

	for (kind = 0; kind < 2; ++kind) {
		char scenario[64];
		psk = new_binary_psk(expected, &psk_address);
		if (psk == NULL)
			return;
		init_fixture(&f, "raw-psk-failure");
		enable_offload_raw_psk(&f, psk);
		io.fault_index = 0;
		io.fault_return = kind == 0 ? 9 : 0;
		io.fault_io_error = kind == 0 ? 0 : 7;
		SCENARIO();
		result = wpa_driver_sana2_associate(&f.drv, &f.params);
		free(psk);
		(void) snprintf(scenario, sizeof(scenario),
				"raw PSK SETOPTIONS %s failure",
				kind == 0 ? "DoIO return" : "io_Error");
		CHECK(scenario, "association fails", result, -1);
		CHECK(scenario, "DoIO inspected active PSK before reply",
		      io.doio_count, 1);
		CHECK(scenario, "PSK bytes copied during failing DoIO",
		      io.doio[0].psk_copied, 1);
		CHECK(scenario, "borrowed pointer matches caller allocation",
		      io.doio[0].psk_pointer == psk_address, 1);
		check_bytes(scenario, "all 32 binary bytes preserved before error",
			    io.doio[0].psk_copy, expected,
			    S2WIRELESS_PSK_SIZE);
		CHECK(scenario, "request Data cleared after reply",
		      f.request.ios2_Data == NULL, 1);
		CHECK(scenario, "request StatData cleared after reply",
		      f.request.ios2_StatData == NULL, 1);
		check_string(scenario, "SSID unchanged", f.drv.ssid, "previous");
	}

	psk = new_binary_psk(expected, &psk_address);
	if (psk == NULL)
		return;
	init_fixture(&f, "prefer-passphrase");
	enable_offload_psk(&f, passphrase);
	f.params.psk = psk;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	free(psk);
	CHECK("passphrase preferred when both are supplied", "association succeeds",
	      result, 0);
	CHECK("passphrase preferred when both are supplied", "passphrase tag exact",
	      io.doio[0].option_tags[6], S2INFO_Passphrase);
	CHECK("passphrase preferred when both are supplied", "passphrase pointer exact",
	      io.doio[0].option_data[6] == (UPINT) passphrase, 1);
	CHECK("passphrase preferred when both are supplied", "raw tag omitted",
	      io.doio[0].option_tags[7], TAG_IGNORE);
	CHECK("passphrase preferred when both are supplied",
	      "ignored option may retain pointer without reading it",
	      io.doio[0].option_data[7] == psk_address, 1);
	CHECK("passphrase preferred when both are supplied",
	      "recorder does not copy ignored raw PSK", io.doio[0].psk_copied, 0);
	CHECK("passphrase preferred when both are supplied",
	      "recorder has no active raw PSK pointer", io.doio[0].psk_pointer, 0);
}

static void test_setkey_faults_and_option_faults(void)
{
	struct fixture f;
	int slot, kind, result;

	for (kind = 0; kind < 2; ++kind) {
		for (slot = 0; slot < NUM_WEP_KEYS; ++slot) {
			char scenario[96];
			init_fixture(&f, "preserve");
			io.fault_index = slot;
			io.fault_return = kind == 0 ? 7 : 0;
			io.fault_io_error = kind == 0 ? 0 : 5;
			SCENARIO();
			result = wpa_driver_sana2_associate(&f.drv, &f.params);
			(void) snprintf(scenario, sizeof(scenario),
					"S2_SETKEY slot %d %s failure", slot,
					kind == 0 ? "DoIO return" : "io_Error");
			CHECK(scenario, "association fails", result, -1);
			CHECK(scenario, "stops at failed key slot", io.set_key_count,
			      slot + 1);
			CHECK(scenario, "no SETOPTIONS after key failure",
			      io.doio_count, slot + 1);
			CHECK(scenario, "failed command is S2_SETKEY",
			      io.doio[slot].command, S2_SETKEY);
			CHECK(scenario, "request Data cleared",
			      f.request.ios2_Data == NULL, 1);
			CHECK(scenario, "request StatData cleared",
			      f.request.ios2_StatData == NULL, 1);
			check_string(scenario, "SSID unchanged", f.drv.ssid, "previous");
		}
	}

	for (kind = 0; kind < 2; ++kind) {
		char scenario[64];
		init_fixture(&f, "preserve");
		io.fault_index = 4;
		io.fault_return = kind == 0 ? 7 : 0;
		io.fault_io_error = kind == 0 ? 0 : 5;
		SCENARIO();
		result = wpa_driver_sana2_associate(&f.drv, &f.params);
		(void) snprintf(scenario, sizeof(scenario),
				"S2_SETOPTIONS %s failure",
				kind == 0 ? "DoIO return" : "io_Error");
		CHECK(scenario, "association fails", result, -1);
		CHECK(scenario, "four legacy clears completed", io.set_key_count, 4);
		CHECK(scenario, "four clears then SETOPTIONS", io.doio_count, 5);
		CHECK(scenario, "failed command is SETOPTIONS",
		      io.doio[4].command, S2_SETOPTIONS);
		CHECK(scenario, "request Data cleared",
		      f.request.ios2_Data == NULL, 1);
		CHECK(scenario, "request StatData cleared",
		      f.request.ios2_StatData == NULL, 1);
		check_string(scenario, "SSID unchanged", f.drv.ssid, "previous");
	}
}

static void test_actual_cipher_helper(void)
{
	SCENARIO();
	CHECK("actual cipher helper", "none", wpa_driver_sana2_cipher(CIPHER_NONE),
	      S2ENC_NONE);
	CHECK("actual cipher helper", "WEP40", wpa_driver_sana2_cipher(CIPHER_WEP40),
	      S2ENC_WEP);
	CHECK("actual cipher helper", "WEP104",
	      wpa_driver_sana2_cipher(CIPHER_WEP104), S2ENC_WEP);
	CHECK("actual cipher helper", "TKIP", wpa_driver_sana2_cipher(CIPHER_TKIP),
	      S2ENC_TKIP);
	CHECK("actual cipher helper", "CCMP", wpa_driver_sana2_cipher(CIPHER_CCMP),
	      S2ENC_CCMP);
	CHECK("actual cipher helper", "unknown maps to zero",
	      wpa_driver_sana2_cipher(99), 0);
}

static void test_unsupported_empty_clear_contract(void)
{
	struct fixture f;
	static const int unsupported[] = { S2ERR_NOT_SUPPORTED, IOERR_NOCMD };
	size_t i;
	int result;

	for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
		char scenario[80];
		init_fixture(&f, "clear-unsupported");
		io.fault_command = S2_SETKEY;
		io.fault_command_all = 1;
		io.fault_io_error = unsupported[i];
		SCENARIO();
		result = wpa_driver_sana2_associate(&f.drv, &f.params);
		(void) snprintf(scenario, sizeof(scenario),
				"unsupported empty clears, no history, io_Error %d",
				unsupported[i]);
		CHECK(scenario, "association succeeds", result, 0);
		CHECK(scenario, "attempted all four empty slots", io.set_key_count, 4);
		CHECK(scenario, "SETOPTIONS follows empty clears", io.doio_count, 5);
		CHECK(scenario, "history remains clear", f.drv.wep_key_attempted, 0);
		check_string(scenario, "SSID updated", f.drv.ssid, "clear-unsupported");
	}

	init_fixture(&f, "old-wep-history");
	f.drv.wep_key_attempted = 1;
	io.fault_command = S2_SETKEY;
	io.fault_command_all = 1;
	io.fault_io_error = S2ERR_NOT_SUPPORTED;
	SCENARIO();
	result = wpa_driver_sana2_associate(&f.drv, &f.params);
	CHECK("unsupported empty clear with prior WEP history", "association fails",
	      result, -1);
	CHECK("unsupported empty clear with prior WEP history", "stops immediately",
	      io.set_key_count, 1);
	CHECK("unsupported empty clear with prior WEP history", "no SETOPTIONS",
	      io.doio_count, 1);
	CHECK("unsupported empty clear with prior WEP history", "history preserved",
	      f.drv.wep_key_attempted, 1);
	check_string("unsupported empty clear with prior WEP history", "SSID unchanged",
		     f.drv.ssid, "previous");

	{
		static const u8 wep_key[] = { 1, 2, 3, 4, 5 };
		init_fixture(&f, "current-wep-attempt");
		f.params.wep_key_len[0] = sizeof(wep_key);
		f.params.wep_key[0] = wep_key;
		io.fault_index = 0;
		io.fault_io_error = IOERR_NOCMD;
		SCENARIO();
		result = wpa_driver_sana2_associate(&f.drv, &f.params);
		CHECK("unsupported current WEP install", "association fails", result, -1);
		CHECK("unsupported current WEP install", "attempted WEP is remembered",
		      f.drv.wep_key_attempted, 1);
		CHECK("unsupported current WEP install", "no SETOPTIONS", io.doio_count, 1);
		check_string("unsupported current WEP install", "SSID unchanged",
			     f.drv.ssid, "previous");
	}

	{
		static const u8 wep_key[] = { 1, 2, 3, 4, 5 };
		init_fixture(&f, "slot-two-wep");
		f.params.wep_key_len[2] = sizeof(wep_key);
		f.params.wep_key[2] = wep_key;
		io.fault_index = 0;
		io.fault_io_error = S2ERR_NOT_SUPPORTED;
		SCENARIO();
		result = wpa_driver_sana2_associate(&f.drv, &f.params);
		CHECK("WEP slot 2 after unsupported slot 0 clear", "association fails",
		      result, -1);
		CHECK("WEP slot 2 after unsupported slot 0 clear",
		      "stops before WEP install", io.set_key_count, 1);
		CHECK("WEP slot 2 after unsupported slot 0 clear",
		      "only an empty clear was attempted", io.set_key[0].alg,
		      WPA_ALG_NONE);
		CHECK("WEP slot 2 after unsupported slot 0 clear", "no SETOPTIONS",
		      io.doio_count, 1);
		check_string("WEP slot 2 after unsupported slot 0 clear",
			     "SSID unchanged", f.drv.ssid, "previous");
	}
}

static void test_direct_set_key_borrowed_buffer_cleanup(void)
{
	struct fixture f;
	static const u8 peer_addr[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const u8 sequence[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	static const u8 key[] = { 9, 8, 7, 6, 5 };
	int result;

	init_fixture(&f, "direct-set-key");
	using_wep = 0;
	SCENARIO();
	result = wpa_driver_sana2_set_key_actual(NULL, &f.drv, WPA_ALG_WEP,
						 peer_addr, 2, 1, sequence,
						 sizeof(sequence), key, sizeof(key));
	CHECK("direct set_key success", "returns success", result, 0);
	CHECK("direct set_key success", "one DoIO call", io.doio_count, 1);
	CHECK("direct set_key success", "command", io.doio[0].command, S2_SETKEY);
	CHECK("direct set_key success", "key index", io.doio[0].wire_error, 2);
	CHECK("direct set_key success", "cipher", io.doio[0].packet_type, S2ENC_WEP);
	CHECK("direct set_key success", "key size", io.doio[0].data_length,
	      sizeof(key));
	CHECK("direct set_key success", "key pointer exact",
	      io.doio[0].data_pointer == (UPINT) key, 1);
	CHECK("direct set_key success", "sequence pointer exact",
	      io.doio[0].statdata_pointer == (UPINT) sequence, 1);
	check_bytes("direct set_key success", "peer address", io.doio[0].dst_addr,
		    peer_addr, ETH_ALEN);
	CHECK("direct set_key success", "request Data cleared",
	      f.request.ios2_Data == NULL, 1);
	CHECK("direct set_key success", "request StatData cleared",
	      f.request.ios2_StatData == NULL, 1);
	CHECK("direct set_key success", "WEP history set",
	      f.drv.wep_key_attempted, 1);
	CHECK("direct set_key success", "WEP workaround flag set", using_wep, 1);

	init_fixture(&f, "direct-set-key-return-error");
	io.fault_index = 0;
	io.fault_return = 7;
	SCENARIO();
	result = wpa_driver_sana2_set_key_actual(NULL, &f.drv, WPA_ALG_WEP,
						 peer_addr, 1, 0, sequence,
						 sizeof(sequence), key, sizeof(key));
	CHECK("direct set_key DoIO failure", "returns failure", result, -1);
	CHECK("direct set_key DoIO failure", "request Data cleared",
	      f.request.ios2_Data == NULL, 1);
	CHECK("direct set_key DoIO failure", "request StatData cleared",
	      f.request.ios2_StatData == NULL, 1);
	CHECK("direct set_key DoIO failure", "recorded borrowed key pointer",
	      io.doio[0].data_pointer == (UPINT) key, 1);
	CHECK("direct set_key DoIO failure", "recorded borrowed sequence pointer",
	      io.doio[0].statdata_pointer == (UPINT) sequence, 1);
	CHECK("direct set_key DoIO failure", "WEP attempt remembered on failure",
	      f.drv.wep_key_attempted, 1);

	init_fixture(&f, "direct-set-key-io-error");
	io.fault_index = 0;
	io.fault_io_error = 6;
	SCENARIO();
	result = wpa_driver_sana2_set_key_actual(NULL, &f.drv, WPA_ALG_WEP,
						 peer_addr, 0, 0, sequence,
						 sizeof(sequence), key, sizeof(key));
	CHECK("direct set_key io_Error", "returns failure", result, -1);
	CHECK("direct set_key io_Error", "request Data cleared",
	      f.request.ios2_Data == NULL, 1);
	CHECK("direct set_key io_Error", "request StatData cleared",
	      f.request.ios2_StatData == NULL, 1);
	CHECK("direct set_key io_Error", "recorded borrowed key pointer",
	      io.doio[0].data_pointer == (UPINT) key, 1);
	CHECK("direct set_key io_Error", "recorded borrowed sequence pointer",
	      io.doio[0].statdata_pointer == (UPINT) sequence, 1);
	CHECK("direct set_key io_Error", "WEP attempt remembered on failure",
	      f.drv.wep_key_attempted, 1);
}

int main(void)
{
	test_null_and_ssid_validation();
	test_ssid_32_and_borrowed_options();
	test_legacy_open_and_wep_host_wpa();
	test_credential_refusals_and_offload_wep();
	test_actual_cipher_helper();
	test_passphrase_lengths_and_tags();
	test_raw_psk_forwarding_and_lifetime();
	test_setkey_faults_and_option_faults();
	test_unsupported_empty_clear_contract();
	test_direct_set_key_borrowed_buffer_cleanup();
	if (failure_count != 0) {
		fprintf(stderr, "FAIL: %u/%u checks failed across %u scenarios\n",
			failure_count, check_count, scenario_count);
		return 1;
	}
	printf("PASS: %u scenarios, %u checks\n", scenario_count, check_count);
	return 0;
}
