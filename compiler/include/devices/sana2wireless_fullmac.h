/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef DEVICES_SANA2WIRELESS_FULLMAC_H
#define DEVICES_SANA2WIRELESS_FULLMAC_H

#include <devices/sana2wireless.h>

/* Shared predicates only: a valid response is not hardware qualification.
 * All callers still negotiate with the actual opened device and apply their
 * explicit diagnostic policy. Providers revalidate their live state/profile
 * before accepting an operation; this predicate does not hold an epoch. */
static __inline__ int Sana2FullMACCapabilitiesValid(
    const struct Sana2FullMACCapabilities *caps)
{
    const ULONG flags = S2FULLMAC_CAPF_DIAGNOSTIC_RISK;
    const ULONG profiles = S2FULLMAC_PROFILE_OPEN |
                           S2FULLMAC_PROFILE_WPA2_CCMP |
                           S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    const ULONG pwe = S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
                      S2FULLMAC_SAE_PWE_H2E | S2FULLMAC_SAE_PWE_BOTH;

    if (!caps || caps->SizeAvailable != (ULONG)sizeof(*caps) ||
        caps->SizeSupplied != (ULONG)sizeof(*caps) ||
        caps->Version != S2FULLMAC_VERSION || (caps->Flags & ~flags) ||
        !caps->Profiles || (caps->Profiles & ~profiles) ||
        (caps->SAEPweMethods & ~pwe) || caps->Reserved[0] ||
        caps->Reserved[1])
        return 0;
    if (caps->Profiles & S2FULLMAC_PROFILE_SAE_CCMP_PMF)
        return caps->SAEPweMethods != 0;
    return caps->SAEPweMethods == 0;
}

/* The object and declared Data extent must be readable throughout this call.
 * Headers/length/kind are checked before reading any credential byte. No
 * string scan, derived key, allocation, transport or secret output occurs.
 * This checks the typed contract, not scan matching or firmware completion. */
static __inline__ int Sana2FullMACAssociationValid(
    const struct Sana2FullMACCapabilities *caps,
    const struct Sana2FullMACAssociation *selection,
    const struct Sana2WirelessCredential *credential)
{
    ULONG i, profile;
    if (!Sana2FullMACCapabilitiesValid(caps) || !selection ||
        selection->Size != (ULONG)sizeof(*selection) ||
        selection->Version != S2FULLMAC_VERSION || selection->Reserved ||
        (selection->Flags & ~S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK))
        return 0;
    profile = selection->Profile;
    if (!profile || (profile & (profile - 1)) ||
        (profile & caps->Profiles) != profile)
        return 0;
    if ((caps->Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) &&
        !(selection->Flags & S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK))
        return 0;

    if (profile == S2FULLMAC_PROFILE_OPEN)
        return !credential && selection->PMFPolicy == S2FULLMAC_PMF_DISABLED &&
               selection->SAEPweMethod == 0;

    if (!credential || credential->Size != (ULONG)sizeof(*credential) ||
        credential->Version != S2WIRELESS_CREDENTIAL_VERSION ||
        !credential->Data)
        return 0;

    if (profile == S2FULLMAC_PROFILE_WPA2_CCMP) {
        if (selection->PMFPolicy != S2FULLMAC_PMF_DISABLED ||
            selection->SAEPweMethod)
            return 0;
        if (credential->Kind == S2WIRELESS_CRED_WPA2_PSK)
            return credential->Length == S2WIRELESS_PSK_SIZE;
        if (credential->Kind != S2WIRELESS_CRED_WPA2_PASSPHRASE ||
            credential->Length < 8 || credential->Length > 63)
            return 0;
        for (i = 0; i < credential->Length; ++i)
            if (credential->Data[i] < 32 || credential->Data[i] > 126)
                return 0;
        return 1;
    }

    if (profile != S2FULLMAC_PROFILE_SAE_CCMP_PMF ||
        selection->PMFPolicy != S2FULLMAC_PMF_REQUIRED ||
        !selection->SAEPweMethod ||
        (selection->SAEPweMethod & (selection->SAEPweMethod - 1)) ||
        (selection->SAEPweMethod & caps->SAEPweMethods) !=
            selection->SAEPweMethod ||
        credential->Kind != S2WIRELESS_CRED_SAE_PASSWORD ||
        !credential->Length || credential->Length > 63)
        return 0;
    for (i = 0; i < credential->Length; ++i)
        if (!credential->Data[i])
            return 0;
    return 1;
}

#endif /* DEVICES_SANA2WIRELESS_FULLMAC_H */
