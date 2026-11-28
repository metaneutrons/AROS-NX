/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_FULLMAC_POLICY_H
#define P4_WIFI_FULLMAC_POLICY_H

/* A test-risk opt-in is not a peer lifecycle/attempt contract. Keep this
 * separate and default-off until that contract is qualified. Host models may
 * explicitly enable it, but cannot establish installed-peer qualification. */
#ifndef P4_WIFI_FULLMAC_PEER_CONTRACT_VERIFIED
#define P4_WIFI_FULLMAC_PEER_CONTRACT_VERIFIED 0
#endif
#ifndef P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED
#define P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED 0
#endif
#ifndef P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED
#define P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED 0
#endif
#ifndef P4_WIFI_ASSOCIATION_TEST_RISK_ACCEPTED
#define P4_WIFI_ASSOCIATION_TEST_RISK_ACCEPTED 0
#endif
#define P4_WIFI_FULLMAC_ADMISSION_ENABLED \
    (P4_WIFI_FULLMAC_PEER_CONTRACT_VERIFIED && \
     ((P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED && \
       P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED) || \
      P4_WIFI_ASSOCIATION_TEST_RISK_ACCEPTED))

#endif
