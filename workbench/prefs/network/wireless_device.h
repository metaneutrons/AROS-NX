#ifndef NETPREFS_WIRELESS_DEVICE_H
#define NETPREFS_WIRELESS_DEVICE_H

#include <exec/types.h>

enum NetPrefsWirelessKind
{
    NETPREFS_WIRELESS_UNKNOWN,
    NETPREFS_WIRELESS_NO,
    NETPREFS_WIRELESS_YES
};

/* Capability query only: never scan, configure or put the interface online.
 * An unavailable/unqueryable device is UNKNOWN, not a wired device. */
enum NetPrefsWirelessKind NetPrefs_QueryWireless(CONST_STRPTR device, LONG unit);

#endif
