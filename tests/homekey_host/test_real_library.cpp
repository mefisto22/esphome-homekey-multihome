// End-to-end host test of the multi-Home store with the REAL pinned HomeKey
// library (HK_HomeKit provisioning + HKAuthenticationContext FAST/STANDARD
// flows) and the production glue in homekey_library.cpp.
//
// The "phone" below implements the device side of the HomeKey NFC protocol
// with real P-256 / ECDH / X9.63-KDF / HKDF / AES-CBC / AES-CMAC crypto, as
// documented by kormax/apple-home-key and mirrored by the library. It holds a
// HomeKey for exactly one Home (one reader group / reader public key) and,
// like a real device, rejects an AUTH1 whose reader signature does not verify
// with that Home's reader key. It is a protocol model, not an iPhone: it
// proves the store drives the unmodified library correctly across several
// Homes, it cannot prove Apple's behaviour.

#include "phone_model.h"

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  srand(20261006);  // deterministic keys / transaction ids
  fake_nvs::reset();
  Home home_a{gen_keypair(), bytes{1, 2, 3, 4, 5, 6, 7, 8}, gen_keypair()};
  Home home_b{gen_keypair(), bytes{9, 8, 7, 6, 5, 4, 3, 2}, gen_keypair()};
  Phone phone_a{"phone A", home_a.reader.pub, home_a.gid(), gen_keypair()};
  Phone phone_b{"phone B", home_b.reader.pub, home_b.gid(), gen_keypair()};

  Device d;
  d.boot(0);
  printf("[....] Home A provisions the reader (real HK_HomeKit)\n");
  CHECK(enroll(*d.store, home_a, phone_a));
  {
    auto ps = d.store->profiles_copy();
    CHECK(ps.size() == 1 && hk_store::has_reader_key(ps[0]));
    if (ps.size() != 1 || ps[0].issuers.size() != 1 || ps[0].issuers[0].endpoints.size() != 1) {
      printf("enrolment failed, aborting\n%s", test_log_buffer().c_str());
      return 1;
    }
    CHECK(ps[0].reader_gid == home_a.gid());  // gid derivation matches the protocol
    CHECK(ps[0].reader_pk == home_a.reader.pub);
    CHECK(ps[0].issuers.size() == 1 && ps[0].issuers[0].endpoints.size() == 1);
    CHECK(ps[0].issuers[0].endpoints[0].endpoint_id == phone_a.endpoint_id());
    CHECK(fake_nvs::flash()[hk_store::NVS_SCRATCH_NAMESPACE].empty());
  }
  printf("[....] phone A: first tap = STANDARD flow, second tap = FAST flow\n");
  auto r = tap(*d.store, phone_a, home_a.gid());
  CHECK(r.success && r.flow == kFlowSTANDARD && r.attempts == 1);
  CHECK(r.endpoint_id == phone_a.endpoint_id() && r.issuer_id == home_a.issuer_id());
  r = tap(*d.store, phone_a, home_a.gid());
  CHECK(r.success && r.flow == kFlowFAST);

  printf("[....] reset pairing (keep HomeKeys), reboot unpaired, Home B provisions\n");
  CHECK(d.store->archive_active_home());
  d.boot(0);
  CHECK(enroll(*d.store, home_b, phone_b));
  CHECK(hk_store::has_reader_key(d.store->profiles_copy()[0]));
  CHECK(d.store->profiles_copy()[0].reader_gid == home_a.gid());  // Home A untouched
  CHECK(d.store->ecp_frames().size() == 2);

  printf("[....] both phones, every ECP hint, phones that answer AUTH0 for any group\n");
  for (int round = 0; round < 2; round++) {
    for (const bytes &hint : {home_a.gid(), home_b.gid(), bytes{}}) {
      int a_sel = phone_a.selects, b_sel = phone_b.selects, a_fail = phone_a.flow_fail, b_fail = phone_b.flow_fail;
      auto ra = tap(*d.store, phone_a, hint);
      auto rb = tap(*d.store, phone_b, hint);
      CHECK(ra.success && ra.reader_gid == home_a.gid() && ra.endpoint_id == phone_a.endpoint_id());
      CHECK(rb.success && rb.reader_gid == home_b.gid() && rb.endpoint_id == phone_b.endpoint_id());
      CHECK(phone_a.flow_fail == a_fail && phone_b.flow_fail == b_fail);  // no premature "failed"
      if (hint == home_a.gid())
        CHECK(ra.attempts == 1 && rb.attempts == 2 && phone_b.selects == b_sel + 1);
      if (hint == home_b.gid())
        CHECK(rb.attempts == 1 && ra.attempts == 2 && phone_a.selects == a_sel + 1);
    }
    CHECK(phone_a.auth1_rejected > 0 && phone_b.auth1_rejected > 0);  // wrong-Home attempts were rejected
    if (round == 0) {
      printf("[....] reboot (= OTA for NVS) with Home B paired\n");
      d.boot(1);
    }
  }
  // After the first STANDARD flow each phone uses FAST flow with its own Home.
  r = tap(*d.store, phone_b, home_b.gid());
  CHECK(r.success && r.flow == kFlowFAST);
  r = tap(*d.store, phone_a, home_a.gid());
  CHECK(r.success && r.flow == kFlowFAST);

  printf("[....] phones that reject AUTH0 for a foreign reader group\n");
  phone_a.reject_unknown_group_in_auth0 = phone_b.reject_unknown_group_in_auth0 = true;
  r = tap(*d.store, phone_a, home_b.gid());
  CHECK(r.success && r.attempts == 2);
  r = tap(*d.store, phone_b, home_a.gid());
  CHECK(r.success && r.attempts == 2);

  printf("[....] HomeKey of an unknown Home / unknown device is rejected\n");
  Home home_x{gen_keypair(), bytes{4, 4, 4, 4, 4, 4, 4, 4}, gen_keypair()};
  Phone stranger{"stranger", home_x.reader.pub, home_x.gid(), gen_keypair()};
  int fails_before = stranger.flow_fail;
  r = tap(*d.store, stranger, home_a.gid());
  CHECK(!r.success && r.attempts == 2);
  CHECK(stranger.flow_fail > fails_before && stranger.flow_ok == 0);

  printf("[....] removal of the paired Home revokes only that Home\n");
  d.store->revoke_active_home();
  CHECK(!tap(*d.store, phone_b, {}).success);
  CHECK(tap(*d.store, phone_a, {}).success);

  printf("[....] factory reset\n");
  CHECK(d.store->erase_all());
  d.boot(0);
  CHECK(!tap(*d.store, phone_a, {}).success);

  // Secrets never reach the log, even though the stub's default level is VERBOSE.
  const std::string &log = test_log_buffer();
  for (const Home *h : {&home_a, &home_b}) {
    CHECK(log.find(hex(h->reader.priv)) == std::string::npos);
  }
  for (auto &kv : phone_a.persistent)
    CHECK(log.find(hex(kv.second)) == std::string::npos);
  CHECK(log.find("Reader Key:") == std::string::npos);
  CHECK(log.find("kenc=") == std::string::npos);
  CHECK(log.find("Shared Key") == std::string::npos);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
