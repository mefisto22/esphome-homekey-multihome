# Vendored HK-HomeKit-Lib

This directory is a copy of [rednblkx/HK-HomeKit-Lib](https://github.com/rednblkx/HK-HomeKit-Lib)
(MIT License, see `LICENSE`) at commit `c6a5b958bf4312c53df2c9e6aebe846336998861`
(branch `esp-idf`), the version this project previously pulled as a pinned
git dependency.

It is built as a local ESP-IDF component (see `add_homekey_library()` in
`../__init__.py`). ESPHome does not compile this sub-directory into the main
firmware sources itself.

Local modifications are listed in the "Patches" section below; everything else
is byte-identical to the upstream commit.

## Patches

Against malformed or malicious NFC input (every byte from the NFC peer is
attacker controlled), so a hostile tag can no longer crash or hang the
device. All marked with `HAP-ESPHome patch` in the source.

- `priv/TLV8.hpp`: bounds-check the TLV parser; malformed input stops parsing
  instead of reading past the buffer / trusting a bogus BER length.
- `src/utils/ndef.cpp`: bounds-check the NDEF parser; `findType` returns
  `nullptr` for a missing record instead of dereferencing it.
- `src/crypto/DigitalKeySecureContext.cpp`: reject responses shorter than one
  AES block plus the MAC (fixes a `size - 8` underflow).
- `src/crypto/ISO18013SecureContext.cpp`: parse the envelope CBOR without
  exceptions and check the `data` field type and length.
- `src/auth/hkAttestationAuth.cpp`: exception-free, fully type-checked
  `verify()` (CBOR is still parsed non-strictly, as upstream; the issuer id
  is still matched as a prefix, as upstream, but never read past its end);
  guard empty/short APDU responses; size the device-engagement and
  Sig_structure CBOR buffers from the input and reject encoder overflows;
  cap the chained GET RESPONSE reads (256 reads / 32 KB, far above a genuine
  attestation package); require the status and encrypted-message TLVs.
- `src/auth/hkAuthContext.cpp`, `src/auth/hkStdAuth.cpp`: check TLV lookups
  before dereferencing, validate the endpoint public key length, and never
  compare an endpoint id past the end of a shorter device identifier.
- `src/auth/hkFastAuth.cpp`: a cryptogram shorter than the 16 compared bytes
  can never match (no overread).

For valid input every patched path does the same as upstream; the only
additional rejections are malformed data (e.g. COSE device key coordinates
that are not 32 bytes, which RFC 8152 requires) and inputs upstream would
have crashed on. `tests/homekey_host/test_hostile_nfc.cpp` covers both the
hostile inputs and genuine attestation documents.

(none)
