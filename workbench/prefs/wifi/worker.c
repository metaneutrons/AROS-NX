/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Device and configuration work for the WiFi window.

    Everything that can block lives here: a scan sits in the driver for several
    seconds, and connecting stops and starts the supplicant. The window talks to
    this process with messages only - it never calls into another task's state,
    which is exactly where WirelessManager's own GUI went wrong.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <devices/sana2wireless.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <dos/dostags.h>
#include <devices/newstyle.h>
#include <utility/tagitem.h>

/*
 * The address has to come from the stack, and the stack may not be there: open
 * bsdsocket.library by hand rather than letting the link library open it at
 * startup, or this tool would refuse to run in exactly the case it is wanted.
 */
#define __BSDSOCKET_NOLIBBASE__

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

#include <proto/exec.h>
#include <proto/socket.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/alib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "wifigui.h"
#include "locale.h"
#include "volatile_config.h"
#include "security_display.h"

#define WIRELESS_VAR    "AROSTCP/WirelessDevice"
#define STACK_VAR       "SYS/Packages/AROSTCP"
#define PREFS_ENVARC    "ENVARC:Sys/Wireless.prefs"
#define PREFS_ENV       "ENV:Sys/Wireless.prefs"
#define PUDDLE_SIZE     4096
#define ETH_ALEN        6
#define ASSOC_TIMEOUT   20          /* half-seconds to wait for an association */

struct MsgPort *WifiWorkerPort = NULL;
struct Task *WifiMainTask = NULL;

struct Library *SocketBase = NULL;      /* open only while an address is read */

/* SANA-II expects copy hooks even from an opener that never moves a frame. */
static BOOL copy_buff(UBYTE *dst, UBYTE *src, ULONG size)
{
    CopyMem(src, dst, size);
    return TRUE;
}

static const struct TagItem buffer_tags[] =
{
    { S2_CopyToBuff,    (IPTR)copy_buff },
    { S2_CopyFromBuff,  (IPTR)copy_buff },
    { TAG_DONE,         0               }
};

/* ------------------------------------------------------------------------- */
/* Device access                                                             */

struct WifiDev
{
    struct MsgPort      *wd_Port;
    struct IOSana2Req   *wd_Req;
    BOOL                 wd_Opened;
};

static void CloseWifiDev(struct WifiDev *dev)
{
    if (dev->wd_Opened)
        CloseDevice((struct IORequest *)dev->wd_Req);
    FreeVec(dev->wd_Req);
    if (dev->wd_Port != NULL)
        DeleteMsgPort(dev->wd_Port);
    memset(dev, 0, sizeof(*dev));
}

static BOOL OpenWifiDev(struct WifiDev *dev, CONST_STRPTR device, LONG unit)
{
    memset(dev, 0, sizeof(*dev));

    dev->wd_Port = CreateMsgPort();
    if (dev->wd_Port == NULL)
        return FALSE;
    dev->wd_Req = AllocVec(sizeof(struct IOSana2Req), MEMF_PUBLIC | MEMF_CLEAR);
    if (dev->wd_Req == NULL)
    {
        CloseWifiDev(dev);
        return FALSE;
    }

    dev->wd_Req->ios2_Req.io_Message.mn_ReplyPort = dev->wd_Port;
    dev->wd_Req->ios2_Req.io_Message.mn_Length = sizeof(struct IOSana2Req);
    dev->wd_Req->ios2_BufferManagement = (APTR)buffer_tags;

    if (OpenDevice(device, unit, (struct IORequest *)dev->wd_Req, 0) != 0)
    {
        CloseWifiDev(dev);
        return FALSE;
    }
    dev->wd_Opened = TRUE;
    return TRUE;
}

/* Split "<device> UNIT <n>", the form the network preferences write. */
static void SplitDeviceVar(STRPTR buf, STRPTR *device, LONG *unit)
{
    STRPTR p = buf;

    *device = buf;
    while (*p != '\0' && *p != ' ')
        p++;
    if (*p == '\0')
        return;
    *p++ = '\0';

    while (*p == ' ')
        p++;
    if (Strnicmp(p, "UNIT", 4) == 0)
    {
        p += 4;
        while (*p == ' ')
            p++;
        if (*p >= '0' && *p <= '9')
            *unit = atol(p);
    }
}

static BOOL WifiDevice(struct WifiMsg *m)
{
    STRPTR device = NULL;

    if (m->wm_Device[0] != '\0')
        return TRUE;                    /* already resolved once */

    if (GetVar(WIRELESS_VAR, m->wm_Device, sizeof(m->wm_Device), LV_VAR) <= 0)
    {
        strcpy(m->wm_Status, (const char *)_(MSG_ST_NO_DEVICE_VAR));
        return FALSE;
    }

    m->wm_Unit = 0;
    SplitDeviceVar(m->wm_Device, &device, &m->wm_Unit);
    return TRUE;
}

/* Name the protection from the beacon IEs (a UWORD count, then the elements). */
static CONST_STRPTR IEProtection(const UBYTE *ies)
{
    ULONG len, off;
    CONST_STRPTR prot = "open";

    if (ies == NULL)
        return "?";

    len = *(const UWORD *)ies;
    ies += 2;

    for (off = 0; off + 2 <= len; off += 2 + ies[off + 1])
    {
        UBYTE id = ies[off], ielen = ies[off + 1];

        if (off + 2 + ielen > len)
            break;
        if (id == 48)
            return "WPA2";
        if (id == 221 && ielen >= 4 &&
            ies[off + 2] == 0x00 && ies[off + 3] == 0x50 &&
            ies[off + 4] == 0xf2 && ies[off + 5] == 0x01)
            prot = "WPA";
    }

    return prot;
}

/* ------------------------------------------------------------------------- */
/* Wireless.prefs                                                            */

static STRPTR ReadPrefsChecked(CONST_STRPTR name, LONG *lenp, BOOL *ok)
{
    BPTR fh = Open(name, MODE_OLDFILE);
    LONG size;
    STRPTR buf = NULL;

    *lenp = 0;
    *ok = FALSE;
    if (fh == BNULL)
    {
        *ok = IoErr() == ERROR_OBJECT_NOT_FOUND;
        return NULL;
    }

    if (Seek(fh, 0, OFFSET_END) == -1)
    {
        Close(fh);
        return NULL;
    }
    size = Seek(fh, 0, OFFSET_BEGINNING);
    /* An existing empty file is distinct from an absent file during rollback. */
    if (size >= 0 && size < 0x7fffffff &&
        (buf = AllocVec((ULONG)size + 1, MEMF_ANY | MEMF_CLEAR)) != NULL)
    {
        if (Read(fh, buf, size) == size)
        {
            *lenp = size;
            *ok = TRUE;
        }
        else
        {
            WifiClearSecret(buf, (size_t)size + 1);
            FreeVec(buf);
            buf = NULL;
        }
    }
    Close(fh);
    return buf;
}

static STRPTR ReadPrefs(CONST_STRPTR name, LONG *lenp)
{
    BOOL ok;
    return ReadPrefsChecked(name, lenp, &ok);
}

/* TRUE when the legacy PSK path has a saved key for this network. SAE always
 * prompts again until the manager's profile parser can be safely queried. */
static BOOL PrefsHaveNetwork(CONST_STRPTR ssid,
    enum WifiSecurityProfile profile)
{
    LONG len;
    STRPTR buf;
    STRPTR p;
    TEXT needle[2 * WIFI_SSID_MAX + 8];
    BOOL found = FALSE;

    if (profile != WIFI_SECURITY_PSK)
        return FALSE;
    buf = ReadPrefs(WifiVolatile ? PREFS_ENV : PREFS_ENVARC, &len);
    if (buf == NULL)
        return FALSE;

    if (WifiVolatile)
    {
        char encoded[65];
        if (!WifiSSIDHex(encoded, sizeof(encoded), ssid))
        {
            WifiClearSecret(buf, (size_t)len);
            FreeVec(buf);
            return FALSE;
        }
        /* Include the line delimiter: a shorter SSID must not match a prefix. */
        snprintf(needle, sizeof(needle), "ssid=%s\n", encoded);
        if (strstr(buf, needle) == NULL)
        {
            WifiClearSecret(buf, (size_t)len);
            FreeVec(buf);
            return FALSE;
        }
    }

    /* Walk bounded network blocks instead of associating an SSID in one
     * block with a psk in a later one. The shared formatter emits hex SSIDs;
     * the matcher also accepts the quoted form from older saved profiles. */
    p = buf;
    while ((p = strstr(p, "network")) != NULL)
    {
        size_t block_length;
        STRPTR psk;

        if (p[7] != '=' || p[8] != 123)
        {
            p += 7;
            continue;
        }
        if (!WifiConfigBlockLength(p, (size_t)((buf + len) - p),
                &block_length))
            break;
        psk = strstr(p, "psk=");
        if (psk != NULL && psk < p + block_length - 1 &&
            WifiConfigBlockMatchesSSID(p, block_length, ssid))
        {
            found = TRUE;
            break;
        }
        p += block_length;
    }

    WifiClearSecret(buf, (size_t)len);
    FreeVec(buf);
    return found;
}

/*
 * Write the configuration with this network added or replaced, keeping every
 * other block byte for byte. Rewriting the whole file from a parsed view is
 * what loses the networks a user set up elsewhere.
 */
static BOOL MergePrefs(CONST_STRPTR name, CONST_STRPTR ssid,
    CONST_STRPTR key, enum WifiSecurityProfile profile)
{
    LONG len;
    STRPTR buf = ReadPrefs(name, &len);
    TEXT formatted[384];
    BPTR fh;
    STRPTR p;
    BOOL result = TRUE;
    size_t formatted_length;

    if (!WifiFormatPersistentProfileBlock(formatted, sizeof(formatted),
            ssid, key, profile))
    {
        if (buf != NULL)
        {
            WifiClearSecret(buf, (size_t)len);
            FreeVec(buf);
        }
        return FALSE;
    }
    formatted_length = strlen((const char *)formatted);

    fh = Open(name, MODE_NEWFILE);
    if (fh == BNULL)
    {
        if (buf != NULL)
        {
            WifiClearSecret(buf, (size_t)len);
            FreeVec(buf);
        }
        WifiClearSecret(formatted, sizeof(formatted));
        return FALSE;
    }

    /* Copy the old file, skipping any block that names the same network. */
    p = buf;
    while (p != NULL && *p != '\0')
    {
        STRPTR blockstart, blockend, search;
        size_t block_length;

        search = p;
        blockstart = strstr(search, "network");
        while (blockstart != NULL &&
               (blockstart[7] != '=' || blockstart[8] != 123))
        {
            search = blockstart + 7;
            blockstart = strstr(search, "network");
        }
        if (blockstart == NULL)
        {
            size_t remaining = (size_t)(len - (p - buf));
            if (remaining != 0 &&
                Write(fh, p, (LONG)remaining) != (LONG)remaining)
                result = FALSE;
            break;
        }

        if (blockstart > p)
        {
            LONG prefix_length = blockstart - p;
            if (Write(fh, p, prefix_length) != prefix_length)
                result = FALSE;
        }

        if (!WifiConfigBlockLength(blockstart,
                (size_t)((buf + len) - blockstart), &block_length))
        {
            result = FALSE;
            break;
        }
        blockend = blockstart + block_length;

        if (!WifiConfigBlockMatchesSSID((CONST_STRPTR)blockstart,
                block_length, ssid) &&
            Write(fh, blockstart, (LONG)block_length) != (LONG)block_length)
            result = FALSE;
        p = blockend;
    }

    /* And append the one validated profile. */
    if (result && Write(fh, formatted, (LONG)formatted_length) !=
            (LONG)formatted_length)
        result = FALSE;

    if (!Close(fh))
        result = FALSE;
    if (buf != NULL)
    {
        WifiClearSecret(buf, (size_t)len);
        FreeVec(buf);
    }
    WifiClearSecret(formatted, sizeof(formatted));
    return result;
}

/* ------------------------------------------------------------------------- */
/* The supplicant                                                            */

static BOOL StopSupplicant(void)
{
    struct Task *task;
    int tries;

    task = FindTask("C:WirelessManager");
    if (task == NULL)
        return TRUE;

    Signal(task, SIGBREAKF_CTRL_C);
    for (tries = 0; tries < 10; tries++)
    {
        if (FindTask("C:WirelessManager") == NULL)
            return TRUE;
        Delay(25);
    }
    return FALSE;
}

static BOOL StartSupplicant(CONST_STRPTR device, LONG unit)
{
    TEXT command[160];
    struct TagItem tags[] =
    {
        { SYS_Input,    (IPTR)NULL  },
        { SYS_Output,   (IPTR)NULL  },
        { SYS_Error,    (IPTR)NULL  },
        { SYS_Asynch,   (IPTR)TRUE  },
        { TAG_DONE,     0           }
    };
    int tries;

    snprintf(command, sizeof(command),
        "C:WirelessManager \"%s\" UNIT %ld NOGUI\n", device, (long)unit);
    SystemTagList(command, tags);

    for (tries = 0; tries < 20; tries++)
    {
        if (FindTask("C:WirelessManager") != NULL)
            return TRUE;
        Delay(25);
    }
    return FALSE;
}

/* The interfaces file may spell the device in any case. */
static STRPTR FindNoCase(STRPTR haystack, CONST_STRPTR needle)
{
    STRPTR p;

    if (*needle == '\0')
        return haystack;
    for (p = haystack; *p != '\0'; p++)
    {
        CONST_STRPTR n = needle;
        STRPTR q = p;

        while (*n != '\0' && ToUpper(*q) == ToUpper(*n))
        {
            q++;
            n++;
        }
        if (*n == '\0')
            return p;
    }
    return NULL;
}

static BOOL StackDBDir(STRPTR out, ULONG size);
static BOOL ScanInterfaces(CONST_STRPTR path, CONST_STRPTR device,
    STRPTR name, ULONG size, ULONG *used);

/*
 * The stack knows the device as "net0" or whatever the preferences called it,
 * so find the interface whose DEV= names our device.
 */
static BOOL InterfaceName(CONST_STRPTR device, STRPTR name, ULONG size)
{
    TEXT db[192], path[224];

    if (!StackDBDir(db, sizeof(db)))
        return FALSE;
    snprintf((char *)path, sizeof(path), "%s/interfaces", (const char *)db);

    return ScanInterfaces(path, device, name, size, NULL);
}

/* ------------------------------------------------------------------------- */
/* The interface the stack runs IP over                                      */

/*
 * Where the stack will look for its configuration, resolved the way it does
 * itself: ENV:AROSTCP/Config names the directory, and without it the stack
 * derives a path where nothing is installed. So when it is unset, point it at
 * the db directory the package ships - that one is complete.
 */
static BOOL StackDBDir(STRPTR out, ULONG size)
{
    TEXT dir[160];
    BPTR lock;

    /* VOLATILE never falls back to (or rewrites) a database on SYS:/ENVARC:.
     * Its image/startup must supply the complete RAM-backed ENV database. */
    if (WifiVolatile)
    {
        if (size <= strlen("ENV:AROSTCP/db"))
            return FALSE;
        strcpy((char *)out, "ENV:AROSTCP/db");
        lock = Lock(out, ACCESS_READ);
        if (lock == BNULL)
            return FALSE;
        UnLock(lock);
        /* The stack must read the same database; never save this variable. */
        return SetVar("AROSTCP/Config", out, -1, GVF_GLOBAL_ONLY);
    }

    if (GetVar("AROSTCP/Config", out, size, GVF_GLOBAL_ONLY) > 0)
    {
        if ((lock = Lock(out, ACCESS_READ)) != BNULL)
        {
            UnLock(lock);
            return TRUE;
        }
    }

    if (GetVar(STACK_VAR, dir, sizeof(dir), LV_VAR) <= 0)
        return FALSE;
    snprintf((char *)out, size, "%s/db", dir);
    if ((lock = Lock(out, ACCESS_READ)) == BNULL)
        return FALSE;
    UnLock(lock);

    /* Both directories may be absent on a machine whose network preferences
     * have never been saved; SetVar cannot create them. */
    UnLock(CreateDir("ENV:AROSTCP"));
    UnLock(CreateDir("ENVARC:AROSTCP"));
    SetVar("AROSTCP/Config", out, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    return TRUE;
}

/*
 * Both questions the interfaces file answers: which line is ours, and which
 * names are taken. Comments start with '#' or ';' and a line's first word is
 * the interface name.
 */
static BOOL ScanInterfaces(CONST_STRPTR path, CONST_STRPTR device,
    STRPTR name, ULONG size, ULONG *used)
{
    LONG len;
    STRPTR buf = ReadPrefs(path, &len);
    STRPTR line, next;
    BOOL found = FALSE;

    if (buf == NULL)
        return FALSE;

    for (line = buf; line != NULL && *line != '\0'; line = next)
    {
        STRPTR nl = (STRPTR)strchr((const char *)line, '\n');
        ULONG n = 0;
        TEXT first[16];

        next = (nl != NULL) ? nl + 1 : NULL;
        if (nl != NULL)
            *nl = '\0';

        if (*line == '#' || *line == ';' || *line == '\0')
            continue;

        while (line[n] != '\0' && line[n] != ' ' && n < sizeof(first) - 1)
        {
            first[n] = line[n];
            n++;
        }
        first[n] = '\0';
        if (n == 0)
            continue;

        if (used != NULL && Strnicmp(first, (STRPTR)"net", 3) == 0)
        {
            LONG index = atol((const char *)first + 3);

            if (index >= 0 && index < 32)
                *used |= 1UL << index;
        }

        if (!found && FindNoCase(line, device) != NULL)
        {
            snprintf((char *)name, size, "%s", (const char *)first);
            found = TRUE;
            if (used == NULL)
                break;
        }
    }

    FreeVec(buf);
    return found;
}

/*
 * Associating gets us on the air; it takes an interface for the stack to run
 * DHCP over. Rather than requiring a trip through the network preferences for
 * a wireless device that is right here, add the line ourselves - keyed on the
 * device, so a device that already has an interface keeps it untouched.
 */
static BOOL EnsureInterface(struct WifiMsg *m, BOOL *created)
{
    TEXT db[192], path[224], line[288];
    ULONG usednames = 0;
    LONG index;
    BPTR fh;

    *created = FALSE;
    if (!StackDBDir(db, sizeof(db)))
        return FALSE;
    snprintf((char *)path, sizeof(path), "%s/interfaces", (const char *)db);

    if (ScanInterfaces(path, m->wm_Device, m->wm_Interface,
            sizeof(m->wm_Interface), &usednames))
        return TRUE;

    for (index = 0; index < 32; index++)
    {
        if (!(usednames & (1UL << index)))
            break;
    }
    if (index == 32)
        return FALSE;

    snprintf((char *)m->wm_Interface, sizeof(m->wm_Interface), "net%d", index);
    snprintf((char *)line, sizeof(line), "\n%s DEV=%s UNIT=%d IP=DHCP UP\n",
        (const char *)m->wm_Interface, (const char *)m->wm_Device, m->wm_Unit);

    /* Appending keeps whatever else is configured, including the commented-out
     * examples the file ships with. */
    if ((fh = Open(path, MODE_READWRITE)) == BNULL)
        return FALSE;
    Seek(fh, 0, OFFSET_END);
    if (FPuts(fh, line) != 0)
    {
        Close(fh);
        return FALSE;
    }
    Close(fh);

    *created = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* The TCP/IP stack                                                          */

static BOOL StackRunning(void)
{
    return FindTask("bsdsocket.library") != NULL;
}

/*
 * Associating is not the same as having a network: without the stack nobody
 * runs DHCP. A stack that is already up needs no help - the driver reports the
 * new association and it renews the lease by itself.
 */
static BOOL StartStack(void)
{
    TEXT dir[160], command[224];
    struct TagItem tags[] =
    {
        { SYS_Input,    (IPTR)NULL  },
        { SYS_Output,   (IPTR)NULL  },
        { SYS_Error,    (IPTR)NULL  },
        { SYS_Asynch,   (IPTR)TRUE  },
        { TAG_DONE,     0           }
    };
    int tries;

    if (StackRunning())
        return TRUE;

    if (GetVar(STACK_VAR, dir, sizeof(dir), LV_VAR) <= 0)
        return FALSE;

    snprintf(command, sizeof(command), "\"%s/C/AROSTCP\"\n", dir);
    SystemTagList(command, tags);

    for (tries = 0; tries < 40; tries++)
    {
        if (StackRunning())
            return TRUE;
        Delay(25);
    }
    return FALSE;
}

/* One address out of an ioctl reply, or "" when it is unset. */
static void AddrToText(struct ifreq *ifr, STRPTR out, ULONG size)
{
    const UBYTE *a = (const UBYTE *)
        &((struct sockaddr_in *)&ifr->ifr_addr)->sin_addr;

    out[0] = '\0';
    if (a[0] != 0 || a[1] != 0 || a[2] != 0 || a[3] != 0)
        snprintf(out, size, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
}

static void AskIface(int sock, CONST_STRPTR ifname, ULONG req, STRPTR out,
                     ULONG size)
{
    struct ifreq ifr;

    out[0] = '\0';
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (IoctlSocket(sock, req, (char *)&ifr) == 0)
        AddrToText(&ifr, out, size);
}

/* Copy one whitespace- or delimiter-terminated field. */
static void CopyField(STRPTR out, ULONG size, CONST_STRPTR src)
{
    ULONG n = 0;

    while (src[n] > ' ' && src[n] != ';' && src[n] != ',' && n < size - 1)
    {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
}

enum
{
    LT_WORD = 1, LT_STRING, LT_BAD,
    LT_OPEN = '{', LT_CLOSE = '}', LT_SEMI = ';', LT_COMMA = ','
};

struct LeaseToken
{
    CONST_STRPTR text;
    size_t length;
    int kind;
    BOOL escaped;
};

/* Bounded dhclient lexer. Quotes and comments cannot introduce braces/keywords. */
static BOOL LeaseNextToken(CONST_STRPTR data, size_t length, size_t *offset,
    struct LeaseToken *token)
{
    size_t p = *offset, start;

    while (p < length)
    {
        if ((UBYTE)data[p] <= ' ')
            p++;
        else if (data[p] == '#')
        {
            while (p < length && data[p] != '\n')
                p++;
        }
        else
            break;
    }
    if (p == length)
    {
        *offset = p;
        return FALSE;
    }

    token->text = data + p;
    token->length = 1;
    token->escaped = FALSE;
    if (data[p] == '{' || data[p] == '}' || data[p] == ';' || data[p] == ',')
    {
        token->kind = (UBYTE)data[p];
        *offset = p + 1;
        return TRUE;
    }
    if (data[p] == '"')
    {
        start = ++p;
        while (p < length)
        {
            if (data[p] == '\\')
            {
                token->escaped = TRUE;
                if (++p == length)
                    break;
                p++;
            }
            else if (data[p] == '"')
            {
                token->text = data + start;
                token->length = p - start;
                token->kind = LT_STRING;
                *offset = p + 1;
                return TRUE;
            }
            else
                p++;
        }
        token->kind = LT_BAD;
        *offset = length;
        return TRUE;
    }

    start = p;
    while (p < length)
    {
        UBYTE c = (UBYTE)data[p];

        if (c == '\\')
        {
            token->escaped = TRUE;
            p += p + 1 < length ? 2 : 1;
        }
        else if (c <= ' ' || c == '{' || c == '}' || c == ';' ||
            c == ',' || c == '#' || c == '"')
            break;
        else
            p++;
    }
    token->text = data + start;
    token->length = p - start;
    token->kind = LT_WORD;
    *offset = p;
    return TRUE;
}

static BOOL LeaseTokenIs(const struct LeaseToken *token, CONST_STRPTR text)
{
    size_t n = strlen((const char *)text);

    return !token->escaped && (token->kind == LT_WORD ||
        token->kind == LT_STRING) && token->length == n &&
        memcmp(token->text, text, n) == 0;
}

static BOOL LeaseCopyToken(STRPTR out, ULONG size,
    const struct LeaseToken *token)
{
    size_t n;

    if (size == 0 || token->kind != LT_WORD || token->escaped)
        return FALSE;
    n = token->length < size - 1 ? token->length : size - 1;
    memcpy(out, token->text, n);
    out[n] = '\0';
    return n != 0;
}

/* Strict decimal parsing from bounded, unescaped tokens. */
static BOOL LeaseUnsigned(const struct LeaseToken *token, uint64_t *value)
{
    size_t i;
    uint64_t n = 0;

    if (token->kind != LT_WORD || token->escaped || token->length == 0)
        return FALSE;
    for (i = 0; i < token->length; i++)
    {
        unsigned digit = (unsigned char)token->text[i] - '0';

        if (digit > 9 || n > (UINT64_MAX - digit) / 10)
            return FALSE;
        n = n * 10 + digit;
    }
    *value = n;
    return TRUE;
}

/* dhclient print_time writes never, epoch seconds, or a UTC calendar tuple.
 * Convert explicitly: mktime would incorrectly apply the local timezone. */
static BOOL LeaseExpiryActive(const struct LeaseToken fields[3], size_t count,
    time_t now)
{
    uint64_t expiry, weekday, year, month, day, hour, minute, second, days;
    struct LeaseToken part;
    unsigned i, month_days;
    BOOL leap;
    static const unsigned month_lengths[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (now < 0)
        return FALSE;
    if (count == 1 && fields[0].kind == LT_WORD &&
        LeaseTokenIs(&fields[0], "never"))
        return TRUE;
    if (count == 2 && fields[0].kind == LT_WORD &&
        LeaseTokenIs(&fields[0], "epoch"))
        return LeaseUnsigned(&fields[1], &expiry) && expiry > (uint64_t)now;
    if (count != 3 || !LeaseUnsigned(&fields[0], &weekday) || weekday > 6 ||
        fields[0].length != 1 || fields[1].kind != LT_WORD ||
        fields[2].kind != LT_WORD || fields[1].escaped || fields[2].escaped ||
        fields[1].length != 10 || fields[2].length != 8 ||
        fields[1].text[4] != '/' || fields[1].text[7] != '/' ||
        fields[2].text[2] != ':' || fields[2].text[5] != ':')
        return FALSE;

    part = fields[1];
    part.length = 4;
    if (!LeaseUnsigned(&part, &year) || year < 1970 || year > 9999)
        return FALSE;
    part.text = fields[1].text + 5;
    part.length = 2;
    if (!LeaseUnsigned(&part, &month) || month < 1 || month > 12)
        return FALSE;
    part.text = fields[1].text + 8;
    if (!LeaseUnsigned(&part, &day))
        return FALSE;
    leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    month_days = month_lengths[month - 1] + (month == 2 && leap);
    if (day < 1 || day > month_days)
        return FALSE;
    part = fields[2];
    part.length = 2;
    if (!LeaseUnsigned(&part, &hour) || hour > 23)
        return FALSE;
    part.text = fields[2].text + 3;
    if (!LeaseUnsigned(&part, &minute) || minute > 59)
        return FALSE;
    part.text = fields[2].text + 6;
    if (!LeaseUnsigned(&part, &second) || second > 59)
        return FALSE;

    days = 365 * (year - 1970) + (year - 1) / 4 - 1969 / 4
        - ((year - 1) / 100 - 1969 / 100)
        + (year - 1) / 400 - 1969 / 400;
    for (i = 1; i < month; i++)
        days += month_lengths[i - 1] + (i == 2 && leap);
    days += day - 1;
    if ((days + 4) % 7 != weekday)
        return FALSE;
    expiry = days * 86400 + hour * 3600 + minute * 60 + second;
    return expiry > (uint64_t)now;
}

/* Commit fields only after a complete lease matches the live interface/address.
 * A last matching but unusable expiry clears older historical values. */
static void SelectLeaseForAddress(CONST_STRPTR data, size_t length,
    CONST_STRPTR interface, CONST_STRPTR address, time_t now, STRPTR gateway,
    ULONG gateway_size, STRPTR dns, ULONG dns_size)
{
    struct LeaseToken t, next, key, first, second, fields[3];
    size_t pos = 0, outer = 0, address_len;

    if (gateway_size) gateway[0] = '\0';
    if (dns_size) dns[0] = '\0';
    if (!data || !interface || !address)
        return;
    address_len = strlen((const char *)address);

    while (LeaseNextToken(data, length, &pos, &t))
    {
        if (t.kind == LT_BAD)
            return;
        if (t.kind == LT_OPEN) { outer++; continue; }
        if (t.kind == LT_CLOSE) { if (outer) outer--; continue; }
        if (outer || t.kind != LT_WORD || !LeaseTokenIs(&t, "lease"))
            continue;

        {
            size_t after_key = pos, depth = 1;
            BOOL valid = TRUE, iface_match = FALSE, ip_match = FALSE, closed = FALSE;
            BOOL saw_interface = FALSE, saw_address = FALSE;
            BOOL saw_expiry = FALSE, expiry_active = FALSE, duplicate_expiry = FALSE;
            char route[20] = "", servers[64] = "";

            if (!LeaseNextToken(data, length, &pos, &next))
                return;
            if (next.kind != LT_OPEN) { pos = after_key; continue; }

            while (LeaseNextToken(data, length, &pos, &t))
            {
                size_t count = 0, nested = 0;
                BOOL ended = FALSE, nested_stmt = FALSE;

                if (t.kind == LT_BAD)
                    return;
                if (t.kind == LT_OPEN) { depth++; continue; }
                if (t.kind == LT_CLOSE)
                {
                    if (--depth == 0) { closed = TRUE; break; }
                    continue;
                }
                if (depth != 1 || t.kind == LT_SEMI)
                    continue;

                key = t;
                while (LeaseNextToken(data, length, &pos, &next))
                {
                    if (next.kind == LT_BAD)
                        return;
                    if (next.kind == LT_OPEN) { nested++; nested_stmt = TRUE; }
                    else if (next.kind == LT_CLOSE)
                    {
                        if (nested) nested--;
                        else { valid = FALSE; depth = 0; closed = ended = TRUE; break; }
                    }
                    else if (!nested && next.kind == LT_SEMI) { ended = TRUE; break; }
                    else if (!nested && next.kind != LT_COMMA)
                    {
                        if (count < 3)
                            fields[count] = next;
                        if (count == 0) first = next;
                        else if (count == 1) second = next;
                        if (count < 4)
                            count++;
                    }
                }
                if (!ended)
                    return;
                if (!depth)
                    break;
                if (key.kind != LT_WORD) { valid = FALSE; continue; }

                if (LeaseTokenIs(&key, "interface"))
                {
                    if (saw_interface) valid = FALSE;
                    saw_interface = TRUE;
                    iface_match = !nested_stmt && count == 1 &&
                        LeaseTokenIs(&first, interface);
                }
                else if (LeaseTokenIs(&key, "fixed-address"))
                {
                    if (saw_address) valid = FALSE;
                    saw_address = TRUE;
                    ip_match = !nested_stmt && count == 1 &&
                        first.kind == LT_WORD && !first.escaped &&
                        first.length == address_len &&
                        memcmp(first.text, address, address_len) == 0;
                }
                else if (LeaseTokenIs(&key, "expire"))
                {
                    if (saw_expiry)
                        duplicate_expiry = TRUE;
                    saw_expiry = TRUE;
                    expiry_active = !nested_stmt && !duplicate_expiry &&
                        LeaseExpiryActive(fields, count, now);
                }
                else if (LeaseTokenIs(&key, "option") && count &&
                    first.kind == LT_WORD)
                {
                    STRPTR out = NULL;
                    ULONG out_size = 0;

                    if (LeaseTokenIs(&first, "routers"))
                    {
                        out = route;
                        out_size = sizeof(route);
                    }
                    else if (LeaseTokenIs(&first, "domain-name-servers"))
                    {
                        out = servers;
                        out_size = sizeof(servers);
                    }
                    if (out)
                    {
                        out[0] = '\0';
                        if (nested_stmt || count < 2 ||
                            !LeaseCopyToken(out, out_size, &second))
                            valid = FALSE;
                    }
                }
                if (nested_stmt &&
                    (LeaseTokenIs(&key, "interface") ||
                     LeaseTokenIs(&key, "fixed-address")))
                    valid = FALSE;
            }
            if (!closed)
                return;
            if (valid && iface_match && ip_match)
            {
                if (!saw_expiry || !expiry_active)
                {
                    route[0] = '\0';
                    servers[0] = '\0';
                }
                if (gateway_size)
                    snprintf((char *)gateway, gateway_size, "%s", route);
                if (dns_size)
                    snprintf((char *)dns, dns_size, "%s", servers);
            }
        }
    }
}

/* DateStamp uses DOS's initialized timer base. Unlike stdc time(), it does not
 * lazily open a timer request owned by this worker on a shared libc base.
 * Use the same epoch/timezone adjustment as stdc time() and dhclient. */
static time_t LeaseCurrentTime(void)
{
    struct DateStamp stamp;
    int64_t seconds;
    time_t now;

    if (DateStamp(&stamp) == NULL || stamp.ds_Days < 0 ||
        (stamp.ds_Days == 0 && stamp.ds_Minute == 0 && stamp.ds_Tick == 0) ||
        stamp.ds_Minute < 0 || stamp.ds_Minute >= 1440 ||
        stamp.ds_Tick < 0 || stamp.ds_Tick >= 3000)
        return (time_t)-1;
    seconds = ((int64_t)stamp.ds_Days + 2922) * 86400 +
        stamp.ds_Minute * 60 + stamp.ds_Tick / 50 +
        (int64_t)__stdc_gmtoffset() * 60;
    now = (time_t)seconds;
    return seconds >= 0 && (int64_t)now == seconds ? now : (time_t)-1;
}

/* Exact interface token; GW= stays case-insensitive as in the stack grammar. */
static BOOL InterfaceGatewayFromPrefs(CONST_STRPTR data, size_t length,
    CONST_STRPTR interface, STRPTR gateway, ULONG size)
{
    size_t line = 0, interface_len;

    if (size) gateway[0] = '\0';
    if (!size || !interface)
        return FALSE;
    interface_len = strlen((const char *)interface);
    while (line < length)
    {
        size_t physical = line, end = line, p, start, limit;
        BOOL continued, match = FALSE;

        while (end < length && data[end] != '\n') end++;
        continued = end > physical && data[end - 1] == '+';
        limit = end - (continued ? 1 : 0);
        p = physical;
        while (p < limit && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r')) p++;
        if (p < limit && data[p] != '#' && data[p] != ';')
        {
            start = p;
            while (p < limit && data[p] != ' ' && data[p] != '\t' &&
                data[p] != '\r' && data[p] != '#' && data[p] != ';') p++;
            match = p - start == interface_len &&
                memcmp(data + start, interface, interface_len) == 0;
        }

        for (;;)
        {
            if (match)
            {
                while (p < limit)
                {
                    size_t value, stop;

                    while (p < limit && (data[p] == ' ' || data[p] == '\t' ||
                        data[p] == '\r')) p++;
                    if (p == limit || data[p] == '#' || data[p] == ';') break;
                    start = p;
                    while (p < limit && data[p] != ' ' && data[p] != '\t' &&
                        data[p] != '\r' && data[p] != '#' && data[p] != ';') p++;
                    if (p - start <= 3 || ToUpper(data[start]) != 'G' ||
                        ToUpper(data[start + 1]) != 'W' || data[start + 2] != '=')
                        continue;
                    value = start + 3;
                    stop = value;
                    while (stop < p && data[stop] != ',') stop++;
                    if (stop == value) continue;
                    if (stop - value >= size) stop = value + size - 1;
                    memcpy(gateway, data + value, stop - value);
                    gateway[stop - value] = '\0';
                    return TRUE;
                }
            }
            if (!continued) break;
            physical = end + 1;
            if (physical >= length) { end = physical; break; }
            end = physical;
            while (end < length && data[end] != '\n') end++;
            continued = end > physical && data[end - 1] == '+';
            limit = end - (continued ? 1 : 0);
            p = physical;
            while (p < limit && (data[p] == ' ' || data[p] == '\t' ||
                data[p] == '\r')) p++;
        }
        line = end < length ? end + 1 : end;
    }
    return FALSE;
}
/*
 * Gateway and name servers do not come back from an ioctl. The lease file is
 * historical client data, not proof of the route or resolver's current state,
 * so use only its last complete record for this interface and current address.
 * Fall back to the configured preferences database for static configuration.
 */
/* Match dhclient's PATH_DHCLIENT_DB override and POSIX /T default. The
 * preference reader uses raw DOS paths; it cannot interpret /T as T:.
 * This application links posixc for this read-only lease access. */
static STRPTR ReadLeasePrefs(LONG *lenp)
{
    const char *path = getenv("PATH_DHCLIENT_DB");
    FILE *file;
    long size;
    STRPTR buf = NULL;

    *lenp = 0;
    file = fopen(path != NULL ? path : "/T/dhclient.leases", "rb");
    if (file == NULL)
        return NULL;

    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 &&
        size < 0x7fffffff && fseek(file, 0, SEEK_SET) == 0 &&
        (buf = AllocVec((ULONG)size + 1, MEMF_ANY | MEMF_CLEAR)) != NULL)
    {
        if (fread(buf, 1, (size_t)size, file) == (size_t)size && !ferror(file))
            *lenp = (LONG)size;
        else
        {
            WifiClearSecret(buf, (size_t)size + 1);
            FreeVec(buf);
            buf = NULL;
        }
    }
    if (fclose(file) != 0 && buf != NULL)
    {
        WifiClearSecret(buf, (size_t)*lenp + 1);
        FreeVec(buf);
        buf = NULL;
        *lenp = 0;
    }
    return buf;
}

static void FillRouteAndDNS(struct WifiMsg *m)
{
    LONG len;
    STRPTR buf, opt;
    TEXT db[192], path[224];

    m->wm_Gateway[0] = '\0';
    m->wm_DNS[0] = '\0';

    buf = ReadLeasePrefs(&len);
    if (buf != NULL)
    {
        SelectLeaseForAddress(buf, (size_t)len, m->wm_Interface,
            m->wm_Address, LeaseCurrentTime(), m->wm_Gateway, sizeof(m->wm_Gateway),
            m->wm_DNS, sizeof(m->wm_DNS));
        FreeVec(buf);
    }

    if (m->wm_Gateway[0] == '\0' && StackDBDir(db, sizeof(db)))
    {
        snprintf((char *)path, sizeof(path), "%s/interfaces", (const char *)db);
        buf = ReadPrefs(path, &len);
        if (buf != NULL)
        {
            InterfaceGatewayFromPrefs(buf, (size_t)len, m->wm_Interface,
                m->wm_Gateway, sizeof(m->wm_Gateway));
            FreeVec(buf);
        }
    }

    if (m->wm_DNS[0] == '\0' && StackDBDir(db, sizeof(db)))
    {
        snprintf((char *)path, sizeof(path), "%s/netdb-myhost", (const char *)db);
        buf = ReadPrefs(path, &len);
        if (buf != NULL)
        {
            opt = FindNoCase(buf, "NAMESERVER ");
            if (opt != NULL)
                CopyField(m->wm_DNS, sizeof(m->wm_DNS), opt + 11);
            FreeVec(buf);
        }
    }
}

/* What the stack has on the interface behind our device, if anything. */
static void FillAddress(struct WifiMsg *m)
{
    int sock;

    m->wm_Address[0] = '\0';
    m->wm_Netmask[0] = '\0';
    m->wm_Broadcast[0] = '\0';
    m->wm_Interface[0] = '\0';
    m->wm_Gateway[0] = '\0';
    m->wm_DNS[0] = '\0';

    /* The name comes from the preferences, so it is known even with the stack
     * down - and telling those two cases apart is the whole point. */
    if (!InterfaceName(m->wm_Device, m->wm_Interface, sizeof(m->wm_Interface)))
        return;
    if (!StackRunning())
        return;

    SocketBase = OpenLibrary("bsdsocket.library", 0);
    if (SocketBase == NULL)
        return;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0)
    {
        AskIface(sock, m->wm_Interface, SIOCGIFADDR,
            m->wm_Address, sizeof(m->wm_Address));
        AskIface(sock, m->wm_Interface, SIOCGIFNETMASK,
            m->wm_Netmask, sizeof(m->wm_Netmask));
        AskIface(sock, m->wm_Interface, SIOCGIFBRDADDR,
            m->wm_Broadcast, sizeof(m->wm_Broadcast));
        CloseSocket(sock);
    }

    CloseLibrary(SocketBase);
    SocketBase = NULL;

    if (m->wm_Address[0] != '\0')
        FillRouteAndDNS(m);
}

/*
 * A driver is wireless if it answers NSCMD_DEVICEQUERY with S2_GETNETWORKS in
 * its command list - the same test wpa_supplicant uses to tell a FullMAC radio
 * from an ethernet card. There is no wire type to go by. Asked on an already
 * open device.
 */
static BOOL ProbeWireless(struct WifiDev *dev)
{
    struct NSDeviceQueryResult nsq;
    BOOL wireless = FALSE;

    memset(&nsq, 0, sizeof(nsq));
    ((struct IOStdReq *)dev->wd_Req)->io_Command = NSCMD_DEVICEQUERY;
    ((struct IOStdReq *)dev->wd_Req)->io_Data = &nsq;
    ((struct IOStdReq *)dev->wd_Req)->io_Length = sizeof(nsq);

    if (DoIO((struct IORequest *)dev->wd_Req) == 0 &&
        nsq.SupportedCommands != NULL)
    {
        const UWORD *cmd;

        for (cmd = nsq.SupportedCommands; *cmd != 0; cmd++)
        {
            if (*cmd == S2_GETNETWORKS)
            {
                wireless = TRUE;
                break;
            }
        }
    }

    return wireless;
}

/* The answer cannot change while the machine is running, and a status request
 * happens up to forty times during one connect. */
static BOOL DeviceIsWireless(struct WifiDev *dev, CONST_STRPTR name)
{
    static TEXT probed[WIFI_DEV_MAX];
    static BOOL probedresult;

    if (probed[0] != '\0' && Stricmp((STRPTR)probed, (STRPTR)name) == 0)
        return probedresult;

    probedresult = ProbeWireless(dev);
    snprintf(probed, sizeof(probed), "%s", name);
    return probedresult;
}

/*
 * Only the radios. DEVS:Networks also holds drivers for hardware that is not
 * in this machine, and the only way to tell one apart is to open it and ask -
 * a driver whose hardware is absent fails the open, which is the answer.
 */
static void DoDevices(struct WifiMsg *m)
{
    struct FileInfoBlock *fib;
    BPTR lock;

    m->wm_DeviceCount = 0;

    lock = Lock("DEVS:Networks", SHARED_LOCK);
    if (lock == BNULL)
    {
        strcpy(m->wm_Status, (const char *)_(MSG_ST_NO_DEVS_NETWORKS));
        return;
    }

    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib != NULL && Examine(lock, fib))
    {
        while (ExNext(lock, fib) && m->wm_DeviceCount < WIFI_MAX_DEVS)
        {
            STRPTR ext = (STRPTR)strrchr((const char *)fib->fib_FileName, '.');
            TEXT name[WIFI_DEV_MAX];
            LONG unit;

            if (fib->fib_DirEntryType > 0)
                continue;
            if (ext == NULL || Stricmp(ext, ".device") != 0)
                continue;

            snprintf(name, sizeof(name), "DEVS:Networks/%s", fib->fib_FileName);

            /* One driver can hold more than one radio, so walk its units until
             * one refuses to open. Capped, because a driver that ignores the
             * unit number would otherwise fill the chooser by itself. */
            for (unit = 0; unit < WIFI_MAX_UNITS &&
                 m->wm_DeviceCount < WIFI_MAX_DEVS; unit++)
            {
                struct WifiDev dev;
                BOOL wireless;

                if (!OpenWifiDev(&dev, name, unit))
                    break;
                wireless = ProbeWireless(&dev);
                CloseWifiDev(&dev);
                if (!wireless)
                    break;      /* wireless is a property of the driver */

                snprintf(m->wm_Devices[m->wm_DeviceCount], WIFI_DEV_MAX, "%s",
                    name);
                m->wm_DeviceUnits[m->wm_DeviceCount] = unit;
                m->wm_DeviceCount++;
            }
        }
    }
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);

    if (m->wm_DeviceCount == 0)
        strcpy(m->wm_Status, (const char *)_(MSG_ST_NO_WIRELESS_DEVICE));
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */

/* A startup OpenDevice probe can fail transiently. A later successful scan
 * proves this exact device/unit implements S2_GETNETWORKS; repair the chooser
 * from that observation, not from its filename or association capabilities. */
static void RememberScannedDevice(struct WifiMsg *m)
{
    LONG i;

    if (m->wm_Device[0] == '\0' || m->wm_Unit < 0 ||
        m->wm_DeviceCount < 0 || m->wm_DeviceCount > WIFI_MAX_DEVS)
        return;
    for (i = 0; i < m->wm_DeviceCount; i++)
        if (Stricmp(m->wm_Devices[i], m->wm_Device) == 0 &&
            m->wm_DeviceUnits[i] == m->wm_Unit)
            return;
    if (i == WIFI_MAX_DEVS)
        return;
    snprintf(m->wm_Devices[i], WIFI_DEV_MAX, "%s", m->wm_Device);
    m->wm_DeviceUnits[i] = m->wm_Unit;
    m->wm_DeviceCount++;
}

/* Fills in wm_Associated/wm_SSID. Returns FALSE only if the device is absent. */
static BOOL DoStatus(struct WifiMsg *m)
{
    struct WifiDev dev;
    APTR pool;

    m->wm_Associated = FALSE;
    m->wm_Wireless = FALSE;
    m->wm_SSID[0] = '\0';
    m->wm_BSSID[0] = '\0';
    m->wm_MAC[0] = '\0';

    if (!WifiDevice(m))
        return FALSE;
    if (!OpenWifiDev(&dev, m->wm_Device, m->wm_Unit))
    {
        snprintf(m->wm_Status, sizeof(m->wm_Status), (const char *)_(MSG_ST_CANNOT_OPEN),
            m->wm_Device, (long)m->wm_Unit);
        return FALSE;
    }

    m->wm_Wireless = DeviceIsWireless(&dev, m->wm_Device);

    pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
    if (pool != NULL)
    {
        dev.wd_Req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
        if (DoIO((struct IORequest *)dev.wd_Req) == 0)
        {
            const UBYTE *a = dev.wd_Req->ios2_SrcAddr;

            snprintf(m->wm_MAC, sizeof(m->wm_MAC),
                "%02x:%02x:%02x:%02x:%02x:%02x",
                a[0], a[1], a[2], a[3], a[4], a[5]);
        }

        dev.wd_Req->ios2_Req.io_Command = S2_GETNETWORKINFO;
        dev.wd_Req->ios2_Data = pool;
        dev.wd_Req->ios2_StatData = NULL;
        if (DoIO((struct IORequest *)dev.wd_Req) == 0 &&
            dev.wd_Req->ios2_StatData != NULL)
        {
            STRPTR ssid = (STRPTR)GetTagData(S2INFO_SSID, (IPTR)NULL,
                dev.wd_Req->ios2_StatData);
            UBYTE *bssid = (UBYTE *)GetTagData(S2INFO_BSSID, (IPTR)NULL,
                dev.wd_Req->ios2_StatData);

            m->wm_Associated = TRUE;
            if (ssid != NULL)
                snprintf(m->wm_SSID, sizeof(m->wm_SSID), "%s", ssid);
            if (bssid != NULL)
                snprintf(m->wm_BSSID, sizeof(m->wm_BSSID),
                    "%02x:%02x:%02x:%02x:%02x:%02x",
                    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        }
        DeletePool(pool);
    }

    CloseWifiDev(&dev);

    if (m->wm_Associated)
    {
        FillAddress(m);
        if (m->wm_Address[0] != '\0')
            snprintf(m->wm_Status, sizeof(m->wm_Status),
                (const char *)_(MSG_ST_CONNECTED),
                (const char *)(m->wm_SSID[0] != '\0' ? (CONST_STRPTR)m->wm_SSID
                                                     : _(MSG_ST_A_NETWORK)),
                m->wm_Address);
        else if (m->wm_Interface[0] == '\0')
            snprintf(m->wm_Status, sizeof(m->wm_Status),
                (const char *)_(MSG_ST_CONNECTED_NO_IFACE_PREFS), m->wm_SSID,
                m->wm_Device);
        else if (!StackRunning())
            snprintf(m->wm_Status, sizeof(m->wm_Status),
                (const char *)_(MSG_ST_CONNECTED_NO_STACK), m->wm_SSID);
        else
            snprintf(m->wm_Status, sizeof(m->wm_Status),
                (const char *)_(MSG_ST_CONNECTED_WAITING),
                (const char *)(m->wm_SSID[0] != '\0' ? (CONST_STRPTR)m->wm_SSID
                                                     : _(MSG_ST_A_NETWORK)));
    }
    else if (!m->wm_Wireless)
        snprintf(m->wm_Status, sizeof(m->wm_Status),
            (const char *)_(MSG_ST_NOT_WIRELESS), m->wm_Device);
    else
        strcpy(m->wm_Status, (const char *)_(MSG_ST_NOT_CONNECTED));

    return TRUE;
}

static void DoScan(struct WifiMsg *m)
{
    struct WifiDev dev;
    APTR pool;
    struct Sana2WirelessCapabilities caps;
    BOOL summary_capable;

    m->wm_Count = 0;

    /* So the window can tick the network we are on while listing the rest. */
    DoStatus(m);

    if (!WifiDevice(m))
        return;
    if (!OpenWifiDev(&dev, m->wm_Device, m->wm_Unit))
    {
        snprintf(m->wm_Status, sizeof(m->wm_Status), (const char *)_(MSG_ST_CANNOT_OPEN),
            m->wm_Device, (long)m->wm_Unit);
        return;
    }

    pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE * 8, PUDDLE_SIZE);
    if (pool == NULL)
    {
        strcpy(m->wm_Status, (const char *)_(MSG_ST_OUT_OF_MEMORY));
        CloseWifiDev(&dev);
        return;
    }

    /* Negotiate the optional per-device display metadata contract. Legacy
     * devices keep their authentic-IE path; this grants no handshake policy. */
    memset(&caps, 0, sizeof(caps));
    caps.SizeAvailable = sizeof(caps);
    caps.Version = S2WIRELESS_CAPABILITIES_VERSION;
    dev.wd_Req->ios2_Req.io_Command = S2_GETWIRELESSCAPABILITIES;
    dev.wd_Req->ios2_StatData = &caps;
    dev.wd_Req->ios2_Data = NULL;
    dev.wd_Req->ios2_DataLength = 0;
    summary_capable = DoIO((struct IORequest *)dev.wd_Req) == 0 &&
        dev.wd_Req->ios2_Req.io_Error == 0 && WifiSummaryCapability(&caps);

    dev.wd_Req->ios2_Req.io_Command = S2_GETNETWORKS;
    dev.wd_Req->ios2_Data = pool;
    dev.wd_Req->ios2_StatData = NULL;
    dev.wd_Req->ios2_DataLength = 0;

    if (DoIO((struct IORequest *)dev.wd_Req) == 0)
    {
        struct TagItem **lists = dev.wd_Req->ios2_StatData;
        ULONG count = dev.wd_Req->ios2_DataLength, i;
        LONG j;

        RememberScannedDevice(m);

        for (i = 0; i < count && i < WIFI_MAX_NETS && lists != NULL; i++)
        {
            struct TagItem *tl = lists[i];
            struct WifiNet *n = &m->wm_Nets[m->wm_Count];
            STRPTR ssid;

            if (tl == NULL)
                continue;

            ssid = (STRPTR)GetTagData(S2INFO_SSID, (IPTR)"", tl);
            if (ssid[0] == '\0')
                continue;               /* hidden: nothing to click on */

            /*
             * A scan reports one entry per BSS, so a dual-band access point -
             * or a mesh - answers several times under the same name. Keep the
             * strongest of each name: the list is meant to be a list of
             * networks, not of radios.
             */
            for (j = 0; j < m->wm_Count; j++)
            {
                if (strcmp((const char *)m->wm_Nets[j].wn_SSID,
                           (const char *)ssid) == 0)
                    break;
            }
            if (j < m->wm_Count)
            {
                if ((LONG)GetTagData(S2INFO_Signal, 0, tl) <=
                    m->wm_Nets[j].wn_Signal)
                    continue;           /* the one we have is better */
                n = &m->wm_Nets[j];     /* replace it in place */
            }

            snprintf(n->wn_SSID, sizeof(n->wn_SSID), "%s", ssid);
            n->wn_Signal = (LONG)GetTagData(S2INFO_Signal, 0, tl);
            n->wn_Channel = (LONG)GetTagData(S2INFO_Channel, 0, tl);
            snprintf(n->wn_Protection, sizeof(n->wn_Protection), "%s",
                summary_capable ? WifiSummaryProtection(
                    (const struct Sana2WirelessSecuritySummary *)
                    GetTagData(S2INFO_SecuritySummary, (IPTR)NULL, tl)) :
                    (const char *)IEProtection((const UBYTE *)
                        GetTagData(S2INFO_InfoElements, (IPTR)NULL, tl)));
            n->wn_Known = PrefsHaveNetwork(n->wn_SSID,
                WifiSecurityProfileFromLabel(
                    (CONST_STRPTR)n->wn_Protection));
            if (j == m->wm_Count)
                m->wm_Count++;
        }
        /* Strongest first: that is the order they are likely wanted in. */
        for (j = 0; j < m->wm_Count - 1; j++)
        {
            LONG k;

            for (k = 0; k < m->wm_Count - 1 - j; k++)
            {
                if (m->wm_Nets[k].wn_Signal < m->wm_Nets[k + 1].wn_Signal)
                {
                    struct WifiNet t = m->wm_Nets[k];

                    m->wm_Nets[k] = m->wm_Nets[k + 1];
                    m->wm_Nets[k + 1] = t;
                }
            }
        }

        if (m->wm_Count == 1)
            strcpy(m->wm_Status, (const char *)_(MSG_ST_ONE_NETWORK));
        else
            snprintf(m->wm_Status, sizeof(m->wm_Status),
                (const char *)_(MSG_ST_NETWORKS), (long)m->wm_Count);
    }
    else
        strcpy(m->wm_Status, (const char *)_(MSG_ST_SCAN_FAILED));

    DeletePool(pool);
    CloseWifiDev(&dev);
}

/*
 * A failed attempt should leave the stored configuration exactly as it was:
 * the supplicant reads the passphrase from the file, so trying one means
 * writing it, and a typo would otherwise overwrite a passphrase that worked.
 */
struct PrefsBackup
{
    STRPTR  pb_Env;
    LONG    pb_EnvLen;
    STRPTR  pb_Envarc;
    LONG    pb_EnvarcLen;
    BOOL    pb_Taken;
};

static BOOL SavePrefs(struct PrefsBackup *b)
{
    BOOL env_ok, envarc_ok = TRUE;
    b->pb_Env = ReadPrefsChecked(PREFS_ENV, &b->pb_EnvLen, &env_ok);
    if (!WifiVolatile)
        b->pb_Envarc = ReadPrefsChecked(PREFS_ENVARC, &b->pb_EnvarcLen,
                                      &envarc_ok);
    b->pb_Taken = env_ok && envarc_ok;
    return b->pb_Taken;
}

static BOOL WriteWhole(CONST_STRPTR name, STRPTR buf, LONG len)
{
    BPTR fh;
    BOOL result;

    /* No file before the attempt means the network was unknown, and unknown is
     * what it has to go back to - or the next Connect trusts a bad key. */
    if (buf == NULL)
    {
        return DeleteFile(name) || IoErr() == ERROR_OBJECT_NOT_FOUND;
    }
    if ((fh = Open(name, MODE_NEWFILE)) == BNULL)
        return FALSE;
    result = Write(fh, buf, len) == len;
    if (!Close(fh))
        result = FALSE;
    return result;
}

static BOOL RestorePrefs(struct PrefsBackup *b)
{
    BOOL result;
    if (!b->pb_Taken)
        return TRUE;
    result = WriteWhole(PREFS_ENV, b->pb_Env, b->pb_EnvLen);
    if (!WifiVolatile)
        if (!WriteWhole(PREFS_ENVARC, b->pb_Envarc, b->pb_EnvarcLen))
            result = FALSE;
    return result;
}

static void FreePrefsBackup(struct PrefsBackup *b)
{
    WifiClearSecret(b->pb_Env, b->pb_EnvLen);
    WifiClearSecret(b->pb_Envarc, b->pb_EnvarcLen);
    FreeVec(b->pb_Env);
    FreeVec(b->pb_Envarc);
    b->pb_Env = NULL;
    b->pb_Envarc = NULL;
    b->pb_Taken = FALSE;
}

/*
 * Was this network on the air the last time we looked? What is not there
 * cannot be blamed on the passphrase, and the answer is already in the scan
 * results the window is showing - no need to spend seconds scanning again.
 */
static BOOL SSIDInScan(struct WifiMsg *m)
{
    LONG i;

    for (i = 0; i < m->wm_Count; i++)
    {
        if (strcmp((const char *)m->wm_Nets[i].wn_SSID,
                   (const char *)m->wm_SSID) == 0)
            return TRUE;
    }
    return FALSE;
}

/*
 * The attempt itself, from starting the supplicant to having an address.
 * WRES_NEEDKEY on the way out means the passphrase is what to suspect. The
 * name is passed in because DoStatus() clears wm_SSID whenever the device is
 * not associated, which is every case worth reporting.
 */
static void RunConnect(struct WifiMsg *m, CONST_STRPTR wanted,
    BOOL inrange)
{
    int tries;

    if (!StopSupplicant())
    {
        strcpy(m->wm_Status, (const char *)_(MSG_ST_WM_NO_STOP));
        return;
    }
    if (!StartSupplicant(m->wm_Device, m->wm_Unit))
    {
        strcpy(m->wm_Status, (const char *)_(MSG_ST_WM_NO_START));
        return;
    }

    /* The supplicant now scans, associates and runs the handshake; watch the
     * driver until it reports an association. */
    for (tries = 0; tries < ASSOC_TIMEOUT; tries++)
    {
        Delay(25);
        if (DoStatus(m) && m->wm_Associated)
        {
            BOOL created;

            /* VOLATILE commits a new selection only after an address exists. */
            if (!WifiVolatile)
                m->wm_Result = WRES_OK;

            if (!EnsureInterface(m, &created))
            {
                snprintf(m->wm_Status, sizeof(m->wm_Status),
                    (const char *)_(MSG_ST_CONNECTED_NO_IFACE), wanted,
                    m->wm_Device);
                return;
            }

            /* A running stack read the interfaces file when it started, so a
             * line added now means nothing to it until it is restarted. */
            if (created && StackRunning())
            {
                snprintf(m->wm_Status, sizeof(m->wm_Status),
                    (const char *)_(MSG_ST_RESTART_STACK), wanted,
                    m->wm_Interface);
                return;
            }

            if (!StartStack())
            {
                snprintf(m->wm_Status, sizeof(m->wm_Status),
                    (const char *)_(MSG_ST_STACK_NO_START), wanted);
                return;
            }

            /* Associating is quicker than DHCP, so give the lease a moment to
             * arrive - the address is the answer the user is waiting for. */
            for (tries = 0; tries < ASSOC_TIMEOUT; tries++)
            {
                if (m->wm_Address[0] != '\0')
                {
                    m->wm_Result = WRES_OK;
                    return;
                }
                Delay(25);
                DoStatus(m);

                /*
                 * WPA2 lets anybody associate and only then runs the
                 * handshake, so being thrown off again seconds later is what
                 * a wrong passphrase looks like from out here.
                 */
                if (!m->wm_Associated && m->wm_Protected)
                {
                    m->wm_Result = WRES_NEEDKEY;
                    snprintf(m->wm_Status, sizeof(m->wm_Status),
                        (const char *)_(MSG_ST_WRONG_PASSWORD), wanted);
                    return;
                }
            }
            if (WifiVolatile && m->wm_Associated &&
                m->wm_Address[0] != '\0')
                m->wm_Result = WRES_OK;
            return;
        }
    }

    m->wm_Result = WRES_FAILED;

    /*
     * Never associating at all is not the passphrase's fault either - unless
     * the network was there when we scanned, in which case the handshake
     * failing before we ever caught it associated is the likely story.
     */
    if (m->wm_Protected && inrange)
    {
        m->wm_Result = WRES_NEEDKEY;
        snprintf(m->wm_Status, sizeof(m->wm_Status),
            (const char *)_(MSG_ST_WRONG_PASSWORD), wanted);
        return;
    }

    snprintf(m->wm_Status, sizeof(m->wm_Status),
        (const char *)_(MSG_ST_COULD_NOT_CONNECT), wanted);
}

static void DoConnect(struct WifiMsg *m)
{
    struct PrefsBackup backup = { NULL, 0, NULL, 0, FALSE };
    TEXT wanted[WIFI_SSID_MAX];
    BOOL inrange;

    m->wm_Result = WRES_FAILED;

    /* Fail closed before opening a device, touching credentials, or
     * restarting WirelessManager when the scan's label was not exact. */
    if (m->wm_SecurityProfile != WIFI_SECURITY_OPEN &&
        m->wm_SecurityProfile != WIFI_SECURITY_PSK &&
        m->wm_SecurityProfile != WIFI_SECURITY_SAE)
    {
        strcpy(m->wm_Status,
            "Cannot connect: security type is unknown.");
        return;
    }
    m->wm_Protected = m->wm_SecurityProfile != WIFI_SECURITY_OPEN;

    if ((m->wm_Protected && m->wm_Key[0] != '\0' &&
         !WifiValidateProfileCredential(m->wm_SecurityProfile,
             (CONST_STRPTR)m->wm_Key)) ||
        (!m->wm_Protected && m->wm_Key[0] != '\0'))
    {
        strcpy(m->wm_Status,
            "Password must be printable ASCII and within the security limit.");
        return;
    }

    if (!WifiDevice(m))
        return;
    if (m->wm_SSID[0] == '\0')
    {
        strcpy(m->wm_Status, (const char *)_(MSG_ST_NO_NETWORK_SELECTED));
        return;
    }

    /* With scan results in hand, a network missing from them is worth saying
     * so about instead of spending half a minute failing to associate. */
    snprintf((char *)wanted, sizeof(wanted), "%s", (const char *)m->wm_SSID);
    inrange = SSIDInScan(m);
    if (m->wm_Count > 0 && !inrange)
    {
        snprintf(m->wm_Status, sizeof(m->wm_Status),
            (const char *)_(MSG_ST_NOT_IN_RANGE), m->wm_SSID);
        return;
    }

    /*
     * A protected network needs a passphrase, but not if we already stored one
     * for it - that is what "remembered" means. Asking the window for it is a
     * separate round trip.
     */
    if (m->wm_Protected && m->wm_Key[0] == '\0' &&
        !PrefsHaveNetwork(m->wm_SSID, m->wm_SecurityProfile))
    {
        m->wm_Result = WRES_NEEDKEY;
        snprintf(m->wm_Status, sizeof(m->wm_Status),
            (const char *)_(MSG_ST_ENTER_PASSWORD), m->wm_SSID);
        return;
    }

    /* Only rewrite the stored configuration when we were given something new
     * to put in it, so a reconnect to a remembered network keeps its key. */
    if (!m->wm_Protected || m->wm_Key[0] != '\0')
    {
        if (!SavePrefs(&backup))
        {
            strcpy(m->wm_Status, (const char *)_(MSG_ST_CANNOT_WRITE_PREFS));
            FreePrefsBackup(&backup);
            return;
        }
        if ((WifiVolatile &&
             (m->wm_SecurityProfile == WIFI_SECURITY_SAE
                ? !WifiWriteVolatileProfilePrefs(m->wm_SSID, m->wm_Key,
                      m->wm_SecurityProfile)
                : !WifiWriteVolatilePrefs(m->wm_SSID, m->wm_Key,
                      m->wm_Protected))) ||
            (!WifiVolatile &&
             (!MergePrefs(PREFS_ENVARC, m->wm_SSID, m->wm_Key,
                  m->wm_SecurityProfile) ||
              !MergePrefs(PREFS_ENV, m->wm_SSID, m->wm_Key,
                  m->wm_SecurityProfile))))
        {
            strcpy(m->wm_Status, (const char *)_(MSG_ST_CANNOT_WRITE_PREFS));
            RestorePrefs(&backup);
            FreePrefsBackup(&backup);
            return;
        }
    }

    RunConnect(m, wanted, inrange);

    /* Blaming the passphrase and keeping it are contradictory: put the file
     * back, so an untested passphrase never becomes the remembered one. */
    if (m->wm_Result == WRES_NEEDKEY ||
        (WifiVolatile && m->wm_Result != WRES_OK))
    {
        if (!RestorePrefs(&backup))
        {
            m->wm_Result = WRES_FAILED;
            strcpy(m->wm_Status, (const char *)_(MSG_ST_CANNOT_WRITE_PREFS));
        }
    }
    FreePrefsBackup(&backup);
}

/*
 * Leaving means two things: stop the supplicant, or it associates again within
 * seconds, and tell the firmware to drop the link.
 */
static void DoDisconnect(struct WifiMsg *m)
{
    struct WifiDev dev;
    struct TagItem tags[2];

    if (!WifiDevice(m))
        return;

    StopSupplicant();

    if (!OpenWifiDev(&dev, m->wm_Device, m->wm_Unit))
    {
        snprintf(m->wm_Status, sizeof(m->wm_Status), (const char *)_(MSG_ST_CANNOT_OPEN),
            m->wm_Device, (long)m->wm_Unit);
        return;
    }

    tags[0].ti_Tag = S2INFO_Disassociate;
    tags[0].ti_Data = TRUE;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;

    dev.wd_Req->ios2_Req.io_Command = S2_SETOPTIONS;
    dev.wd_Req->ios2_Data = tags;
    DoIO((struct IORequest *)dev.wd_Req);
    CloseWifiDev(&dev);

    DoStatus(m);
    if (!m->wm_Associated)
        strcpy(m->wm_Status, (const char *)_(MSG_ST_DISCONNECTED));
}

/* ------------------------------------------------------------------------- */

VOID WifiWorker(VOID)
{
    struct MsgPort *port = CreateMsgPort();
    struct WifiMsg *m;
    BOOL done = FALSE;

    WifiWorkerPort = port;
    Signal(WifiMainTask, SIGF_SINGLE);       /* port is up (or NULL on failure) */
    if (port == NULL)
        return;

    while (!done)
    {
        WaitPort(port);
        while ((m = (struct WifiMsg *)GetMsg(port)) != NULL)
        {
            m->wm_Result = WRES_OK;
            m->wm_Status[0] = '\0';

            switch (m->wm_Cmd)
            {
            case WCMD_DEVICES:
                DoDevices(m);
                break;

            case WCMD_STATUS:
                DoStatus(m);
                break;

            case WCMD_SCAN:
                DoScan(m);
                break;

            case WCMD_CONNECT:
                DoConnect(m);
                WifiClearSecret(m->wm_Key, sizeof(m->wm_Key));
                break;

            case WCMD_DISCONNECT:
                DoDisconnect(m);
                break;

            case WCMD_QUIT:
                done = TRUE;
                break;
            }

            ReplyMsg(&m->wm_Msg);
        }
    }

    /* The window waits for this before it frees the port's last message. */
    DeleteMsgPort(port);
    WifiWorkerPort = NULL;
    Signal(WifiMainTask, SIGF_SINGLE);
}
