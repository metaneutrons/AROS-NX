#!/usr/bin/env python3
"""Host regression tests for WiFi's volatile preference contract.

This compiles the production volatile_config.c against minimal DOS mocks and
extracts the worker's preference and rollback helpers against DOS mocks. It
needs Clang with AddressSanitizer and UndefinedBehaviorSanitizer support.
Leak detection is disabled for macOS; the mocks explicitly balance allocations.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


WORKBENCH = Path(__file__).resolve().parents[3]
WIFI = WORKBENCH / "prefs" / "wifi"

HARNESS = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volatile_config.h"
#include <dos/dos.h>

static unsigned checks;
static unsigned open_calls, puts_calls, close_calls;
static int open_fails, puts_fails, close_fails;
static char opened_path[128];
static LONG opened_mode;
static char puts_text[512];

static void check_at(int condition, const char *description, int line)
{
    ++checks;
    if (!condition)
    {
        fprintf(stderr, "FAIL at line %d: %s\n", line, description);
        exit(1);
    }
}

#define CHECK(x) check_at((x), #x, __LINE__)

BPTR Open(CONST_STRPTR path, LONG mode)
{
    ++open_calls;
    snprintf(opened_path, sizeof(opened_path), "%s", path);
    opened_mode = mode;
    return open_fails ? BNULL : (BPTR)1;
}

LONG FPuts(BPTR file, CONST_STRPTR text)
{
    (void)file;
    ++puts_calls;
    snprintf(puts_text, sizeof(puts_text), "%s", text);
    return puts_fails ? -1 : 0;
}

BOOL Close(BPTR file)
{
    (void)file;
    ++close_calls;
    return close_fails ? FALSE : TRUE;
}

static void reset_mocks(void)
{
    open_calls = puts_calls = close_calls = 0;
    open_fails = puts_fails = close_fails = 0;
    opened_path[0] = '\0';
    opened_mode = -1;
    puts_text[0] = '\0';
}

static void test_formatting(void)
{
    char output[256];
    char ssid_special[] = {'a', '"', '\\', '\n', (char)0xc3,
                           (char)0xa9, '\0'};
    char expected_max[160] = "ap_scan=1\nnetwork={\n\tssid=";
    char ssid_max[33];
    char ssid_long[34];
    char key63[64];
    char key64[65];
    char bad_key[9] = "abcdefgh";
    size_t i;

    CHECK(WifiFormatVolatilePrefs(output, sizeof(output), "MyWiFi", NULL,
                                  FALSE));
    CHECK(strcmp(output,
        "ap_scan=1\nnetwork={\n\tssid=4d7957694669\n"
        "\tkey_mgmt=NONE\n}\n") == 0);

    CHECK(WifiFormatVolatilePrefs(output, sizeof(output), ssid_special,
                                  "q\"#\\{}123", TRUE));
    CHECK(strcmp(output,
        "ap_scan=1\nnetwork={\n\tssid=61225c0ac3a9\n"
        "\tpsk=\"q\"#\\{}123\"\n\tkey_mgmt=WPA-PSK\n}\n") == 0);

    memset(ssid_max, 'x', 32);
    ssid_max[32] = '\0';
    for (i = 0; i < 32; ++i)
        strcat(expected_max, "78");
    strcat(expected_max, "\n\tkey_mgmt=NONE\n}\n");
    CHECK(WifiFormatVolatilePrefs(output, sizeof(output), ssid_max, NULL,
                                  FALSE));
    CHECK(strcmp(output, expected_max) == 0);

    memset(ssid_long, 'y', 33);
    ssid_long[33] = '\0';
    memset(output, 0xa5, sizeof(output));
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), ssid_long, NULL,
                                   FALSE));
    CHECK(output[0] == '\0');
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "", NULL, FALSE));
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), NULL, NULL, FALSE));

    CHECK(WifiFormatVolatilePrefs(output, sizeof(output), "x", "abcdefgh",
                                  TRUE));
    CHECK(strstr(output, "\tpsk=\"abcdefgh\"\n") != NULL);

    memset(key63, '!', 63);
    key63[0] = '"';
    key63[1] = '#';
    key63[2] = '\\';
    key63[3] = '{';
    key63[4] = '}';
    key63[63] = '\0';
    CHECK(WifiFormatVolatilePrefs(output, sizeof(output), "x", key63, TRUE));
    CHECK(strlen(strstr(output, "\tpsk=\"") + 6) >= 64);
    CHECK(strstr(output, "\tpsk=\"\"#\\{}") != NULL);

    memset(key64, 'a', 64);
    key64[64] = '\0';
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "x", key64, TRUE));
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "x", "short!!",
                                   TRUE));
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "x", NULL, TRUE));

    for (i = 1; i < 32; ++i)
    {
        memcpy(bad_key, "abcdefgh", sizeof(bad_key));
        bad_key[i % 8] = (char)i;
        CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "x", bad_key,
                                       TRUE));
    }
    memcpy(bad_key, "abcdefgh", sizeof(bad_key));
    bad_key[3] = 127;
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "x", bad_key,
                                   TRUE));
    memcpy(bad_key, "abcdefgh", sizeof(bad_key));
    bad_key[3] = (char)0x80;
    CHECK(!WifiFormatVolatilePrefs(output, sizeof(output), "x", bad_key,
                                   TRUE));

    memset(output, 0xa5, sizeof(output));
    CHECK(!WifiFormatVolatilePrefs(output, 24, "x", "abcdefgh", TRUE));
    for (i = 0; i < 24; ++i)
        CHECK(output[i] == '\0');
    output[0] = 'z';
    CHECK(!WifiFormatVolatilePrefs(output, 0, "x", "abcdefgh", TRUE));
    CHECK(output[0] == 'z');
    CHECK(!WifiFormatVolatilePrefs(NULL, sizeof(output), "x", "abcdefgh",
                                   TRUE));
}

static void test_ssid_hex(void)
{
    char encoded[65];
    char special[] = {'a', '"', '\\', '\n', (char)0xc3,
                      (char)0xa9, '\0'};
    char maximum[33];
    char too_long[34];
    size_t i;

    CHECK(WifiSSIDHex(encoded, sizeof(encoded), special));
    CHECK(strcmp(encoded, "61225c0ac3a9") == 0);
    CHECK(WifiSSIDHex(encoded, 13, special));
    CHECK(encoded[12] == '\0');
    CHECK(!WifiSSIDHex(encoded, 12, special));
    for (i = 0; i < 12; ++i)
        CHECK(encoded[i] == '\0');

    memset(maximum, 'x', 32);
    maximum[32] = '\0';
    CHECK(WifiSSIDHex(encoded, sizeof(encoded), maximum));
    CHECK(strlen(encoded) == 64);
    for (i = 0; i < 32; ++i)
        CHECK(encoded[2 * i] == '7' && encoded[2 * i + 1] == '8');

    memset(too_long, 'y', 33);
    too_long[33] = '\0';
    memset(encoded, 0xa5, sizeof(encoded));
    CHECK(!WifiSSIDHex(encoded, sizeof(encoded), too_long));
    for (i = 0; i < sizeof(encoded); ++i)
        CHECK(encoded[i] == '\0');
    CHECK(!WifiSSIDHex(encoded, sizeof(encoded), ""));
    CHECK(!WifiSSIDHex(encoded, sizeof(encoded), NULL));
    CHECK(!WifiSSIDHex(NULL, sizeof(encoded), "x"));
}

static void test_clear(void)
{
    char secret[] = "clear-me";
    size_t i;

    WifiClearSecret(secret, sizeof(secret));
    for (i = 0; i < sizeof(secret); ++i)
        CHECK(secret[i] == '\0');
    WifiClearSecret(NULL, 64);
}

static void test_write_path(void)
{
    const char expected[] =
        "ap_scan=1\nnetwork={\n\tssid=74657374\n"
        "\tpsk=\"password\"\n\tkey_mgmt=WPA-PSK\n}\n";

    reset_mocks();
    CHECK(WifiWriteVolatilePrefs("test", "password", TRUE));
    CHECK(open_calls == 1 && puts_calls == 1 && close_calls == 1);
    CHECK(strcmp(opened_path, "ENV:Sys/Wireless.prefs") == 0);
    CHECK(opened_mode == MODE_NEWFILE);
    CHECK(strcmp(puts_text, expected) == 0);

    reset_mocks();
    open_fails = 1;
    CHECK(!WifiWriteVolatilePrefs("test", "password", TRUE));
    CHECK(open_calls == 1 && puts_calls == 0 && close_calls == 0);
    CHECK(strcmp(opened_path, "ENV:Sys/Wireless.prefs") == 0);

    reset_mocks();
    puts_fails = 1;
    CHECK(!WifiWriteVolatilePrefs("test", "password", TRUE));
    CHECK(open_calls == 1 && puts_calls == 1 && close_calls == 1);

    reset_mocks();
    close_fails = 1;
    CHECK(!WifiWriteVolatilePrefs("test", "password", TRUE));
    CHECK(open_calls == 1 && puts_calls == 1 && close_calls == 1);

    reset_mocks();
    CHECK(!WifiWriteVolatilePrefs("test", "short!!", TRUE));
    CHECK(open_calls == 0 && puts_calls == 0 && close_calls == 0);
}

int main(void)
{
    CHECK(WifiSecurityProfileFromLabel("WPA2/3") == WIFI_SECURITY_SAE);
    CHECK(WifiSecurityProfileFromLabel("WPA3") == WIFI_SECURITY_SAE);
    CHECK(WifiSecurityProfileFromLabel("WPA2") == WIFI_SECURITY_PSK);
    CHECK(WifiSecurityProfileFromLabel("WPA2/WPA3") == WIFI_SECURITY_UNKNOWN);
    CHECK(WifiSecurityProfileFromLabel("?") == WIFI_SECURITY_UNKNOWN);
    CHECK(WifiSecurityProfileFromLabel(NULL) == WIFI_SECURITY_UNKNOWN);
    test_ssid_hex();
    test_formatting();
    test_clear();
    test_write_path();
    printf("C contract checks: %u passed\n", checks);
    return 0;
}
'''

WORKER_HARNESS_PREFIX = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "volatile_config.h"
#include <dos/dos.h>

#define PREFS_ENV "ENV:Sys/Wireless.prefs"
#define PREFS_ENVARC "ENVARC:Sys/Wireless.prefs"
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define OFFSET_END 1
#define OFFSET_BEGINNING -1
#define MEMF_ANY 1
#define MEMF_CLEAR 2
#define ERROR_OBJECT_NOT_FOUND 205
#define MOCK_IO_ERROR 500

static unsigned checks;
static unsigned alloc_calls, free_calls, bad_free_calls;
static unsigned open_calls, close_calls, active_handles;
static unsigned successful_open_calls;
static BOOL fail_alloc_next;
static LONG io_error;
BOOL WifiVolatile;

struct FileState
{
    char path[80];
    BOOL exists;
    BOOL fail_open;
    BOOL fail_seek_end;
    BOOL fail_seek_begin;
    BOOL fail_read;
    BOOL fail_write;
    BOOL fail_close;
    BOOL fail_delete;
    LONG length;
    char data[512];
    unsigned read_open_attempts;
    unsigned write_open_attempts;
    unsigned writes;
    unsigned deletes;
};

struct MockHandle
{
    struct FileState *file;
    LONG mode;
    LONG position;
    BOOL active;
};

struct Allocation
{
    APTR pointer;
    size_t length;
};

static struct FileState env_file, envarc_file;
static struct MockHandle handles[16];
static struct Allocation allocations[32];
static unsigned handles_used;

static void check_at(int condition, const char *description, int line)
{
    ++checks;
    if (!condition)
    {
        fprintf(stderr, "FAIL at line %d: %s\n", line, description);
        exit(1);
    }
}

#define CHECK(x) check_at((x), #x, __LINE__)

static void reset_case(void)
{
    unsigned i;
    CHECK(active_handles == 0);
    for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i)
        CHECK(allocations[i].pointer == NULL);
    memset(&env_file, 0, sizeof(env_file));
    memset(&envarc_file, 0, sizeof(envarc_file));
    strcpy(env_file.path, PREFS_ENV);
    strcpy(envarc_file.path, PREFS_ENVARC);
    memset(handles, 0, sizeof(handles));
    handles_used = 0;
    alloc_calls = free_calls = bad_free_calls = 0;
    open_calls = close_calls = successful_open_calls = 0;
    fail_alloc_next = FALSE;
    io_error = 0;
    WifiVolatile = FALSE;
}

static struct FileState *file_for(CONST_STRPTR path)
{
    if (strcmp(path, PREFS_ENV) == 0)
        return &env_file;
    if (strcmp(path, PREFS_ENVARC) == 0)
        return &envarc_file;
    return NULL;
}

static void put_file(struct FileState *file, CONST_STRPTR text)
{
    size_t length = strlen(text);
    CHECK(length < sizeof(file->data));
    file->exists = TRUE;
    file->length = (LONG)length;
    memcpy(file->data, text, length + 1);
}

static struct MockHandle *handle_for(BPTR handle)
{
    struct MockHandle *mock = (struct MockHandle *)(uintptr_t)handle;
    return mock != NULL && mock->active ? mock : NULL;
}

BPTR Open(CONST_STRPTR path, LONG mode)
{
    struct FileState *file = file_for(path);
    struct MockHandle *mock;

    ++open_calls;
    if (file == NULL)
    {
        io_error = MOCK_IO_ERROR;
        return BNULL;
    }
    if (mode == MODE_OLDFILE)
    {
        ++file->read_open_attempts;
        if (file->fail_open)
        {
            io_error = MOCK_IO_ERROR;
            return BNULL;
        }
        if (!file->exists)
        {
            io_error = ERROR_OBJECT_NOT_FOUND;
            return BNULL;
        }
    }
    else
    {
        ++file->write_open_attempts;
        if (file->fail_open)
        {
            io_error = MOCK_IO_ERROR;
            return BNULL;
        }
        file->exists = TRUE;
        file->length = 0;
        file->data[0] = '\0';
    }
    CHECK(handles_used < sizeof(handles) / sizeof(handles[0]));
    mock = &handles[handles_used++];
    mock->file = file;
    mock->mode = mode;
    mock->position = 0;
    mock->active = TRUE;
    ++active_handles;
    ++successful_open_calls;
    return (BPTR)(uintptr_t)mock;
}

LONG Seek(BPTR handle, LONG position, LONG mode)
{
    struct MockHandle *mock = handle_for(handle);
    LONG previous;
    if (mock == NULL || mock->mode != MODE_OLDFILE || position != 0)
    {
        io_error = MOCK_IO_ERROR;
        return -1;
    }
    if (mode == OFFSET_END)
    {
        if (mock->file->fail_seek_end)
            return -1;
        previous = mock->position;
        mock->position = mock->file->length;
        return previous;
    }
    if (mode == OFFSET_BEGINNING)
    {
        if (mock->file->fail_seek_begin)
            return -1;
        previous = mock->position;
        mock->position = 0;
        return previous;
    }
    return -1;
}

LONG Read(BPTR handle, APTR buffer, LONG length)
{
    struct MockHandle *mock = handle_for(handle);
    LONG amount = length;
    if (mock == NULL || mock->mode != MODE_OLDFILE || length < 0)
        return -1;
    if (mock->file->fail_read && amount > 0)
        --amount;
    if (amount > 0)
        memcpy(buffer, mock->file->data + mock->position, (size_t)amount);
    return amount;
}

LONG Write(BPTR handle, CONST_APTR buffer, LONG length)
{
    struct MockHandle *mock = handle_for(handle);
    LONG amount = length;
    if (mock == NULL || mock->mode != MODE_NEWFILE || length < 0)
        return -1;
    ++mock->file->writes;
    if (mock->file->fail_write && amount > 0)
        --amount;
    if ((size_t)amount >= sizeof(mock->file->data))
        amount = (LONG)sizeof(mock->file->data) - 1;
    if (amount > 0)
        memcpy(mock->file->data, buffer, (size_t)amount);
    mock->file->data[amount] = '\0';
    mock->file->length = amount;
    return amount;
}

BOOL Close(BPTR handle)
{
    struct MockHandle *mock = handle_for(handle);
    if (mock == NULL)
    {
        io_error = MOCK_IO_ERROR;
        return FALSE;
    }
    mock->active = FALSE;
    --active_handles;
    ++close_calls;
    if (mock->file->fail_close)
    {
        io_error = MOCK_IO_ERROR;
        return FALSE;
    }
    return TRUE;
}

LONG FPuts(BPTR handle, CONST_STRPTR text)
{
    (void)handle;
    (void)text;
    return 0;
}

LONG IoErr(void)
{
    return io_error;
}

APTR AllocVec(ULONG size, ULONG flags)
{
    unsigned i;
    APTR pointer;
    if (fail_alloc_next)
    {
        fail_alloc_next = FALSE;
        return NULL;
    }
    pointer = malloc(size == 0 ? 1 : (size_t)size);
    if (pointer == NULL)
        return NULL;
    if ((flags & MEMF_CLEAR) != 0)
        memset(pointer, 0, size == 0 ? 1 : (size_t)size);
    for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i)
        if (allocations[i].pointer == NULL)
        {
            allocations[i].pointer = pointer;
            allocations[i].length = size == 0 ? 1 : (size_t)size;
            ++alloc_calls;
            return pointer;
        }
    free(pointer);
    return NULL;
}

void FreeVec(APTR pointer)
{
    unsigned i;
    if (pointer == NULL)
        return;
    for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i)
        if (allocations[i].pointer == pointer)
        {
            size_t n;
            const UBYTE *bytes = pointer;
            for (n = 0; n < allocations[i].length; ++n)
                if (bytes[n] != 0)
                {
                    ++bad_free_calls;
                    break;
                }
            free(pointer);
            allocations[i].pointer = NULL;
            allocations[i].length = 0;
            ++free_calls;
            return;
        }
    check_at(FALSE, "FreeVec called with an untracked pointer", __LINE__);
}

BOOL DeleteFile(CONST_STRPTR path)
{
    struct FileState *file = file_for(path);
    if (file == NULL)
    {
        io_error = MOCK_IO_ERROR;
        return FALSE;
    }
    ++file->deletes;
    if (file->fail_delete)
    {
        io_error = MOCK_IO_ERROR;
        return FALSE;
    }
    if (!file->exists)
    {
        io_error = ERROR_OBJECT_NOT_FOUND;
        return FALSE;
    }
    file->exists = FALSE;
    file->length = 0;
    file->data[0] = '\0';
    return TRUE;
}

static void check_balanced(void)
{
    unsigned i;
    CHECK(active_handles == 0);
    CHECK(successful_open_calls == close_calls);
    CHECK(alloc_calls == free_calls);
    CHECK(bad_free_calls == 0);
    for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i)
        CHECK(allocations[i].pointer == NULL);
}
'''

WORKER_HARNESS_TESTS = r'''
static void test_reader_states(void)
{
    LONG length;
    BOOL ok;
    STRPTR data;

    reset_case();
    length = -1;
    ok = FALSE;
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data == NULL && length == 0 && ok);
    CHECK(env_file.read_open_attempts == 1 && close_calls == 0);
    check_balanced();

    reset_case();
    env_file.fail_open = TRUE;
    ok = TRUE;
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data == NULL && length == 0 && !ok);
    check_balanced();

    reset_case();
    put_file(&env_file, "");
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data != NULL && data[0] == '\0' && length == 0 && ok);
    FreeVec(data);
    CHECK(close_calls == 1);
    check_balanced();

    reset_case();
    put_file(&env_file, "psk=reader-secret");
    env_file.fail_seek_end = TRUE;
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data == NULL && !ok && close_calls == 1);
    check_balanced();

    reset_case();
    put_file(&env_file, "psk=reader-secret");
    env_file.fail_seek_begin = TRUE;
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data == NULL && !ok && close_calls == 1);
    check_balanced();

    reset_case();
    put_file(&env_file, "psk=reader-secret");
    fail_alloc_next = TRUE;
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data == NULL && !ok && close_calls == 1);
    check_balanced();

    reset_case();
    put_file(&env_file, "psk=reader-secret");
    env_file.fail_read = TRUE;
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data == NULL);
    CHECK(!ok);
    CHECK(close_calls == 1);
    CHECK(alloc_calls == 1 && free_calls == 1 && bad_free_calls == 0);
    check_balanced();

    reset_case();
    put_file(&env_file, "psk=reader-secret");
    data = ReadPrefsChecked(PREFS_ENV, &length, &ok);
    CHECK(data != NULL && length == (LONG)strlen("psk=reader-secret") && ok);
    CHECK(strcmp(data, "psk=reader-secret") == 0);
    WifiClearSecret(data, (size_t)length);
    FreeVec(data);
    check_balanced();
}

static void test_save_and_free(void)
{
    struct PrefsBackup backup;

    reset_case();
    WifiVolatile = TRUE;
    put_file(&env_file, "psk=volatile-secret");
    put_file(&envarc_file, "psk=persistent-secret");
    memset(&backup, 0, sizeof(backup));
    CHECK(SavePrefs(&backup));
    CHECK(backup.pb_Taken && backup.pb_Env != NULL);
    CHECK(envarc_file.read_open_attempts == 0);
    FreePrefsBackup(&backup);
    CHECK(backup.pb_Env == NULL && backup.pb_Envarc == NULL && !backup.pb_Taken);
    check_balanced();

    reset_case();
    put_file(&env_file, "");
    memset(&backup, 0, sizeof(backup));
    CHECK(SavePrefs(&backup));
    CHECK(backup.pb_Env != NULL && backup.pb_EnvLen == 0);
    CHECK(backup.pb_Envarc == NULL);
    FreePrefsBackup(&backup);
    check_balanced();

    reset_case();
    WifiVolatile = FALSE;
    put_file(&env_file, "psk=env-secret");
    put_file(&envarc_file, "psk=envarc-secret");
    memset(&backup, 0, sizeof(backup));
    CHECK(SavePrefs(&backup));
    CHECK(env_file.read_open_attempts == 1 && envarc_file.read_open_attempts == 1);
    FreePrefsBackup(&backup);
    check_balanced();

    reset_case();
    WifiVolatile = FALSE;
    put_file(&env_file, "psk=env-secret");
    put_file(&envarc_file, "psk=envarc-secret");
    envarc_file.fail_read = TRUE;
    memset(&backup, 0, sizeof(backup));
    CHECK(!SavePrefs(&backup));
    CHECK(!backup.pb_Taken && backup.pb_Env != NULL);
    FreePrefsBackup(&backup);
    check_balanced();
}

static void test_write_whole_failures(void)
{
    char secret[] = "psk=write-secret";

    reset_case();
    env_file.fail_open = TRUE;
    CHECK(!WriteWhole(PREFS_ENV, secret, (LONG)strlen(secret)));
    CHECK(env_file.write_open_attempts == 1 && close_calls == 0);
    check_balanced();

    reset_case();
    env_file.fail_write = TRUE;
    CHECK(!WriteWhole(PREFS_ENV, secret, (LONG)strlen(secret)));
    CHECK(env_file.writes == 1 && close_calls == 1);
    check_balanced();

    reset_case();
    env_file.fail_close = TRUE;
    CHECK(!WriteWhole(PREFS_ENV, secret, (LONG)strlen(secret)));
    CHECK(env_file.writes == 1 && close_calls == 1);
    check_balanced();

    reset_case();
    CHECK(WriteWhole(PREFS_ENV, secret, (LONG)strlen(secret)));
    CHECK(env_file.exists && strcmp(env_file.data, secret) == 0);
    CHECK(close_calls == 1);
    check_balanced();

    reset_case();
    CHECK(WriteWhole(PREFS_ENV, NULL, 0));
    CHECK(env_file.deletes == 1 && !env_file.exists);
    check_balanced();

    reset_case();
    env_file.fail_delete = TRUE;
    CHECK(!WriteWhole(PREFS_ENV, NULL, 0));
    CHECK(env_file.deletes == 1);
    check_balanced();
}

static void test_restore_paths(void)
{
    struct PrefsBackup backup;

    reset_case();
    WifiVolatile = TRUE;
    put_file(&env_file, "psk=env-before");
    put_file(&envarc_file, "psk=arc-before");
    memset(&backup, 0, sizeof(backup));
    CHECK(SavePrefs(&backup));
    put_file(&env_file, "psk=env-after");
    CHECK(RestorePrefs(&backup));
    CHECK(strcmp(env_file.data, "psk=env-before") == 0);
    CHECK(envarc_file.write_open_attempts == 0);
    FreePrefsBackup(&backup);
    check_balanced();

    reset_case();
    WifiVolatile = FALSE;
    put_file(&env_file, "psk=env-before");
    put_file(&envarc_file, "psk=arc-before");
    memset(&backup, 0, sizeof(backup));
    CHECK(SavePrefs(&backup));
    put_file(&env_file, "psk=env-after");
    put_file(&envarc_file, "psk=arc-after");
    env_file.fail_write = TRUE;
    CHECK(!RestorePrefs(&backup));
    CHECK(env_file.write_open_attempts == 1 && envarc_file.write_open_attempts == 1);
    CHECK(strcmp(envarc_file.data, "psk=arc-before") == 0);
    FreePrefsBackup(&backup);
    check_balanced();

    reset_case();
    memset(&backup, 0, sizeof(backup));
    CHECK(RestorePrefs(&backup));
    CHECK(open_calls == 0);
    check_balanced();
}

int main(void)
{
    test_reader_states();
    test_save_and_free();
    test_write_whole_failures();
    test_restore_paths();
    printf("Extracted worker helper checks: %u passed; alloc/free balanced\n",
           checks);
    return 0;
}
'''


STUBS = {
    "exec/types.h": r'''#ifndef TEST_EXEC_TYPES_H
#define TEST_EXEC_TYPES_H
#include <stddef.h>
typedef unsigned char UBYTE;
typedef unsigned long ULONG;
typedef long LONG;
typedef int BOOL;
typedef void *APTR;
typedef const void *CONST_APTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
#define TRUE 1
#define FALSE 0
#endif
''',
    "dos/dos.h": r'''#ifndef TEST_DOS_DOS_H
#define TEST_DOS_DOS_H
#include <stdint.h>
#include <exec/types.h>
typedef uintptr_t BPTR;
#define BNULL ((BPTR)0)
#define MODE_NEWFILE 1006
#endif
''',
    "proto/dos.h": r'''#ifndef TEST_PROTO_DOS_H
#define TEST_PROTO_DOS_H
#include <dos/dos.h>
BPTR Open(CONST_STRPTR path, LONG mode);
LONG FPuts(BPTR file, CONST_STRPTR text);
BOOL Close(BPTR file);
#endif
''',
}


def _skip_c_space_and_comments(source: str, position: int) -> int:
    while position < len(source):
        if source[position].isspace():
            position += 1
        elif source.startswith("/*", position):
            end = source.find("*/", position + 2)
            if end < 0:
                raise AssertionError("unterminated C block comment")
            position = end + 2
        elif source.startswith("//", position):
            end = source.find("\n", position + 2)
            position = len(source) if end < 0 else end + 1
        else:
            break
    return position


def _matching_brace(source: str, opening: int) -> int:
    depth = 0
    position = opening
    state = "code"
    while position < len(source):
        char = source[position]
        following = source[position + 1] if position + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "/" and following == "*":
                state = "block-comment"
                position += 1
            elif char == "/" and following == "/":
                state = "line-comment"
                position += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return position
        elif state in ("string", "character"):
            if char == "\\":
                position += 1
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif state == "block-comment" and char == "*" and following == "/":
            state = "code"
            position += 1
        elif state == "line-comment" and char == "\n":
            state = "code"
        position += 1
    raise AssertionError("unterminated C function body")


def function_body(source: str, name: str) -> str:
    """Return the body of a named C function, skipping its forward prototype."""
    name_pattern = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for match in name_pattern.finditer(source):
        position = match.end()
        depth = 1
        while position < len(source) and depth:
            if source[position] == "(":
                depth += 1
            elif source[position] == ")":
                depth -= 1
            position += 1
        if depth:
            continue
        position = _skip_c_space_and_comments(source, position)
        if position < len(source) and source[position] == "{":
            end = _matching_brace(source, position)
            return source[position + 1 : end]
    raise AssertionError(f"could not find definition of {name}()")


def function_definition(source: str, name: str) -> str:
    """Return a function's real declaration and body for the isolated mock TU."""
    name_pattern = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for match in name_pattern.finditer(source):
        position = match.end()
        depth = 1
        while position < len(source) and depth:
            if source[position] == "(":
                depth += 1
            elif source[position] == ")":
                depth -= 1
            position += 1
        if depth:
            continue
        position = _skip_c_space_and_comments(source, position)
        if position < len(source) and source[position] == "{":
            end = _matching_brace(source, position)
            start = source.rfind("\n", 0, match.start()) + 1
            return source[start : end + 1].strip()
    raise AssertionError(f"could not extract definition of {name}()")


def structure_definition(source: str, name: str) -> str:
    match = re.search(r"\bstruct\s+" + re.escape(name) + r"\s*\{", source)
    if match is None:
        raise AssertionError(f"could not find struct {name}")
    opening = source.find("{", match.start())
    end = _matching_brace(source, opening)
    return source[match.start() : end + 1] + ";"


def if_body(source: str, condition: str) -> str:
    match = re.search(r"\bif\s*\(\s*" + re.escape(condition) + r"\s*\)", source)
    if match is None:
        raise AssertionError(f"could not find if ({condition})")
    position = _skip_c_space_and_comments(source, match.end())
    if position >= len(source) or source[position] != "{":
        raise AssertionError(f"if ({condition}) has no braced body")
    return source[position + 1 : _matching_brace(source, position)]


def source_contract_checks() -> int:
    worker = (WIFI / "worker.c").read_text(encoding="utf-8")
    checks = 0

    def require(condition: bool, description: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(description)

    prefs = function_body(worker, "PrefsHaveNetwork")
    require(
        re.search(r"WifiVolatile\s*\?\s*PREFS_ENV\s*:\s*PREFS_ENVARC", prefs)
        is not None,
        "network lookup must read ENV in volatile mode and ENVARC otherwise",
    )
    require(
        "WifiSSIDHex(encoded, sizeof(encoded), ssid)" in prefs
        and '"ssid=%s\\n"' in prefs,
        "network lookup must match the hex SSID written by volatile_config.c",
    )
    invalid_ssid = if_body(
        prefs, "!WifiSSIDHex(encoded, sizeof(encoded), ssid)"
    )
    require(
        invalid_ssid.find("WifiClearSecret(buf, (size_t)len)")
        < invalid_ssid.find("FreeVec(buf)"),
        "invalid volatile SSID must wipe the preference buffer before free",
    )

    stack_dir = function_body(worker, "StackDBDir")
    volatile_branch = if_body(stack_dir, "WifiVolatile")
    require(
        "ENVARC:" not in volatile_branch and '"ENV:AROSTCP/db"' in volatile_branch,
        "volatile stack database path must stay on ENV:AROSTCP/db",
    )
    require(
        'SetVar("AROSTCP/Config", out, -1, GVF_GLOBAL_ONLY)' in volatile_branch
        and "GVF_SAVE_VAR" not in volatile_branch,
        "volatile stack path may be set at runtime but must never be saved",
    )
    persistent_branch = stack_dir[stack_dir.find(volatile_branch) + len(volatile_branch) :]
    require(
        'CreateDir("ENVARC:AROSTCP")' in persistent_branch,
        "persistent stack database setup must retain ENVARC directory creation",
    )
    require(
        'SetVar("AROSTCP/Config", out, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR)'
        in persistent_branch,
        "persistent stack database configuration must remain saved",
    )

    save = function_body(worker, "SavePrefs")
    require(
        "ReadPrefsChecked(PREFS_ENV" in save
        and re.search(
            r"if\s*\(\s*!WifiVolatile\s*\)\s*"
            r"b->pb_Envarc\s*=\s*ReadPrefsChecked\(PREFS_ENVARC",
            save,
        )
        is not None,
        "backup must always capture ENV and capture ENVARC only when persistent",
    )
    require(
        "b->pb_Taken = env_ok && envarc_ok" in save
        and "return b->pb_Taken" in save,
        "backup is usable only when every required preference read succeeded",
    )

    restore = function_body(worker, "RestorePrefs")
    require(
        "result = WriteWhole(PREFS_ENV" in restore
        and re.search(
            r"if\s*\(\s*!WifiVolatile\s*\)\s*"
            r"if\s*\(\s*!WriteWhole\(PREFS_ENVARC",
            restore,
        )
        is not None
        and restore.find("WriteWhole(PREFS_ENV")
        < restore.find("WriteWhole(PREFS_ENVARC"),
        "rollback restores ENV first and still attempts ENVARC in persistent mode",
    )

    write_whole = function_body(worker, "WriteWhole")
    require(
        "return DeleteFile(name) || IoErr() == ERROR_OBJECT_NOT_FOUND" in write_whole
        and "result = Write(fh, buf, len) == len" in write_whole
        and "if (!Close(fh))" in write_whole,
        "rollback writes must report delete, short-write, open and close failures",
    )

    free_backup = function_body(worker, "FreePrefsBackup")
    require(
        free_backup.find("WifiClearSecret(b->pb_Env")
        < free_backup.find("FreeVec(b->pb_Env")
        and free_backup.find("WifiClearSecret(b->pb_Envarc")
        < free_backup.find("FreeVec(b->pb_Envarc"),
        "backup buffers must be wiped before free",
    )
    require(
        "b->pb_Env = NULL" in free_backup
        and "b->pb_Envarc = NULL" in free_backup
        and "b->pb_Taken = FALSE" in free_backup,
        "freed backup state must be cleared",
    )

    read_prefs = function_body(worker, "ReadPrefsChecked")
    require(
        read_prefs.find("WifiClearSecret(buf, (size_t)size + 1)")
        < read_prefs.find("FreeVec(buf)"),
        "short-read preference buffer must be wiped before free",
    )
    require(
        "*ok = IoErr() == ERROR_OBJECT_NOT_FOUND" in read_prefs
        and "if (Read(fh, buf, size) == size)" in read_prefs
        and "*ok = TRUE" in read_prefs,
        "checked reads must distinguish missing files from I/O failure and empty files",
    )

    connect = function_body(worker, "DoConnect")
    backup_failure = if_body(connect, "!SavePrefs(&backup)")
    require(
        "FreePrefsBackup(&backup)" in backup_failure
        and "return" in backup_failure
        and connect.find("if (!SavePrefs(&backup))")
        < connect.find("WifiWriteVolatilePrefs"),
        "connection must stop and clear partial backups before writing on backup failure",
    )
    require(
        re.search(r"!WifiVolatile\s*&&\s*\(\s*!MergePrefs\(PREFS_ENVARC", connect)
        is not None
        and "!MergePrefs(PREFS_ENV" in connect,
        "persistent connection writes must keep both ENVARC and ENV merges",
    )
    rollback_failure = if_body(connect, "!RestorePrefs(&backup)")
    require(
        "m->wm_Result = WRES_FAILED" in rollback_failure
        and "MSG_ST_CANNOT_WRITE_PREFS" in rollback_failure,
        "failed rollback must surface as a failed connection with a write error",
    )

    run_connect = function_body(worker, "RunConnect")
    require(
        re.search(
            r"if\s*\(\s*!WifiVolatile\s*\)\s*"
            r"m->wm_Result\s*=\s*WRES_OK",
            run_connect,
        )
        is not None
        and re.search(
            r"if\s*\(\s*m->wm_Address\[0\]\s*!=\s*'\\0'\s*\)\s*"
            r"\{\s*m->wm_Result\s*=\s*WRES_OK",
            run_connect,
        )
        is not None
        and re.search(
            r"if\s*\(\s*WifiVolatile\s*&&\s*m->wm_Associated\s*&&\s*"
            r"m->wm_Address\[0\]\s*!=\s*'\\0'\s*\)\s*"
            r"m->wm_Result\s*=\s*WRES_OK",
            run_connect,
        )
        is not None,
        "volatile connect commits success only after an address is available",
    )

    writer = function_body(
        (WIFI / "volatile_config.c").read_text(encoding="utf-8"),
        "WifiWriteVolatilePrefs",
    )
    require(
        '"ENV:Sys/Wireless.prefs"' in writer
        and "ENVARC:" not in writer
        and "WifiClearSecret(output, sizeof(output))" in writer,
        "volatile writer must use ENV only and clear its local buffer",
    )
    print(f"Worker source contract checks: {checks} passed")
    return checks


def compile_and_run(compiler: str, optimization: str) -> str:
    with tempfile.TemporaryDirectory(prefix="wifi-volatile-tests-") as temp:
        temp_path = Path(temp)
        include = temp_path / "include"
        for name, contents in STUBS.items():
            path = include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        harness = temp_path / "host-regression.c"
        harness.write_text(HARNESS, encoding="utf-8")
        executable = temp_path / "host-regression"
        command = [
            compiler,
            "-std=c99",
            optimization,
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I",
            str(include),
            "-I",
            str(WIFI),
            str(harness),
            str(WIFI / "volatile_config.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True, cwd=WORKBENCH)
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        result = subprocess.run(
            [str(executable)],
            check=False,
            cwd=WORKBENCH,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"volatile_config harness exited {result.returncode}:\n{result.stdout}"
            )
        return result.stdout.strip()


def compile_and_run_worker_helpers(compiler: str, optimization: str) -> str:
    worker = (WIFI / "worker.c").read_text(encoding="utf-8")
    extracted = [
        structure_definition(worker, "PrefsBackup"),
        function_definition(worker, "ReadPrefsChecked"),
        function_definition(worker, "SavePrefs"),
        function_definition(worker, "WriteWhole"),
        function_definition(worker, "RestorePrefs"),
        function_definition(worker, "FreePrefsBackup"),
    ]
    harness_source = (
        WORKER_HARNESS_PREFIX
        + "\n"
        + "\n\n".join(extracted)
        + "\n"
        + WORKER_HARNESS_TESTS
    )
    with tempfile.TemporaryDirectory(prefix="wifi-worker-tests-") as temp:
        temp_path = Path(temp)
        include = temp_path / "include"
        for name, contents in STUBS.items():
            path = include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        harness = temp_path / "worker-helper-regression.c"
        harness.write_text(harness_source, encoding="utf-8")
        executable = temp_path / "worker-helper-regression"
        command = [
            compiler,
            "-std=c99",
            optimization,
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I",
            str(include),
            "-I",
            str(WIFI),
            str(harness),
            str(WIFI / "volatile_config.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True, cwd=WORKBENCH)
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        result = subprocess.run(
            [str(executable)],
            check=False,
            cwd=WORKBENCH,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"extracted worker harness exited {result.returncode}:\n{result.stdout}"
            )
        return result.stdout.strip()


def main() -> int:
    compiler = os.environ.get("CC", "clang")
    if not (WIFI / "volatile_config.c").is_file():
        raise SystemExit(f"production source not found: {WIFI / 'volatile_config.c'}")

    source_checks = source_contract_checks()
    for optimization in ("-O0", "-O2"):
        output = compile_and_run(compiler, optimization)
        print(f"volatile_config.c {optimization}: {output}")
        worker_output = compile_and_run_worker_helpers(compiler, optimization)
        print(f"extracted worker helpers {optimization}: {worker_output}")
    print(f"Total source contract checks: {source_checks}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
