// Host stub (unit tests only): the attestation flow is not exercised.
#pragma once
extern "C" int crypto_sign_ed25519_verify_detached(const unsigned char *sig, const unsigned char *m,
                                                   unsigned long long mlen, const unsigned char *pk);
