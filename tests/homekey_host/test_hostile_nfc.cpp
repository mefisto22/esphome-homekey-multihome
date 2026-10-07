// Hostile-input tests for the NFC-facing code of the vendored HomeKey library.
//
// Everything a device in the NFC field sends is attacker controlled. None of
// it may crash, hang or corrupt memory on the ESP32 (where C++ exceptions are
// disabled, so a nlohmann::json type error is an abort() and a reboot).
//
// Each case runs in a forked child with a time limit, so a crash (signal,
// abort, AddressSanitizer / UBSan report) or a hang only fails that case.
// Part 1 calls the library's internal parsing steps directly (test-only
// access via "#define private public"); part 2 drives the whole stack - store,
// production glue, library - with an attacker device that completes the
// STANDARD flow with an unknown endpoint and then answers the ATTESTATION
// flow with malformed and random data.

// Standard / third-party headers first, so the access hack below only affects
// the library's own class declarations.
#include <array>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <sstream>
#include <tuple>
#include <vector>
#include <cbor.h>
#include <HomeKey.h>
#include <sys/wait.h>
#include <unistd.h>

#define private public
#include "DigitalKeySecureContext.h"
#include "ISO18013SecureContext.h"
#include "TLV8.hpp"
#include "hkAttestationAuth.h"
#include "ndef.h"
#undef private

#include "phone_model.h"

static int g_cases = 0, g_crashed = 0, g_genuine = 0, g_genuine_failed = 0;

static void run_case(const char *name, const std::function<void()> &fn, unsigned timeout_s = 20) {
  g_cases++;
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(timeout_s);  // a hang (e.g. an endless GET RESPONSE loop) is a failure too
    fn();
    fflush(stdout);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!ok) {
    g_crashed++;
    if (WIFSIGNALED(status))
      printf("[CRASH] %s (signal %d%s)\n", name, WTERMSIG(status), WTERMSIG(status) == SIGALRM ? ": hang" : "");
    else
      printf("[CRASH] %s (exit %d)\n", name, WEXITSTATUS(status));
  } else {
    printf("[ OK  ] %s\n", name);
  }
}

// Heap copy of exactly the given size, so AddressSanitizer sees any overread.
static bytes exact(const bytes &b) { return bytes(b.begin(), b.end()); }

using json = nlohmann::json;
static bytes cbor(const json &j) { return json::to_cbor(j); }
static json bin(const bytes &b) { return json::binary(b); }

// Owns everything HKAttestationAuth keeps references to.
struct AttRig {
  std::vector<hkIssuer_t> issuers;
  uint8_t vol[48] = {0};
  DigitalKeySecureContext dks;
  std::function<bool(bytes &, bytes &, bool)> nfc;
  HKAttestationAuth att;
  explicit AttRig(std::function<bool(bytes &, bytes &, bool)> fn)
      : issuers(1), dks(vol), nfc(std::move(fn)), att(issuers, dks, nfc) {
    issuers[0].issuer_id = bytes(8, 0x11);
  }
};

// ------------------------------------------------------------------ part 1 --
static void parser_cases() {
  run_case("TLV8: value longer than the buffer", [] {
    bytes b = exact({0x86, 0x41, 0x01, 0x02, 0x03});
    TLV8 t;
    t.parse(b.data(), b.size());
  });
  run_case("TLV8: lone tag byte", [] {
    bytes b = exact({0x86});
    TLV8 t;
    t.parse(b.data(), b.size());
  });
  run_case("TLV8 (BER): huge long-form length", [] {
    bytes b = exact({0x53, 0x84, 0xFF, 0xFF, 0xFF, 0xF0, 0x00});
    TLV8 t(true);
    t.parse(b.data(), b.size());
  });
  run_case("TLV8 (BER): truncated long-form length", [] {
    bytes b = exact({0x53, 0x82, 0x01});
    TLV8 t(true);
    t.parse(b.data(), b.size());
  });

  run_case("NDEF: record longer than the message", [] {
    bytes b = exact({0xD1, 0x30, 0x40, 'a', 'b'});
    NDEFMessage m(b.data(), b.size());
    m.unpack();
  });
  run_case("NDEF: long (non-SR) record header cut off", [] {
    bytes b = exact({0xC1, 0x01});
    NDEFMessage m(b.data(), b.size());
    m.unpack();
  });
  run_case("NDEF: findType of a missing record", [] {
    bytes b = exact({0xD1, 0x01, 0x01, 'T', 0x00});
    NDEFMessage m(b.data(), b.size());
    m.unpack();
    NDEFRecord *r = m.findType("iso.org:18013:deviceengagement");
    if (r != nullptr)
      _exit(3);
  });

  run_case("DigitalKeySecureContext: responses of every short length", [] {
    uint8_t vol[48] = {0};
    for (size_t n = 0; n <= 40; n++) {
      DigitalKeySecureContext ctx(vol);
      bytes b = exact(bytes(n, 0x5A));
      ctx.decrypt_response(b.data(), b.size());
    }
  });

  auto iso = [] {
    return ISO18013SecureContext(bytes(32, 1), bytes(32, 2), 16);
  };
  run_case("ISO18013: envelope that is not CBOR", [iso] { iso().decryptMessageFromEndpoint(exact({0xFF, 0x00})); });
  run_case("ISO18013: CBOR without \"data\"", [iso] { iso().decryptMessageFromEndpoint(cbor({{"x", 1}})); });
  run_case("ISO18013: CBOR array instead of map", [iso] { iso().decryptMessageFromEndpoint(cbor(json::array({1, 2}))); });
  run_case("ISO18013: ciphertext shorter than the GCM tag", [iso] {
    iso().decryptMessageFromEndpoint(cbor({{"data", bin(bytes(5, 7))}}));
  });
  run_case("ISO18013: empty input", [iso] { iso().decryptMessageFromEndpoint(bytes()); });

  // verify(): the decrypted attestation document (attacker controlled once the
  // attacker completed the key agreement, which needs no secret).
  auto verify = [](const json &doc) {
    std::vector<hkIssuer_t> issuers(1);
    issuers[0].issuer_id = bytes(8, 0x11);
    issuers[0].issuer_pk = bytes(32, 0x22);
    uint8_t vol[48] = {0};
    DigitalKeySecureContext dks(vol);
    std::function<bool(bytes &, bytes &, bool)> nfc = [](bytes &, bytes &, bool) { return false; };
    HKAttestationAuth att(issuers, dks, nfc);
    bytes c = cbor(doc);
    att.verify(c);
  };
  auto valid_mso = [](const json &device_key) {
    json mso = {{"deviceKeyInfo", {{"deviceKey", device_key}}}};
    bytes inner = json::to_cbor(mso);
    // issuerAuth[2] is a CBOR byte string holding tag-24 encoded CBOR.
    json tagged = json::binary(inner, 24);
    return json::to_cbor(tagged);
  };
  run_case("verify: not CBOR", [] {
    std::vector<hkIssuer_t> issuers;
    uint8_t vol[48] = {0};
    DigitalKeySecureContext dks(vol);
    std::function<bool(bytes &, bytes &, bool)> nfc = [](bytes &, bytes &, bool) { return false; };
    HKAttestationAuth att(issuers, dks, nfc);
    bytes c = exact({0xFF, 0xFF});
    att.verify(c);
  });
  run_case("verify: documents is not an array", [verify] { verify({{"documents", 5}}); });
  run_case("verify: document is not a map", [verify] { verify({{"documents", json::array({5})}}); });
  run_case("verify: issuerSigned missing", [verify] { verify({{"documents", json::array({json::object()})}}); });
  run_case("verify: issuerAuth[1] is not a map", [verify] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), "x", bin({2}), bin({3})})}}}}})}});
  });
  run_case("verify: short issuer id and signature", [verify] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), {{"4", bin({0x11})}}, bin({2}),
                                                                         bin({3})})}}}}})}});
  });
  run_case("verify: payload is not tagged CBOR", [verify] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), {{"4", bin(bytes(8, 0x11))}},
                                                                         bin({0x01, 0x02}), bin(bytes(64, 3))})}}}}})}});
  });
  run_case("verify: payload is garbage", [verify] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), {{"4", bin(bytes(8, 0x11))}},
                                                                         bin({0xFF, 0xFF, 0xFF}), bin(bytes(64, 3))})}}}}})}});
  });
  run_case("verify: device key missing", [verify, valid_mso] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), {{"4", bin(bytes(8, 0x11))}},
                                                                         bin(valid_mso(json::object())),
                                                                         bin(bytes(64, 3))})}}}}})}});
  });
  run_case("verify: device key coordinates of the wrong type", [verify, valid_mso] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), {{"4", bin(bytes(8, 0x11))}},
                                                                         bin(valid_mso({{"-2", 1}, {"-3", "y"}})),
                                                                         bin(bytes(64, 3))})}}}}})}});
  });
  run_case("verify: known issuer, short signature and key", [verify, valid_mso] {
    verify({{"documents", json::array({{{"issuerSigned",
                                          {{"issuerAuth", json::array({bin({1}), {{"4", bin(bytes(8, 0x11))}},
                                                                         bin(valid_mso({{"-2", bin({1})}, {"-3", bin({2})}})),
                                                                         bin({3})})}}}}})}});
  });

  // The hardening must not reject a genuine document: a well-formed one is
  // accepted, the COSE Sig_structure handed to Ed25519 is exactly
  // ["Signature1", protected, h'', payload], and the device key comes back.
  auto genuine = [valid_mso](const char *name, size_t ph_len, size_t padding) {
    g_genuine++;
    bytes ph(ph_len, 0xA1), sig(64, 0x5A), x(32, 0x0A), y(32, 0x0B);
    json mso = {{"deviceKeyInfo", {{"deviceKey", {{"1", 2}, {"-1", 1}, {"-2", bin(x)}, {"-3", bin(y)}}}}},
                {"padding", bin(bytes(padding, 0x33))}};
    bytes payload = json::to_cbor(json::binary(json::to_cbor(mso), 24));
    bytes issuer_id = bytes(8, 0x11);
    issuer_id.push_back(0x99);  // upstream matches the stored id as a prefix
    json doc = {{"version", "1.0"},
                {"documents", json::array({{{"docType", "com.apple.HomeKit.1.credential"},
                                            {"issuerSigned", {{"issuerAuth", json::array({bin(ph), {{"4", bin(issuer_id)}},
                                                                                           bin(payload), bin(sig)})}}}}})},
                {"status", 0}};
    std::vector<hkIssuer_t> issuers(2);
    issuers[0].issuer_id = bytes(8, 0x44);
    issuers[0].issuer_pk = bytes(32, 0x55);
    issuers[1].issuer_id = bytes(8, 0x11);
    issuers[1].issuer_pk = bytes(32, 0x22);
    bytes expected = json::to_cbor(json::array({"Signature1", bin(ph), bin({}), bin(payload)}));
    bool hook_ok = false;
    g_ed25519_verify_hook = [&](const unsigned char *s, const unsigned char *m, unsigned long long mlen,
                                const unsigned char *pk) {
      hook_ok = bytes(s, s + 64) == sig && bytes(m, m + mlen) == expected && bytes(pk, pk + 32) == issuers[1].issuer_pk;
      return hook_ok ? 0 : -1;
    };
    uint8_t vol[48] = {0};
    DigitalKeySecureContext dks(vol);
    std::function<bool(bytes &, bytes &, bool)> nfc = [](bytes &, bytes &, bool) { return false; };
    HKAttestationAuth att(issuers, dks, nfc);
    bytes c = cbor(doc);
    auto [issuer, key] = att.verify(c);
    g_ed25519_verify_hook = nullptr;
    bytes want_key{0x04};
    want_key.insert(want_key.end(), x.begin(), x.end());
    want_key.insert(want_key.end(), y.begin(), y.end());
    bool ok = hook_ok && issuer == &issuers[1] && key == want_key;
    if (!ok)
      g_genuine_failed++;
    printf("[%s] genuine attestation accepted: %s\n", ok ? " OK  " : "FAIL ", name);
  };
  genuine("typical size", 3, 200);
  genuine("long protected header", 40, 300);
  genuine("payload over 64 KB", 40, 70000);

  run_case("attestation_salt: envelope without NDEF", [] {
    AttRig rig([](bytes &, bytes &, bool) { return false; });
    bytes env1 = exact({0x90, 0x00}), cmd = exact({0x01});
    rig.att.attestation_salt(env1, cmd);
  });
  run_case("attestation_salt: NDEF without device engagement", [] {
    AttRig rig([](bytes &, bytes &, bool) { return false; });
    bytes env1 = exact({0x53, 0x05, 0xD1, 0x01, 0x01, 'T', 0x00, 0x90, 0x00}), cmd = exact({0x01});
    rig.att.attestation_salt(env1, cmd);
  });
  run_case("attestation_salt: oversized NDEF", [] {
    AttRig rig([](bytes &, bytes &, bool) { return false; });
    const char *type = "iso.org:18013:deviceengagement";
    bytes rec{0xD4, (uint8_t) strlen(type), 200};
    rec.insert(rec.end(), type, type + strlen(type));
    rec.insert(rec.end(), 200, 0x42);
    bytes env1{0x53, (uint8_t) rec.size()};
    env1.insert(env1.end(), rec.begin(), rec.end());
    env1.push_back(0x90);
    env1.push_back(0x00);
    bytes cmd(250, 0x01);
    rig.att.attestation_salt(env1, cmd);
  });
  run_case("envelope1: device answers nothing", [] {
    AttRig rig([](bytes &, bytes &r, bool) {
      r.clear();
      return false;
    });
    rig.att.envelope1Cmd();
  });
  run_case("envelope1: one-byte answers", [] {
    AttRig rig([](bytes &, bytes &r, bool) {
      r = {0x90};
      return true;
    });
    rig.att.envelope1Cmd();
  });
  run_case("envelope2: endless GET RESPONSE chain", [] {
    AttRig rig([](bytes &, bytes &r, bool) {
      r = bytes(250, 0x00);
      r.push_back(0x61);
      r.push_back(0x00);
      return true;
    });
    bytes salt(32, 1);
    rig.att.envelope2Cmd(salt);
  });
  run_case("envelope2: package without encrypted message", [] {
    AttRig rig([](bytes &, bytes &r, bool) {
      r = {0x90, 0x00};
      return true;
    });
    bytes salt(32, 1);
    rig.att.envelope2Cmd(salt);
  });
  run_case("envelope2: encrypted message is not CBOR", [] {
    AttRig rig([](bytes &, bytes &r, bool) {
      r = {0x53, 0x02, 0xFF, 0xFF, 0x90, 0x00};
      return true;
    });
    bytes salt(32, 1);
    rig.att.envelope2Cmd(salt);
  });
}

// ------------------------------------------------------------------ part 2 --
struct Rig {
  Device d;
  Home home{gen_keypair(), bytes{1, 2, 3, 4, 5, 6, 7, 8}, gen_keypair()};
  Phone owner{"owner", home.reader.pub, home.gid(), gen_keypair()};
  Rig() {
    fake_nvs::reset();
    d.boot(0);
    enroll(*d.store, home, owner);
  }
};

// Wraps a phone so single responses can be replaced.
static HomeKeyAuthResult tap_with(HomeKeyStore &store, const std::function<bytes(const bytes &)> &device) {
  HKTransceive t = [&device](bytes &send, bytes &recv, bool) {
    recv = exact(device(send));
    return !recv.empty();
  };
  return store.authenticate(t, {}, SELECT);
}

static void stack_cases() {
  run_case("AUTH0: endpoint key TLV longer than the response", [] {
    Rig r;
    auto res = tap_with(*r.d.store, [&](const bytes &apdu) -> bytes {
      if (apdu[1] == 0x80) {
        bytes b{0x86, 0x7F};
        b.insert(b.end(), 65, 0x04);
        return b;
      }
      return r.owner.transceive(apdu);
    });
    if (res.success)
      _exit(4);
  });
  run_case("AUTH0: no cryptogram", [] {
    Rig r;
    tap_with(*r.d.store, [&](const bytes &apdu) -> bytes {
      bytes b = r.owner.transceive(apdu);
      if (apdu[1] == 0x80 && b.size() > 67)
        return cat({bytes(b.begin(), b.begin() + 67), {0x90, 0x00}});
      return b;
    });
  });
  run_case("AUTH0: 2-byte cryptogram", [] {
    Rig r;
    tap_with(*r.d.store, [&](const bytes &apdu) -> bytes {
      bytes b = r.owner.transceive(apdu);
      if (apdu[1] == 0x80 && b.size() > 67)
        return cat({bytes(b.begin(), b.begin() + 67), {0x9D, 0x02, 0x01, 0x02, 0x90, 0x00}});
      return b;
    });
  });
  run_case("AUTH1: one-byte response", [] {
    Rig r;
    tap_with(*r.d.store, [&](const bytes &apdu) -> bytes {
      if (apdu[1] == 0x81) {
        r.owner.transceive(apdu);
        return {0x01, 0x90, 0x00};
      }
      return r.owner.transceive(apdu);
    });
  });
  run_case("control flow: device leaves before the final status", [] {
    Rig r;
    tap_with(*r.d.store, [&](const bytes &apdu) -> bytes {
      if (apdu[1] == 0x3C)
        return {};
      return r.owner.transceive(apdu);
    });
  });

  // Attacker reaches the ATTESTATION flow and answers with malformed data.
  auto attacker_case = [](const char *name, std::function<bytes(const bytes &, int)> answer) {
    run_case(name, [answer] {
      Rig r;
      Phone evil{"attacker", bytes(), r.home.gid(), gen_keypair()};
      evil.attacker = true;
      int step = 0;
      evil.on_other = [&](const bytes &apdu) { return answer(apdu, step++); };
      tap_with(*r.d.store, [&](const bytes &apdu) { return evil.transceive(apdu); });
      // The legitimate owner must still be able to unlock afterwards.
      if (!tap(*r.d.store, r.owner, r.home.gid()).success)
        _exit(5);
    });
  };
  attacker_case("attacker: every attestation answer is empty", [](const bytes &, int) { return bytes(); });
  attacker_case("attacker: every answer is a bare 90 00", [](const bytes &, int) { return bytes{0x90, 0x00}; });
  attacker_case("attacker: endless 61 00 chain", [](const bytes &, int) {
    bytes b(250, 0x00);
    b.push_back(0x61);
    b.push_back(0x00);
    return b;
  });
  attacker_case("attacker: envelope 2 with a non-CBOR message", [](const bytes &apdu, int) -> bytes {
    if (apdu.size() >= 2 && apdu[0] == 0x00 && apdu[1] == 0xC0)
      return {0x53, 0x03, 0xFF, 0xFF, 0xFF, 0x90, 0x00};
    if (apdu.size() >= 2 && apdu[1] == 0xC3 && apdu[3] == 0x01) {
      // envelope 1: a plausible NDEF with a device engagement record
      const char *type = "iso.org:18013:deviceengagement";
      bytes rec{0xD4, (uint8_t) strlen(type), 3};
      rec.insert(rec.end(), type, type + strlen(type));
      rec.insert(rec.end(), {1, 2, 3});
      return cat({{0x53, (uint8_t) rec.size()}, rec, {0x90, 0x00}});
    }
    return {0x90, 0x00};
  });
  attacker_case("attacker: envelope 2 CBOR without data", [](const bytes &apdu, int) -> bytes {
    if (apdu.size() >= 2 && apdu[0] == 0x00 && apdu[1] == 0xC0) {
      bytes c = cbor({{"x", 1}});
      return cat({{0x53, (uint8_t) c.size()}, c, {0x90, 0x00}});
    }
    if (apdu.size() >= 2 && apdu[1] == 0xC3 && apdu[3] == 0x01) {
      const char *type = "iso.org:18013:deviceengagement";
      bytes rec{0xD4, (uint8_t) strlen(type), 3};
      rec.insert(rec.end(), type, type + strlen(type));
      rec.insert(rec.end(), {1, 2, 3});
      return cat({{0x53, (uint8_t) rec.size()}, rec, {0x90, 0x00}});
    }
    return {0x90, 0x00};
  });

  run_case("fuzz: 400 attacker sessions with random attestation answers", [] {
    for (int i = 0; i < 400; i++) {
      Rig r;
      Phone evil{"attacker", bytes(), r.home.gid(), gen_keypair()};
      evil.attacker = true;
      evil.on_other = [](const bytes &) {
        size_t n = rand() % 300;
        bytes b(n);
        for (auto &x : b)
          x = rand();
        if (rand() % 4 != 0) {  // usually claim success
          b.push_back(0x90);
          b.push_back(0x00);
        }
        if (rand() % 8 == 0 && b.size() > 2) {  // sometimes a valid-looking TLV
          b[0] = (rand() % 2) ? 0x53 : 0x90;
          b[1] = rand();
        }
        return b;
      };
      tap_with(*r.d.store, [&](const bytes &apdu) { return evil.transceive(apdu); });
    }
  }, 300);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  srand(20261006);
  printf("--- part 1: library parsing steps ---\n");
  parser_cases();
  printf("--- part 2: whole stack with a hostile device ---\n");
  stack_cases();
  printf("\n%d cases, %d crashed or hung; %d genuine documents, %d rejected\n", g_cases, g_crashed, g_genuine,
         g_genuine_failed);
  return g_crashed == 0 && g_genuine_failed == 0 ? 0 : 1;
}
