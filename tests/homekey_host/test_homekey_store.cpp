// Host unit tests for components/homekit/homekey_store.{h,cpp}.
//
// Built with -fno-exceptions like the ESP-IDF firmware, so any nlohmann::json
// type error in the decoding paths would abort() the test binary exactly like
// it would reboot-loop the ESP32.
//
// The HomeKey library's provisioning and NFC authentication are replaced by
// fakes that follow the library's observable behaviour (see fake_provision and
// FakePhone/fake_auth). The cryptography inside the library is unchanged by
// this work; what is tested here is everything around it: persistence,
// migration, multi-Home bookkeeping, profile selection and failure handling.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_nvs.h"
#include "homekey_store.h"
#include "test_log.h"

using namespace esphome::homekit;
using bytes = std::vector<uint8_t>;
using json = nlohmann::json;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
      g_failures++;                                                          \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
    }                                                                        \
  } while (0)

static const char *NS = hk_store::NVS_NAMESPACE;
static const char *SCRATCH = hk_store::NVS_SCRATCH_NAMESPACE;

// ---------------------------------------------------------------- helpers --
static bytes filled(size_t n, uint8_t seed) {
  bytes b(n);
  for (size_t i = 0; i < n; i++)
    b[i] = static_cast<uint8_t>(seed + i * 7);
  return b;
}
static bytes tlv(uint8_t tag, const bytes &v) {
  bytes out;
  size_t off = 0;
  do {
    size_t n = std::min<size_t>(255, v.size() - off);
    out.push_back(tag);
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), v.begin() + off, v.begin() + off + n);
    off += n;
  } while (off < v.size());
  return out;
}
static bytes cat(std::initializer_list<bytes> parts) {
  bytes out;
  for (auto &p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}
static bytes req_read_reader_key() { return cat({tlv(0x01, {0x01}), tlv(0x06, tlv(0x01, {0x02}))}); }
static bytes req_write_reader_key(const bytes &sk, const bytes &uid) {
  return cat({tlv(0x01, {0x02}), tlv(0x06, cat({tlv(0x01, {0x02}), tlv(0x02, sk), tlv(0x03, uid)}))});
}
static bytes req_remove_reader_key() { return cat({tlv(0x01, {0x03}), tlv(0x06, tlv(0x04, filled(8, 1)))}); }
static bytes req_device_credential(const bytes &issuer_id, const bytes &pk64) {
  return cat({tlv(0x01, {0x02}),
              tlv(0x04, cat({tlv(0x01, {0x02}), tlv(0x02, pk64), tlv(0x03, issuer_id), tlv(0x04, {0x01})}))});
}

// Fake gid derivation (the real library uses SHA256("key-identifier"||sk)[:8]).
static bytes fake_gid(const bytes &sk) {
  bytes g(sk.begin(), sk.begin() + 8);
  for (auto &b : g)
    b ^= 0x5A;
  return g;
}
static bytes fake_endpoint_id(const bytes &pk64) {
  bytes e(pk64.begin(), pk64.begin() + 6);
  for (auto &b : e)
    b ^= 0x33;
  return e;
}

// Mirrors HK_HomeKit::processResult for the operations Apple Home uses. Like
// the library it also writes a private copy into the NVS handle it is given.
static int g_provision_calls = 0;
static bytes fake_provision(readerData_t &rd, bytes &req, nvs_handle_t scratch) {
  g_provision_calls++;
  bytes op, rkr, dcr;
  bool f1, f2, f3;
  hk_store::tlv8_find(req.data(), req.size(), 0x01, op, f1);
  hk_store::tlv8_find(req.data(), req.size(), 0x06, rkr, f2);
  hk_store::tlv8_find(req.data(), req.size(), 0x04, dcr, f3);
  auto library_save = [&]() {
    bytes blob = json::to_msgpack(rd);
    return nvs_set_blob(scratch, "READERDATA", blob.data(), blob.size()) == ESP_OK && nvs_commit(scratch) == ESP_OK;
  };
  if (op[0] == 0x01) {
    if (!rd.reader_sk.empty())
      return tlv(0x07, tlv(0x01, rd.reader_gid));
    return {0x01, 0x01, 0x01, 0x07, 0x00};
  }
  if (op[0] == 0x02 && f2) {
    bytes sk, uid;
    bool a, b;
    hk_store::tlv8_find(rkr.data(), rkr.size(), 0x02, sk, a);
    hk_store::tlv8_find(rkr.data(), rkr.size(), 0x03, uid, b);
    rd.reader_sk = sk;
    rd.reader_id = uid;
    rd.reader_pk = cat({{0x04}, sk, sk});
    rd.reader_pk_x = sk;
    rd.reader_gid = fake_gid(sk);
    if (!library_save())
      return {};
    return {0x07, 0x03, 0x02, 0x01, 0x00};
  }
  if (op[0] == 0x02 && f3) {
    bytes iid, pk, kt;
    bool a, b, c;
    hk_store::tlv8_find(dcr.data(), dcr.size(), 0x03, iid, a);
    hk_store::tlv8_find(dcr.data(), dcr.size(), 0x02, pk, b);
    hk_store::tlv8_find(dcr.data(), dcr.size(), 0x01, kt, c);
    for (auto &iss : rd.issuers) {
      if (iss.issuer_id == iid) {
        hkEndpoint_t ep;  // intentionally naive: always appended (store must de-duplicate)
        ep.endpoint_id = fake_endpoint_id(pk);
        ep.endpoint_pk = cat({{0x04}, pk});
        ep.endpoint_pk_x = bytes(pk.begin(), pk.begin() + 32);
        ep.key_type = kt[0];
        iss.endpoints.push_back(ep);
        library_save();
        return {};  // the pinned library never fills the DCR response (see homekey_store.cpp)
      }
    }
    return tlv(0x05, cat({tlv(0x02, iid), tlv(0x03, {0x03})}));
  }
  if (op[0] == 0x03 && f2) {
    rd.reader_gid.clear();
    rd.reader_id.clear();
    rd.reader_sk.clear();
    library_save();
    return {0x07, 0x03, 0x02, 0x01, 0x00};
  }
  return {};
}

// A phone holding one HomeKey (bound to one Home's reader group).
struct FakePhone {
  bytes gid;          // reader group the phone's HomeKey belongs to
  bytes endpoint_id;  // the phone's endpoint id
  bool present{true};
  int selects{0};
  int auth0{0};
  int flow_fail{0};
  int flow_success{0};
  bool transceive(bytes &send, bytes &recv) {
    if (!present)
      return false;
    if (send.size() >= 2 && send[0] == 0x00 && send[1] == 0xA4) {
      selects++;
      recv = {0x5C, 0x02, 0x02, 0x00, 0x90, 0x00};
      return true;
    }
    if (send.size() >= 2 && send[0] == 0x80 && send[1] == 0x80) {
      auth0++;
      bytes reader_identifier(send.end() - 16, send.end());
      if (bytes(reader_identifier.begin(), reader_identifier.begin() + 8) != gid) {
        recv = {0x6A, 0x88};  // no key for this reader group
        return true;
      }
      recv = cat({{0x86, 0x41}, filled(65, 9), {0x9D, 0x10}, filled(16, 3), {0x90, 0x00}});
      return true;
    }
    if (send.size() >= 3 && send[0] == 0x80 && send[1] == 0x3C) {
      if (send[2] == 0x00)
        flow_fail++;
      else
        flow_success++;
      recv = {0x90, 0x00};
      return true;
    }
    recv = {0x6D, 0x00};
    return true;
  }
};

static KeyFlow g_auth_flow = kFlowFAST;
static int g_auth_ro_write_rejected = 0;
// Mirrors HKAuthenticationContext::authenticate's APDU sequence.
static std::tuple<bytes, bytes, KeyFlow> fake_auth(readerData_t &rd, const HKTransceive &nfc, nvs_handle_t ro) {
  // The library writes its single-profile blob through the handle it gets;
  // that handle must be read-only so the multi-Home store is never clobbered.
  bytes junk{1, 2, 3};
  if (nvs_set_blob(ro, "READERDATA", junk.data(), junk.size()) != ESP_OK)
    g_auth_ro_write_rejected++;
  bytes ident = cat({rd.reader_gid, rd.reader_id});
  bytes auth0 = cat({{0x80, 0x80, 0x01, 0x01, 0x20}, filled(16, 0), ident});
  bytes resp;
  nfc(auth0, resp, false);
  if (!(resp.size() > 64 && resp[0] == 0x86)) {
    bytes f{0x80, 0x3C, 0x00, 0x00}, r;
    nfc(f, r, false);
    nfc(f, r, false);  // the library really sends it twice on this path
    return {{}, {}, kFlowFailed};
  }
  // FakePhone embeds its endpoint id in the cryptogram slot for simplicity.
  for (auto &iss : rd.issuers) {
    for (auto &ep : iss.endpoints) {
      if (!ep.endpoint_id.empty() && std::search(resp.begin(), resp.end(), ep.endpoint_id.begin(),
                                                 ep.endpoint_id.end()) != resp.end()) {
        if (g_auth_flow == kFlowSTANDARD)
          ep.endpoint_prst_k = filled(32, 0xEE);
        bytes ok{0x80, 0x3C, 0x01, 0x00}, r;
        nfc(ok, r, false);
        return {iss.issuer_id, ep.endpoint_id, g_auth_flow};
      }
    }
  }
  bytes f{0x80, 0x3C, 0x00, 0x00}, r;
  nfc(f, r, false);
  return {{}, {}, kFlowFailed};
}

static const bytes SELECT{0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x01, 0x00};

static HomeKeyAuthResult tap(HomeKeyStore &store, FakePhone &phone, const bytes &hint) {
  // Make the phone's response contain its endpoint id so fake_auth can match.
  HKTransceive t = [&phone](bytes &send, bytes &recv, bool) {
    bool ok = phone.transceive(send, recv);
    if (ok && recv.size() > 64 && recv[0] == 0x86)
      std::copy(phone.endpoint_id.begin(), phone.endpoint_id.end(), recv.begin() + 70);
    return ok;
  };
  return store.authenticate(t, hint, SELECT);
}

static void wire(HomeKeyStore &store) {
  store.set_provisioner(fake_provision);
  store.set_authenticator(fake_auth);
}

// Simulates "reboot" (or OTA): a fresh store instance over the same NVS.
struct Device {
  HomeKeyStore *store{nullptr};
  void boot(int paired) {
    delete store;
    store = new HomeKeyStore();
    wire(*store);
    store->begin(paired);
  }
  ~Device() { delete store; }
};

// Home fixtures
struct Home {
  bytes sk, uid, issuer_id, issuer_pk, phone_pk;
  bytes gid() const { return fake_gid(sk); }
  bytes endpoint() const { return fake_endpoint_id(phone_pk); }
};
static Home home_a{filled(32, 0xA1), filled(8, 0x11), filled(8, 0x21), filled(32, 0x31), filled(64, 0x41)};
static Home home_b{filled(32, 0xB2), filled(8, 0x12), filled(8, 0x22), filled(32, 0x32), filled(64, 0x42)};

static bool provision(HomeKeyStore &store, const Home &h) {
  bool ok1 = false, ok2 = false, ok3 = false;
  store.add_issuer(h.issuer_id, h.issuer_pk);
  store.process_access_control(req_read_reader_key(), ok1);
  store.process_access_control(req_write_reader_key(h.sk, h.uid), ok2);
  store.process_access_control(req_device_credential(h.issuer_id, h.phone_pk), ok3);
  return ok1 && ok2 && ok3;
}

static size_t keyed(const std::vector<readerData_t> &ps) {
  size_t n = 0;
  for (auto &p : ps)
    n += hk_store::has_reader_key(p);
  return n;
}
static const readerData_t *find_profile(HomeKeyStore &store, const bytes &gid, std::vector<readerData_t> &keep) {
  keep = store.profiles_copy();
  for (auto &p : keep)
    if (p.reader_gid == gid)
      return &p;
  return nullptr;
}

// ------------------------------------------------------------------ tests --
static void test_fresh_device() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(d.store->profiles_copy().empty());
  CHECK(d.store->active_index() == -1);
  CHECK(fake_nvs::write_count() == 0);  // nothing written on a fresh device
  auto frames = d.store->ecp_frames();
  CHECK(frames.size() == 1);
  CHECK(frames[0].size() == 18);
  // Same frame the old code produced for an unprovisioned reader.
  bytes zero_frame{0x6A, 0x02, 0xCB, 0x02, 0x06, 0x02, 0x11, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(bytes(frames[0].begin(), frames[0].begin() + 16) == zero_frame);
}

static void test_single_home_provisioning_and_reboot() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  auto ps = d.store->profiles_copy();
  CHECK(ps.size() == 1);
  CHECK(d.store->active_index() == 0);
  CHECK(hk_store::has_reader_key(ps[0]));
  CHECK(ps[0].issuers.size() == 1);
  CHECK(ps[0].issuers[0].endpoints.size() == 1);
  CHECK(fake_nvs::has(NS, hk_store::NVS_KEY_STORE));
  // The library's private copy (holds the reader private key) is scrubbed.
  CHECK(fake_nvs::flash()[SCRATCH].empty());
  // Reboot / OTA with the Home still paired.
  d.boot(1);
  ps = d.store->profiles_copy();
  CHECK(ps.size() == 1 && d.store->active_index() == 0);
  CHECK(ps[0].reader_sk == home_a.sk);
  CHECK(ps[0].reader_gid == home_a.gid());
  CHECK(ps[0].issuers.size() == 1 && ps[0].issuers[0].endpoints.size() == 1);
  FakePhone phone{home_a.gid(), home_a.endpoint()};
  auto r = tap(*d.store, phone, home_a.gid());
  CHECK(r.success && r.attempts == 1);
  CHECK(r.issuer_id == home_a.issuer_id && r.endpoint_id == home_a.endpoint());
  CHECK(phone.flow_fail == 0 && phone.flow_success == 1);
  CHECK(g_auth_ro_write_rejected > 0);
  auto frames = d.store->ecp_frames();
  CHECK(frames.size() == 1 && bytes(frames[0].begin() + 8, frames[0].begin() + 16) == home_a.gid());
}

static void test_two_independent_homes() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  const auto profile_a_before = d.store->profiles_copy()[0];

  // "Reset HomeKit pairing (keep HomeKeys)": archive, HAP reset, reboot unpaired.
  CHECK(d.store->can_enroll_another_home());
  CHECK(d.store->archive_active_home());
  d.boot(0);
  CHECK(d.store->active_index() == -1);
  CHECK(keyed(d.store->profiles_copy()) == 1);

  // Home B pairs and provisions. Its reader-key write must NOT touch Home A.
  CHECK(provision(*d.store, home_b));
  auto ps = d.store->profiles_copy();
  CHECK(ps.size() == 2);
  CHECK(keyed(ps) == 2);
  std::vector<readerData_t> keep;
  const readerData_t *pa = find_profile(*d.store, home_a.gid(), keep);
  CHECK(pa != nullptr);
  if (pa) {
    CHECK(pa->reader_sk == profile_a_before.reader_sk);
    CHECK(pa->reader_id == profile_a_before.reader_id);
    CHECK(pa->issuers.size() == 1 && pa->issuers[0].issuer_id == home_a.issuer_id);
    CHECK(pa->issuers[0].endpoints.size() == 1);
  }
  const readerData_t *pb = find_profile(*d.store, home_b.gid(), keep);
  CHECK(pb != nullptr);
  if (pb) {
    CHECK(pb->issuers.size() == 1 && pb->issuers[0].issuer_id == home_b.issuer_id);
    CHECK(pb->issuers[0].endpoints.size() == 1);
  }
  CHECK(d.store->ecp_frames().size() == 2);

  auto check_both = [&](const char *when) {
    for (int hint = 0; hint < 3; hint++) {
      bytes h = hint == 0 ? home_a.gid() : hint == 1 ? home_b.gid() : bytes{};
      FakePhone pa_phone{home_a.gid(), home_a.endpoint()};
      FakePhone pb_phone{home_b.gid(), home_b.endpoint()};
      auto ra = tap(*d.store, pa_phone, h);
      auto rb = tap(*d.store, pb_phone, h);
      CHECK(ra.success && ra.issuer_id == home_a.issuer_id && ra.reader_gid == home_a.gid());
      CHECK(rb.success && rb.issuer_id == home_b.issuer_id && rb.reader_gid == home_b.gid());
      // A failed attempt on the wrong profile must not end the transaction:
      // no "failed" control flow reaches the phone, a re-SELECT follows.
      CHECK(pa_phone.flow_fail == 0 && pb_phone.flow_fail == 0);
      if (hint == 0) {
        CHECK(ra.attempts == 1 && rb.attempts == 2 && pb_phone.selects == 1);
      }
      if (hint == 1) {
        CHECK(ra.attempts == 2 && rb.attempts == 1 && pa_phone.selects == 1);
      }
    }
    (void) when;
  };
  check_both("after enrolment");
  d.boot(1);  // reboot while Home B is paired
  CHECK(keyed(d.store->profiles_copy()) == 2);
  check_both("after reboot");
  d.boot(1);  // OTA = same as reboot for NVS
  check_both("after OTA");

  // Unknown phone: fails after trying every profile; exactly one failure
  // control flow (from the last attempt) reaches it.
  FakePhone stranger{filled(8, 0x77), filled(6, 0x78)};
  auto rs = tap(*d.store, stranger, home_a.gid());
  CHECK(!rs.success && rs.attempts == 2);
  CHECK(stranger.flow_fail >= 1 && stranger.flow_success == 0);

  // A phone of Home A whose endpoint is unknown must not be accepted via B.
  FakePhone a_unknown{home_a.gid(), filled(6, 0x55)};
  CHECK(!tap(*d.store, a_unknown, home_b.gid()).success);

  // Phone leaves the field after the first failed attempt: no crash, fail.
  FakePhone leaving{home_b.gid(), home_b.endpoint()};
  HKTransceive t = [&leaving](bytes &s, bytes &r, bool) {
    bool ok = leaving.transceive(s, r);
    leaving.present = false;
    return ok;
  };
  CHECK(!d.store->authenticate(t, home_a.gid(), SELECT).success);
}

static void test_duplicates() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  // Same controller paired again / permissions modified.
  CHECK(d.store->add_issuer(home_a.issuer_id, home_a.issuer_pk));
  CHECK(d.store->add_issuer(home_a.issuer_id, home_a.issuer_pk));
  // Same device credential provisioned again (fake library appends blindly).
  bool ok = false;
  d.store->process_access_control(req_device_credential(home_a.issuer_id, home_a.phone_pk), ok);
  CHECK(ok);
  // Same reader key written again.
  d.store->process_access_control(req_write_reader_key(home_a.sk, home_a.uid), ok);
  CHECK(ok);
  auto ps = d.store->profiles_copy();
  CHECK(ps.size() == 1);
  CHECK(ps[0].issuers.size() == 1);
  CHECK(ps[0].issuers[0].endpoints.size() == 1);

  // Home A re-paired after a pairing reset: profiles are merged, not doubled.
  CHECK(d.store->archive_active_home());
  d.boot(0);
  bytes second_user = filled(8, 0x29);
  CHECK(d.store->add_issuer(second_user, filled(32, 0x39)));
  CHECK(d.store->profiles_copy().size() == 2);  // new (keyless) active + archived A
  d.store->process_access_control(req_write_reader_key(home_a.sk, home_a.uid), ok);
  CHECK(ok);
  ps = d.store->profiles_copy();
  CHECK(ps.size() == 1);
  CHECK(d.store->active_index() == 0);
  CHECK(ps[0].issuers.size() == 2);
  CHECK(ps[0].issuers[0].endpoints.size() + ps[0].issuers[1].endpoints.size() == 1);
}

static void test_standard_flow_persists_persistent_key() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  g_auth_flow = kFlowSTANDARD;
  FakePhone phone{home_a.gid(), home_a.endpoint()};
  CHECK(tap(*d.store, phone, home_a.gid()).success);
  g_auth_flow = kFlowFAST;
  d.boot(1);
  CHECK(d.store->profiles_copy()[0].issuers[0].endpoints[0].endpoint_prst_k == filled(32, 0xEE));
}

static void test_pairing_reset_paths_keep_homekeys() {
  // Old-style reset (homekit_base factory_reset button / hap_reset_pairings
  // without archiving first): detected at boot because no controller is paired.
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  d.boot(0);
  CHECK(d.store->active_index() == -1);
  CHECK(keyed(d.store->profiles_copy()) == 1);
  FakePhone phone{home_a.gid(), home_a.endpoint()};
  CHECK(tap(*d.store, phone, {}).success);
  // Next Home writes its reader key into a NEW profile.
  CHECK(provision(*d.store, home_b));
  CHECK(keyed(d.store->profiles_copy()) == 2);
}

static void test_revoke_on_removal_from_home() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  CHECK(d.store->archive_active_home());
  d.boot(0);
  CHECK(provision(*d.store, home_b));
  fake_nvs::put(NS, hk_store::NVS_KEY_LEGACY, {0x80});  // pretend a legacy backup exists
  // Home B's admin removes the accessory from Home B: only B is revoked.
  d.store->revoke_active_home();
  auto ps = d.store->profiles_copy();
  CHECK(ps.size() == 1 && ps[0].reader_gid == home_a.gid());
  CHECK(!fake_nvs::has(NS, hk_store::NVS_KEY_LEGACY));
  d.boot(0);
  CHECK(keyed(d.store->profiles_copy()) == 1);
  FakePhone pb{home_b.gid(), home_b.endpoint()};
  CHECK(!tap(*d.store, pb, {}).success);
  FakePhone pa{home_a.gid(), home_a.endpoint()};
  CHECK(tap(*d.store, pa, {}).success);
}

static void test_factory_reset() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  CHECK(d.store->archive_active_home());
  d.boot(0);
  CHECK(provision(*d.store, home_b));
  fake_nvs::put(NS, hk_store::NVS_KEY_LEGACY, {0x80});
  CHECK(d.store->erase_all());
  CHECK(d.store->profiles_copy().empty());
  CHECK(fake_nvs::flash()[NS].empty());
  CHECK(fake_nvs::flash()[SCRATCH].empty());
  // Nothing can be re-persisted before the reboot that follows.
  bool ok = true;
  d.store->process_access_control(req_write_reader_key(home_a.sk, home_a.uid), ok);
  CHECK(!ok);
  CHECK(fake_nvs::flash()[NS].empty());
  d.boot(0);
  CHECK(d.store->profiles_copy().empty());
  CHECK(!d.store->persistence_disabled());
  FakePhone pa{home_a.gid(), home_a.endpoint()};
  CHECK(!tap(*d.store, pa, {}).success);
  // Fresh provisioning works again after the reset.
  CHECK(provision(*d.store, home_a));
}

static void test_legacy_migration(int paired) {
  fake_nvs::reset();
  // Build READERDATA exactly like the old firmware did: library to_json + msgpack.
  readerData_t legacy;
  legacy.reader_sk = home_a.sk;
  legacy.reader_pk = cat({{0x04}, home_a.sk, home_a.sk});
  legacy.reader_pk_x = home_a.sk;
  legacy.reader_gid = home_a.gid();
  legacy.reader_id = home_a.uid;
  hkIssuer_t iss;
  iss.issuer_id = home_a.issuer_id;
  iss.issuer_pk = home_a.issuer_pk;
  hkEndpoint_t ep;
  ep.endpoint_id = home_a.endpoint();
  ep.endpoint_pk = cat({{0x04}, home_a.phone_pk});
  ep.endpoint_pk_x = bytes(home_a.phone_pk.begin(), home_a.phone_pk.begin() + 32);
  ep.endpoint_prst_k = filled(32, 0x99);
  ep.key_type = 2;
  iss.endpoints.push_back(ep);
  legacy.issuers.push_back(iss);
  hkIssuer_t iss2;  // issuer without endpoints (e.g. second Home user)
  iss2.issuer_id = filled(8, 0x66);
  iss2.issuer_pk = filled(32, 0x67);
  legacy.issuers.push_back(iss2);
  bytes legacy_blob = json::to_msgpack(legacy);
  fake_nvs::put(NS, hk_store::NVS_KEY_LEGACY, legacy_blob);

  Device d;
  d.boot(paired);
  auto ps = d.store->profiles_copy();
  CHECK(ps.size() == 1);
  CHECK(d.store->active_index() == (paired ? 0 : -1));
  CHECK(ps[0].reader_sk == legacy.reader_sk && ps[0].reader_pk == legacy.reader_pk);
  CHECK(ps[0].reader_gid == legacy.reader_gid && ps[0].reader_id == legacy.reader_id);
  CHECK(ps[0].issuers.size() == 2);
  CHECK(ps[0].issuers[0].endpoints.size() == 1);
  CHECK(ps[0].issuers[0].endpoints[0].endpoint_prst_k == ep.endpoint_prst_k);
  CHECK(ps[0].issuers[0].endpoints[0].key_type == 2);
  CHECK(fake_nvs::has(NS, hk_store::NVS_KEY_STORE));
  CHECK(fake_nvs::get(NS, hk_store::NVS_KEY_LEGACY) == legacy_blob);  // backup untouched
  FakePhone phone{home_a.gid(), home_a.endpoint()};
  CHECK(tap(*d.store, phone, {}).success);
  // Second boot reads the new store, migration does not run twice.
  int writes = fake_nvs::write_count();
  d.boot(paired);
  CHECK(fake_nvs::write_count() == writes);
  CHECK(d.store->profiles_copy().size() == 1);
}

static void test_corrupted_data() {
  struct Case {
    const char *name;
    bytes blob;
  };
  std::vector<Case> cases = {
      {"random bytes", {0xC1, 0xFF, 0x00, 0x13}},
      {"truncated msgpack", {}},  // filled in below from a real store blob
      {"not a map", json::to_msgpack(json::array({1, 2, 3}))},
      {"readers wrong type", json::to_msgpack(json{{"v", 2}, {"readers", "x"}})},
      {"reader not object", json::to_msgpack(json{{"v", 2}, {"readers", json::array({5})}})},
      {"key as string", json::to_msgpack(json{{"v", 2}, {"readers", json::array({json{{"reader_private_key", "abc"}}})}})},
      {"byte out of range", json::to_msgpack(json{{"v", 2}, {"readers", json::array({json{{"group_identifier", json::array({1, 300})}}})}})},
      {"issuers wrong type", json::to_msgpack(json{{"v", 2}, {"readers", json::array({json{{"issuers", 7}}})}})},
      {"endpoint garbage", json::to_msgpack(json{{"v", 2}, {"readers", json::array({json{{"issuers", json::array({json{{"endpoints", json::array({json{{"counter", "x"}}})}}})}}})}})},
      {"too many readers", json::to_msgpack(json{{"v", 2}, {"readers", json(std::vector<json>(20, json::object()))}})},
      {"missing version", json::to_msgpack(json{{"readers", json::array()}})},
  };
  // Proper truncated variant
  {
    fake_nvs::reset();
    Device d;
    d.boot(0);
    provision(*d.store, home_a);
    bytes good = fake_nvs::get(NS, hk_store::NVS_KEY_STORE);
    cases[1].blob = bytes(good.begin(), good.begin() + good.size() / 2);
  }
  for (auto &c : cases) {
    fake_nvs::reset();
    fake_nvs::put(NS, hk_store::NVS_KEY_STORE, c.blob);
    Device d;
    d.boot(1);  // must not abort()
    CHECK(d.store->profiles_copy().empty());
    CHECK(fake_nvs::get(NS, hk_store::NVS_KEY_QUARANTINE) == c.blob);
    CHECK(!d.store->persistence_disabled());
    CHECK(provision(*d.store, home_a));  // device stays usable
  }
  // Corrupted new store but valid legacy data: fall back to the legacy data.
  {
    fake_nvs::reset();
    readerData_t legacy;
    legacy.reader_sk = home_a.sk;
    legacy.reader_pk = cat({{0x04}, home_a.sk, home_a.sk});
    legacy.reader_pk_x = home_a.sk;
    legacy.reader_gid = home_a.gid();
    legacy.reader_id = home_a.uid;
    fake_nvs::put(NS, hk_store::NVS_KEY_LEGACY, json::to_msgpack(legacy));
    fake_nvs::put(NS, hk_store::NVS_KEY_STORE, {0xC1});
    Device d;
    d.boot(1);
    CHECK(keyed(d.store->profiles_copy()) == 1);
  }
  // Corrupted legacy data: left untouched, start empty, no crash.
  {
    fake_nvs::reset();
    bytes bad = json::to_msgpack(json{{"group_identifier", "zz"}});
    fake_nvs::put(NS, hk_store::NVS_KEY_LEGACY, bad);
    Device d;
    d.boot(1);
    CHECK(d.store->profiles_copy().empty());
    CHECK(fake_nvs::get(NS, hk_store::NVS_KEY_LEGACY) == bad);
    CHECK(!fake_nvs::has(NS, hk_store::NVS_KEY_STORE));
  }
  // Empty blob.
  {
    fake_nvs::reset();
    fake_nvs::put(NS, hk_store::NVS_KEY_STORE, {});
    Device d;
    d.boot(0);
    CHECK(d.store->profiles_copy().empty());
  }
}

static void test_future_version_is_never_overwritten() {
  fake_nvs::reset();
  bytes future = json::to_msgpack(json{{"v", 3}, {"something", "new"}});
  fake_nvs::put(NS, hk_store::NVS_KEY_STORE, future);
  Device d;
  d.boot(1);
  CHECK(d.store->persistence_disabled());
  bool ok = true;
  d.store->process_access_control(req_write_reader_key(home_a.sk, home_a.uid), ok);
  CHECK(!ok);
  d.store->add_issuer(home_a.issuer_id, home_a.issuer_pk);
  CHECK(fake_nvs::get(NS, hk_store::NVS_KEY_STORE) == future);
  CHECK(!d.store->can_enroll_another_home());
}

static void test_write_failure_rolls_back() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  auto before = hk_store::encode(d.store->profiles_copy(), d.store->active_index());
  // (1) the library's own write fails (it already changed the profile in RAM)
  fake_nvs::fail_next_writes(1);
  bool ok = true;
  auto resp = d.store->process_access_control(req_write_reader_key(home_b.sk, home_b.uid), ok);
  CHECK(!ok && resp.empty());
  CHECK(hk_store::encode(d.store->profiles_copy(), d.store->active_index()) == before);
  // (2) the library succeeds but persisting the store fails
  ok = true;
  HomeKeyStore::ProvisionFn wrapped = [](readerData_t &rd, bytes &req, nvs_handle_t h) {
    bytes r = fake_provision(rd, req, h);
    fake_nvs::fail_next_writes(1);  // the store's own save comes next
    return r;
  };
  d.store->set_provisioner(wrapped);
  resp = d.store->process_access_control(req_write_reader_key(home_b.sk, home_b.uid), ok);
  CHECK(!ok && resp.empty());
  CHECK(hk_store::encode(d.store->profiles_copy(), d.store->active_index()) == before);
  bytes new_phone = filled(64, 0x99);
  resp = d.store->process_access_control(req_device_credential(home_a.issuer_id, new_phone), ok);
  CHECK(!ok);
  CHECK(hk_store::encode(d.store->profiles_copy(), d.store->active_index()) == before);
  d.store->set_provisioner(fake_provision);
  // Archive failure keeps everything as it was.
  fake_nvs::fail_next_writes(1);
  CHECK(!d.store->archive_active_home());
  CHECK(d.store->active_index() == 0);
  d.boot(1);
  CHECK(d.store->profiles_copy()[0].reader_sk == home_a.sk);
}

static void test_malformed_access_control_requests() {
  fake_nvs::reset();
  Device d;
  d.boot(1);
  d.store->add_issuer(home_a.issuer_id, home_a.issuer_pk);
  std::vector<bytes> bad = {
      {},
      {0x01},                                    // truncated
      {0x01, 0x05, 0x02},                        // length overrun
      tlv(0x06, tlv(0x01, {0x02})),              // no operation
      tlv(0x01, {0x01}),                         // read without RKR
      tlv(0x01, {0x02}),                         // write without RKR/DCR
      cat({tlv(0x01, {0x09}), tlv(0x06, {})}),   // unknown operation
      cat({tlv(0x01, {0x02}), tlv(0x04, cat({tlv(0x02, filled(64, 1)), tlv(0x01, {2})}))}),  // DCR w/o issuer
      cat({tlv(0x01, {0x02}), tlv(0x04, cat({tlv(0x03, filled(4, 1)), tlv(0x02, filled(64, 1)), tlv(0x01, {2})}))}),
      cat({tlv(0x01, {0x02}), tlv(0x04, cat({tlv(0x03, filled(8, 1)), tlv(0x02, filled(10, 1)), tlv(0x01, {2})}))}),
      cat({tlv(0x01, {0x02}), tlv(0x06, {0x02, 0x40, 0x01})}),  // nested overrun
  };
  int calls = g_provision_calls;
  for (auto &b : bad) {
    bool ok = true;
    auto r = d.store->process_access_control(b, ok);
    CHECK(!ok && r.empty());
  }
  CHECK(g_provision_calls == calls);
  // Fragmented (>255 byte) TLV item is reassembled.
  bytes big = filled(300, 5), out;
  bool found = false;
  bytes enc = cat({tlv(0x01, {0x02}), tlv(0x09, big)});
  CHECK(hk_store::tlv8_find(enc.data(), enc.size(), 0x09, out, found) && found && out == big);
}

static void test_capacity_limit() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  for (size_t i = 0; i < hk_store::MAX_PROFILES; i++) {
    Home h{filled(32, 0x10 + i), filled(8, 0x50 + i), filled(8, 0x60 + i), filled(32, 0x70 + i), filled(64, 0x80 + i)};
    CHECK(d.store->can_enroll_another_home());
    CHECK(provision(*d.store, h));
    CHECK(d.store->archive_active_home());
    d.boot(0);
  }
  CHECK(keyed(d.store->profiles_copy()) == hk_store::MAX_PROFILES);
  CHECK(!d.store->can_enroll_another_home());
  // A pairing that happens anyway cannot evict existing Homes.
  CHECK(!d.store->add_issuer(home_a.issuer_id, home_a.issuer_pk));
  CHECK(keyed(d.store->profiles_copy()) == hk_store::MAX_PROFILES);
  CHECK(d.store->ecp_frames().size() == hk_store::MAX_PROFILES);
}

static void test_remove_reader_key_only_affects_active_home() {
  fake_nvs::reset();
  Device d;
  d.boot(0);
  CHECK(provision(*d.store, home_a));
  CHECK(d.store->archive_active_home());
  d.boot(0);
  CHECK(provision(*d.store, home_b));
  bool ok = false;
  d.store->process_access_control(req_remove_reader_key(), ok);
  CHECK(ok);
  auto ps = d.store->profiles_copy();
  CHECK(keyed(ps) == 1);
  std::vector<readerData_t> keep;
  CHECK(find_profile(*d.store, home_a.gid(), keep) != nullptr);
}

static void test_codec_roundtrip_and_ecp() {
  std::vector<readerData_t> ps(2);
  ps[0].reader_sk = home_a.sk;
  ps[0].reader_pk = cat({{0x04}, home_a.sk, home_a.sk});
  ps[0].reader_pk_x = home_a.sk;
  ps[0].reader_gid = home_a.gid();
  ps[0].reader_id = home_a.uid;
  hkIssuer_t iss;
  iss.issuer_id = home_a.issuer_id;
  iss.issuer_pk = home_a.issuer_pk;
  hkEndpoint_t ep;
  ep.endpoint_id = home_a.endpoint();
  ep.counter = 5;
  ep.last_used_at = 1234567;
  ep.key_type = 2;
  ep.endpoint_prst_k = filled(32, 1);
  iss.endpoints.push_back(ep);
  ps[0].issuers.push_back(iss);
  bytes blob = hk_store::encode(ps, 1);
  std::vector<readerData_t> out;
  int active = -2;
  CHECK(hk_store::decode(blob, out, active) == hk_store::DecodeStatus::OK);
  CHECK(active == 1);
  CHECK(hk_store::encode(out, active) == blob);
  CHECK(out[0].issuers[0].endpoints[0].last_used_at == 1234567u);
  // ECP frame / CRC identical to the original implementation.
  auto ref_crc = [](const uint8_t *data, unsigned size, uint8_t *res) {
    unsigned short w_crc = 0x6363;
    for (unsigned i = 0; i < size; ++i) {
      unsigned char byte = data[i];
      byte = (byte ^ (w_crc & 0x00FF));
      byte = ((byte ^ (byte << 4)) & 0xFF);
      w_crc = ((w_crc >> 8) ^ (byte << 8) ^ (byte << 3) ^ (byte >> 4)) & 0xFFFF;
    }
    res[0] = w_crc & 0xFF;
    res[1] = (w_crc >> 8) & 0xFF;
  };
  bytes ref{0x6A, 0x2, 0xCB, 0x2, 0x6, 0x2, 0x11, 0x0};
  ref.resize(18);
  const bytes gid = home_a.gid();
  std::copy(gid.begin(), gid.end(), ref.begin() + 8);
  ref_crc(ref.data(), 16, ref.data() + 16);
  CHECK(hk_store::ecp_frame(home_a.gid()) == ref);
}

static void test_no_secrets_in_logs() {
  const std::string &log = test_log_buffer();
  for (const Home *h : {&home_a, &home_b}) {
    CHECK(log.find(hk_store::to_hex(h->sk)) == std::string::npos);
    CHECK(log.find(hk_store::to_hex(bytes(h->sk.begin(), h->sk.begin() + 8))) == std::string::npos);
  }
  CHECK(log.find(hk_store::to_hex(filled(32, 0xEE))) == std::string::npos);  // persistent key
  // Non-secret identifiers are logged.
  CHECK(log.find(hk_store::to_hex(home_a.issuer_id)) != std::string::npos);
}

int main() {
  struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"fresh device", test_fresh_device},
      {"single Home provisioning + reboot", test_single_home_provisioning_and_reboot},
      {"two independent Homes (reset keep, reboot, OTA, auth lookup)", test_two_independent_homes},
      {"duplicate issuer/endpoint/reader key + re-pairing same Home", test_duplicates},
      {"STANDARD flow persistent key is persisted", test_standard_flow_persists_persistent_key},
      {"pairing reset paths keep HomeKeys", test_pairing_reset_paths_keep_homekeys},
      {"removal from Home revokes only that Home", test_revoke_on_removal_from_home},
      {"factory reset erases everything", test_factory_reset},
      {"legacy READERDATA migration (paired)", [] { test_legacy_migration(1); }},
      {"legacy READERDATA migration (unpaired)", [] { test_legacy_migration(0); }},
      {"corrupted / empty NVS data", test_corrupted_data},
      {"newer store format is never overwritten", test_future_version_is_never_overwritten},
      {"NVS write failure rolls back", test_write_failure_rolls_back},
      {"malformed NFC Access Control Point requests", test_malformed_access_control_requests},
      {"capacity limit", test_capacity_limit},
      {"remove reader key only affects active Home", test_remove_reader_key_only_affects_active_home},
      {"codec round trip + ECP frame", test_codec_roundtrip_and_ecp},
      {"no key material in logs", test_no_secrets_in_logs},
  };
  for (auto &t : tests) {
    int before = g_failures;
    t.fn();
    printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", t.name);
  }
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
