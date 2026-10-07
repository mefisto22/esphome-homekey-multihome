// Shared test helpers: real-crypto model of a HomeKey device ("phone") for
// driving the real HomeKey library through the store. Test code only.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>
#include <mbedtls/cmac.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include "fake_nvs.h"
#include "homekey_library.h"
#include "homekey_store.h"
#include "test_log.h"
#include "x963kdf.h"

using namespace esphome::homekit;
using bytes = std::vector<uint8_t>;

// Ed25519 is libsodium's job, not ours; a test that drives the attestation
// verify() installs a hook that checks what is handed over for verification.
static std::function<int(const unsigned char *, const unsigned char *, unsigned long long, const unsigned char *)>
    g_ed25519_verify_hook;
extern "C" int crypto_sign_ed25519_verify_detached(const unsigned char *sig, const unsigned char *m,
                                                   unsigned long long mlen, const unsigned char *pk) {
  return g_ed25519_verify_hook ? g_ed25519_verify_hook(sig, m, mlen, pk) : -1;
}

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    g_checks++;                                                    \
    if (!(cond)) {                                                 \
      g_failures++;                                                \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                              \
  } while (0)

// ------------------------------------------------------------ crypto utils --
static int rng(void *, unsigned char *buf, size_t len) {
  for (size_t i = 0; i < len; i++)
    buf[i] = static_cast<uint8_t>(rand());
  return 0;
}
static bytes cat(std::initializer_list<bytes> parts) {
  bytes out;
  for (auto &p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}
static bytes tlv(uint8_t tag, const bytes &v) {
  bytes out{tag, static_cast<uint8_t>(v.size())};
  out.insert(out.end(), v.begin(), v.end());
  return out;
}
static bool tlv_get(const bytes &buf, size_t start, uint8_t tag, bytes &out) {
  for (size_t i = start; i + 2 <= buf.size();) {
    uint8_t t = buf[i], l = buf[i + 1];
    if (i + 2 + l > buf.size())
      return false;
    if (t == tag) {
      out.assign(buf.begin() + i + 2, buf.begin() + i + 2 + l);
      return true;
    }
    i += 2 + l;
  }
  return false;
}
struct KeyPair {
  bytes priv;  // 32
  bytes pub;   // 65 uncompressed
};
static KeyPair gen_keypair() {
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &kp, rng, nullptr);
  KeyPair out{bytes(32), bytes(65)};
  mbedtls_mpi_write_binary(&kp.MBEDTLS_PRIVATE(d), out.priv.data(), 32);
  size_t olen = 0;
  mbedtls_ecp_point_write_binary(&kp.MBEDTLS_PRIVATE(grp), &kp.MBEDTLS_PRIVATE(Q), MBEDTLS_ECP_PF_UNCOMPRESSED, &olen,
                                 out.pub.data(), out.pub.size());
  mbedtls_ecp_keypair_free(&kp);
  return out;
}
// Same semantics as the library's get_x (mbedtls_mpi_size, no padding).
static bytes get_x(const bytes &pub) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point p;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&p);
  mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
  mbedtls_ecp_point_read_binary(&grp, &p, pub.data(), pub.size());
  bytes x(mbedtls_mpi_size(&p.MBEDTLS_PRIVATE(X)));
  mbedtls_mpi_write_binary(&p.MBEDTLS_PRIVATE(X), x.data(), x.size());
  mbedtls_ecp_point_free(&p);
  mbedtls_ecp_group_free(&grp);
  return x;
}
static bytes sha256(const bytes &d) {
  bytes h(32);
  mbedtls_sha256(d.data(), d.size(), h.data(), 0);
  return h;
}
static bytes ecdh_x(const bytes &priv, const bytes &peer_pub) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d, z;
  mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&q);
  mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
  mbedtls_mpi_read_binary(&d, priv.data(), priv.size());
  mbedtls_ecp_point_read_binary(&grp, &q, peer_pub.data(), peer_pub.size());
  mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, rng, nullptr);
  bytes out(32);
  mbedtls_mpi_write_binary(&z, out.data(), 32);
  mbedtls_ecp_group_free(&grp);
  mbedtls_mpi_free(&d);
  mbedtls_mpi_free(&z);
  mbedtls_ecp_point_free(&q);
  return out;
}
static bytes ecdsa_sign(const bytes &priv, const bytes &msg) {
  bytes h = sha256(msg);
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &kp, priv.data(), priv.size());
  mbedtls_mpi r, s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  mbedtls_ecdsa_sign(&kp.MBEDTLS_PRIVATE(grp), &r, &s, &kp.MBEDTLS_PRIVATE(d), h.data(), 32, rng, nullptr);
  bytes sig(64);
  mbedtls_mpi_write_binary(&r, sig.data(), 32);
  mbedtls_mpi_write_binary(&s, sig.data() + 32, 32);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  mbedtls_ecp_keypair_free(&kp);
  return sig;
}
static bool ecdsa_verify(const bytes &pub, const bytes &msg, const bytes &sig) {
  if (sig.size() != 64)
    return false;
  bytes h = sha256(msg);
  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
  mbedtls_ecp_point_read_binary(&grp, &q, pub.data(), pub.size());
  mbedtls_mpi_read_binary(&r, sig.data(), 32);
  mbedtls_mpi_read_binary(&s, sig.data() + 32, 32);
  int rc = mbedtls_ecdsa_verify(&grp, h.data(), 32, &q, &r, &s);
  mbedtls_ecp_group_free(&grp);
  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  return rc == 0;
}
static bytes hkdf(const bytes &ikm, const bytes &info, size_t len) {
  bytes out(len);
  mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), nullptr, 0, ikm.data(), ikm.size(), info.data(),
               info.size(), out.data(), len);
  return out;
}
static bytes aes_cbc_enc(const bytes &key, bytes iv, const bytes &pt) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key.data(), 128);
  bytes ct(pt.size());
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, pt.size(), iv.data(), pt.data(), ct.data());
  mbedtls_aes_free(&ctx);
  return ct;
}
static bytes aes_cmac(const bytes &key, const bytes &data) {
  bytes mac(16);
  mbedtls_cipher_cmac(mbedtls_cipher_info_from_values(MBEDTLS_CIPHER_ID_AES, 128, MBEDTLS_MODE_ECB), key.data(), 128,
                      data.data(), data.size(), mac.data());
  return mac;
}
static std::string hex(const bytes &v) { return hk_store::to_hex(v); }

// --------------------------------------------------------- protocol model --
static const bytes SELECT{0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x01, 0x00};
static const bytes SUPPORTED_VERS{0x5C, 0x04, 0x02, 0x00, 0x01, 0x00};
static const bytes PROT_VER{0x5C, 0x02, 0x02, 0x00};
static const bytes FLAGS{0x01, 0x01};

struct Phone {
  std::string name;
  bytes home_reader_pk;   // reader public key of the phone's Home
  bytes home_reader_gid;  // reader group identifier of the phone's Home
  KeyPair endpoint;       // the HomeKey's long-term endpoint key
  bool reject_unknown_group_in_auth0{false};
  // Attacker mode: does not check the reader signature in AUTH1 (it knows no
  // reader key) and answers with an endpoint id the reader does not know,
  // which drives the library into the ATTESTATION flow.
  bool attacker{false};
  // Answers every APDU the model does not handle itself (attestation flow).
  std::function<bytes(const bytes &)> on_other;
  // persistent key per reader identifier, established by a STANDARD flow
  std::map<std::string, bytes> persistent;
  // transaction state
  KeyPair eph;
  bytes reader_eph_pub, txid, reader_ident;
  int selects{0}, auth0{0}, auth1_rejected{0}, flow_fail{0}, flow_ok{0}, standard_ok{0};

  bytes endpoint_id() const {
    bytes h(20);
    mbedtls_sha1(endpoint.pub.data(), endpoint.pub.size(), h.data());
    return bytes(h.begin(), h.begin() + 6);
  }
  bytes info(const char *ctx) const {
    bytes c(ctx, ctx + strlen(ctx));
    return cat({get_x(reader_eph_pub), get_x(eph.pub), txid, {0x5E}, FLAGS, c, PROT_VER, SUPPORTED_VERS});
  }
  bytes transceive(const bytes &apdu) {
    if (apdu.size() >= 2 && apdu[0] == 0x00 && apdu[1] == 0xA4) {
      selects++;
      return {0x5C, 0x02, 0x02, 0x00, 0x90, 0x00};
    }
    if (apdu.size() >= 5 && apdu[0] == 0x80 && apdu[1] == 0x80) {
      auth0++;
      if (!tlv_get(apdu, 5, 0x87, reader_eph_pub) || !tlv_get(apdu, 5, 0x4C, txid) ||
          !tlv_get(apdu, 5, 0x4D, reader_ident))
        return {0x6A, 0x80};
      if (reject_unknown_group_in_auth0 && bytes(reader_ident.begin(), reader_ident.begin() + 8) != home_reader_gid)
        return {0x6A, 0x88};
      eph = gen_keypair();
      bytes crypt(16);
      rng(nullptr, crypt.data(), crypt.size());
      auto it = persistent.find(hex(reader_ident));
      if (it != persistent.end() && bytes(reader_ident.begin(), reader_ident.begin() + 8) == home_reader_gid) {
        const char *c = "VolatileFast";
        bytes material = cat({get_x(home_reader_pk), bytes(c, c + strlen(c)), reader_ident, get_x(endpoint.pub),
                              {0x5E}, SUPPORTED_VERS, PROT_VER, get_x(reader_eph_pub), txid, FLAGS, get_x(eph.pub)});
        bytes k = hkdf(it->second, material, 58);
        crypt.assign(k.begin(), k.begin() + 16);
      }
      return cat({tlv(0x86, eph.pub), tlv(0x9D, crypt), {0x90, 0x00}});
    }
    if (apdu.size() >= 5 && apdu[0] == 0x80 && apdu[1] == 0x81) {
      bytes sig;
      if (!tlv_get(apdu, 5, 0x9E, sig))
        return {0x6A, 0x80};
      bytes signed_data = cat({tlv(0x4D, reader_ident), tlv(0x86, get_x(eph.pub)), tlv(0x87, get_x(reader_eph_pub)),
                               tlv(0x4C, txid), tlv(0x93, {0x41, 0x5D, 0x95, 0x69})});
      if (!attacker && !ecdsa_verify(home_reader_pk, signed_data, sig)) {
        auth1_rejected++;  // reader is not a reader of this phone's Home
        return {0x69, 0x82};
      }
      bytes shared = ecdh_x(eph.priv, reader_eph_pub);
      bytes derived(32);
      X963KDF kdf(MBEDTLS_MD_SHA256, 32, txid.data(), txid.size());
      kdf.derive(shared.data(), shared.size(), derived.data());
      bytes vol = hkdf(derived, info("Volatile"), 48);
      persistent[hex(reader_ident)] = hkdf(derived, info("Persistent"), 32);
      bytes kenc(vol.begin(), vol.begin() + 16), krmac(vol.begin() + 32, vol.end());
      bytes dev_sig = ecdsa_sign(endpoint.priv, cat({tlv(0x4D, reader_ident), tlv(0x86, get_x(eph.pub)),
                                                     tlv(0x87, get_x(reader_eph_pub)), tlv(0x4C, txid),
                                                     tlv(0x93, {0x4E, 0x88, 0x7B, 0x4C})}));
      bytes pt = cat({tlv(0x4E, attacker ? bytes{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01} : endpoint_id()),
                      tlv(0x9E, dev_sig)});
      pt.push_back(0x80);
      while (pt.size() % 16)
        pt.push_back(0x00);
      bytes pcb{0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};  // response pcb + counter 0
      bytes icv = aes_cbc_enc(kenc, bytes(16, 0), pcb);
      bytes ct = aes_cbc_enc(kenc, icv, pt);
      bytes mac = aes_cmac(krmac, cat({bytes(16, 0), ct}));
      standard_ok++;
      return cat({ct, bytes(mac.begin(), mac.begin() + 8), {0x90, 0x00}});
    }
    if (apdu.size() >= 3 && apdu[0] == 0x80 && apdu[1] == 0x3C) {
      (apdu[2] == 0x00 ? flow_fail : flow_ok)++;
      return {0x90, 0x00};
    }
    if (on_other)
      return on_other(apdu);
    return {0x6D, 0x00};
  }
};

static HomeKeyAuthResult tap(HomeKeyStore &store, Phone &phone, const bytes &hint) {
  HKTransceive t = [&phone](bytes &send, bytes &recv, bool) {
    recv = phone.transceive(send);
    return true;
  };
  return store.authenticate(t, hint, SELECT);
}

struct Home {
  KeyPair reader;  // reader private key the Home app writes to the lock
  bytes reader_uid;
  KeyPair controller;  // stands in for the HAP controller (issuer) key
  bytes gid() const {
    const char *p = "key-identifier";
    bytes h = sha256(cat({bytes(p, p + strlen(p)), reader.priv}));
    return bytes(h.begin(), h.begin() + 8);
  }
  bytes issuer_pk() const { return bytes(controller.pub.begin() + 1, controller.pub.begin() + 33); }
  bytes issuer_id() const {
    const char *p = "key-identifier";
    bytes h = sha256(cat({bytes(p, p + strlen(p)), issuer_pk()}));
    return bytes(h.begin(), h.begin() + 8);
  }
};

static bool enroll(HomeKeyStore &store, const Home &home, const Phone &phone) {
  bool ok1 = false, ok2 = false, ok3 = false;
  store.add_issuer(home.issuer_id(), home.issuer_pk());
  // Home app: read reader key, write reader key, provision device credential.
  auto r1 = store.process_access_control(cat({tlv(0x01, {0x01}), tlv(0x06, tlv(0x01, {0x02}))}), ok1);
  auto r2 = store.process_access_control(
      cat({tlv(0x01, {0x02}),
           tlv(0x06, cat({tlv(0x01, {0x02}), tlv(0x02, home.reader.priv), tlv(0x03, home.reader_uid)}))}),
      ok2);
  bytes pk64(phone.endpoint.pub.begin() + 1, phone.endpoint.pub.end());
  auto r3 = store.process_access_control(
      cat({tlv(0x01, {0x02}), tlv(0x04, cat({tlv(0x01, {0x02}), tlv(0x02, pk64), tlv(0x03, home.issuer_id()),
                                               tlv(0x04, {0x01})}))}),
      ok3);
  // Reader key write -> status 0; device credential -> issuer id + status 0
  CHECK(r2 == bytes({0x07, 0x03, 0x02, 0x01, 0x00}));
  // The pinned library returns an empty DCR response even on success.
  CHECK(r3.empty());
  return ok1 && ok2 && ok3;
}

struct Device {
  HomeKeyStore *store{nullptr};
  void boot(int paired) {
    delete store;
    store = new HomeKeyStore();
    homekey_install_library_hooks(*store);
    store->begin(paired);
  }
  ~Device() { delete store; }
};
