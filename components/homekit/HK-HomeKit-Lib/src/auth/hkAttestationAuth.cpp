#include "hkAttestationAuth.h"
#include "esp_system.h"
#include "ndef.h"
#include "simple_tlv.h"
#include "TLV8.hpp"
#include "ISO18013SecureContext.h"
#include "logging.h"
#include <cstdint>
#include <esp_random.h>
#include <sodium/crypto_sign_ed25519.h>
#include <mbedtls/sha256.h>
#include <mbedtls/error.h>
#include <cbor.h>
#include <vector>

std::vector<unsigned char> HKAttestationAuth::attestation_salt(std::vector<unsigned char> &env1Data, std::vector<unsigned char> &readerCmd)
{
  TLV8 env1ResTlv;
  env1ResTlv.parse(env1Data.data(), env1Data.size());
  tlv_it tlvEnv1Ndef = env1ResTlv.find(kNDEF_MESSAGE);
  if (tlvEnv1Ndef == env1ResTlv.end()) {  // HAP-ESPHome patch
    LOG(W, "Envelope 1 has no NDEF message");
    return std::vector<unsigned char>();
  }
  std::vector<uint8_t> env1Ndef = tlvEnv1Ndef->value;
  NDEFMessage ndefEnv1Ctx = NDEFMessage(env1Ndef.data(), env1Ndef.size());
  auto ndefEnv1Data = ndefEnv1Ctx.unpack();
  auto ndefEnv1Pack = ndefEnv1Ctx.pack();
  NDEFRecord* res_eng = ndefEnv1Ctx.findType("iso.org:18013:deviceengagement");
  if (res_eng == nullptr || res_eng->data.size() < 1) {  // HAP-ESPHome patch
    LOG(W, "Envelope 1 has no device engagement record");
    return std::vector<unsigned char>();
  }
  // HAP-ESPHome patch: env1Ndef, readerCmd and the device-engagement record
  // are all attacker-sized, so size the CBOR buffers from the actual inputs
  // and bail if the encoder still reports an overflow, instead of hashing over
  // a fixed 255-byte stack buffer that a large NDEF overruns.
  std::vector<uint8_t> devEngCbor(res_eng->data.size() + env1Ndef.size() + readerCmd.size() + 32);
  CborEncoder devEng;
  CborEncoder devEngArray;
  cbor_encoder_init(&devEng, devEngCbor.data(), devEngCbor.size(), 0);
  cbor_encoder_create_array(&devEng, &devEngArray, 2);
  cbor_encode_tag(&devEngArray, CborEncodedCborTag);
  cbor_encode_byte_string(&devEngArray, res_eng->data.data(), res_eng->data.size() - 1);
  CborEncoder innerArray;
  cbor_encoder_create_array(&devEngArray, &innerArray, 2);
  cbor_encode_byte_string(&innerArray, env1Ndef.data(), env1Ndef.size());
  cbor_encode_byte_string(&innerArray, readerCmd.data(), readerCmd.size());
  cbor_encoder_close_container(&devEngArray, &innerArray);
  int devErr = cbor_encoder_close_container(&devEng, &devEngArray);
  if (devErr != CborNoError || cbor_encoder_get_extra_bytes_needed(&devEng) != 0) {
    LOG(E, "Device engagement CBOR encode failed (%d)", devErr);
    return std::vector<unsigned char>();
  }
  size_t devSize = cbor_encoder_get_buffer_size(&devEng, devEngCbor.data());
  LOG(D, "Device Engagement CBOR");
  std::vector<uint8_t> buf(devSize + 32);
  CborEncoder root;
  cbor_encoder_init(&root, buf.data(), buf.size(), 0);
  cbor_encode_tag(&root, CborEncodedCborTag);
  int rootErr = cbor_encode_byte_string(&root, devEngCbor.data(), devSize);
  if (rootErr != CborNoError || cbor_encoder_get_extra_bytes_needed(&root) != 0) {
    LOG(E, "Device engagement root CBOR encode failed (%d)", rootErr);
    return std::vector<unsigned char>();
  }
  size_t rootSize = cbor_encoder_get_buffer_size(&root, buf.data());
  LOG(D, "NDEF CBOR");

  LOG(D, "CBOR MATERIAL DATA: %s", red_log::bufToHexString(buf.data(), rootSize).c_str());

  std::vector<uint8_t> salt(32);
  int shaRet = mbedtls_sha256(buf.data(), rootSize, salt.data(), false);

  if (shaRet != 0)
  {
      LOG(E, "SHA256 Failed - %s", mbedtls_high_level_strerr(shaRet));
      return std::vector<unsigned char>();
  }

  LOG(D, "ATTESTATION SALT: %s", red_log::bufToHexString(salt.data(), salt.size()).c_str());

  return salt;
}

std::tuple<std::vector<uint8_t>, std::vector<uint8_t>> HKAttestationAuth::envelope1Cmd()
{
  std::vector<uint8_t> ctrlFlow = {0x80, 0x3c, 0x40, 0xa0};
  std::vector<uint8_t> ctrlFlowRes;
  // uint16_t ctrlFlowResLen = 8;
  nfc(ctrlFlow, ctrlFlowRes, false);
  LOG(D, "CTRL FLOW RES LENGTH: %d, DATA: %s", ctrlFlowRes.size(), red_log::bufToHexString(ctrlFlowRes.data(), ctrlFlowRes.size()).c_str());
  if (ctrlFlowRes.size() >= 2 && ctrlFlowRes[0] == 0x90 && ctrlFlowRes[1] == 0x0)  // HAP-ESPHome patch
  { // cla=0x00; ins=0xa4; p1=0x04; p2=0x00; lc=0x07(7); data=a0000008580102; le=0x00
    std::vector<uint8_t> data = {0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x02, 0x0};
    std::vector<uint8_t> response;
    // uint16_t responseLength = 4;
    nfc(data, response, false);
    LOG(D, "ENV1.2 RES LENGTH: %d, DATA: %s", response.size(), red_log::bufToHexString(response.data(), response.size()).c_str());
    if (response.size() >= 2 && response[0] == 0x90 && response[1] == 0x0){  // HAP-ESPHome patch
      unsigned char payload[] = {0x15, 0x91, 0x02, 0x02, 0x63, 0x72, 0x01, 0x02, 0x51, 0x02, 0x11, 0x61, 0x63, 0x01, 0x03, 0x6e, 0x66, 0x63, 0x01, 0x0a, 0x6d, 0x64, 0x6f, 0x63, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72};
      unsigned char payload1[] = {0x01};
      unsigned char payload2[] = {0xa2, 0x00, 0x63, 0x31, 0x2e, 0x30, 0x20, 0x81, 0x29};
      auto ndefMessage = NDEFMessage({NDEFRecord("", 0x01, "Hr", payload, sizeof(payload)),
                                      NDEFRecord("nfc", 0x04, "iso.org:18013:nfc", payload1, 1),
                                      NDEFRecord("mdocreader", 0x04, "iso.org:18013:readerengagement", payload2, sizeof(payload2))})
                            .pack();
      LOG(D, "NDEF CMD LENGTH: %d, DATA: %s", ndefMessage.size(), red_log::bufToHexString(ndefMessage.data(), ndefMessage.size()).c_str());
      auto envelope1Tlv = simple_tlv(0x53, ndefMessage.data(), ndefMessage.size(), NULL, NULL);
      std::vector<uint8_t> env1Apdu = {0x00, 0xc3, 0x00, 0x01, static_cast<uint8_t>(envelope1Tlv.size())};
      env1Apdu.reserve(envelope1Tlv.size() + 6);
      // memcpy(env1Apdu.data() + 5, envelope1Tlv.data(), envelope1Tlv.size());
      env1Apdu.insert(env1Apdu.end(), envelope1Tlv.begin(), envelope1Tlv.end());
      env1Apdu.push_back(0x0);
      LOG(D, "APDU CMD LENGTH: %d, DATA: %s", env1Apdu.size(), red_log::bufToHexString(env1Apdu.data(), env1Apdu.size()).c_str());
      std::vector<uint8_t> env1Res;
      // uint16_t env1ResLen = 128;
      nfc(env1Apdu, env1Res, false);
      LOG(D, "APDU RES LENGTH: %d, DATA: %s", env1Res.size(), red_log::bufToHexString(env1Res.data(), env1Res.size()).c_str());
      if (env1Res.size() >= 2 && env1Res[env1Res.size() - 2] == 0x90 && env1Res[env1Res.size() - 1] == 0x0){  // HAP-ESPHome patch
        return std::make_tuple(env1Res, ndefMessage);
      }
    }
  }
  return std::make_tuple(std::vector<uint8_t>(), std::vector<uint8_t>());
}

std::vector<unsigned char> HKAttestationAuth::envelope2Cmd(std::vector<uint8_t> &salt)
{
  ISO18013SecureContext secureCtx = ISO18013SecureContext(attestation_exchange_common_secret, salt, 16);

  uint8_t doctype[150];
  CborEncoder docType;
  CborEncoder docMap;
  cbor_encoder_init(&docType, doctype, 150, 0);
  cbor_encoder_create_map(&docType, &docMap, 2);
  cbor_encode_text_stringz(&docMap, "docType");
  cbor_encode_text_stringz(&docMap, "com.apple.HomeKit.1.credential");
  cbor_encode_text_stringz(&docMap, "nameSpaces");
  CborEncoder namespaces;
  CborEncoder homeCred;
  cbor_encoder_create_map(&docMap, &namespaces, 1);
  cbor_encode_text_stringz(&namespaces, "com.apple.HomeKit");
  cbor_encoder_create_map(&namespaces, &homeCred, 1);
  cbor_encode_text_stringz(&homeCred, "credential_id");
  cbor_encode_boolean(&homeCred, false);
  cbor_encoder_close_container(&namespaces, &homeCred);
  cbor_encoder_close_container(&docMap, &namespaces);
  cbor_encoder_close_container(&docType, &docMap);

  LOG(V, "ENV2 CBOR");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, doctype, cbor_encoder_get_buffer_size(&docType, doctype), ESP_LOG_VERBOSE);

  uint8_t docBuf[150];
  CborEncoder doc;
  cbor_encoder_init(&doc, docBuf, sizeof(docBuf), 0);
  CborEncoder docReq;
  cbor_encoder_create_map(&doc, &docReq, 2);
  cbor_encode_text_stringz(&docReq, "docRequests");
  CborEncoder docArray;
  cbor_encoder_create_array(&docReq, &docArray, 1);
  CborEncoder itemMap;
  cbor_encoder_create_map(&docArray, &itemMap, 1);
  cbor_encode_text_stringz(&itemMap, "itemsRequest");
  cbor_encode_tag(&itemMap, CborEncodedCborTag);
  cbor_encode_byte_string(&itemMap, doctype, cbor_encoder_get_buffer_size(&docType, doctype));
  cbor_encoder_close_container(&docArray, &itemMap);
  cbor_encoder_close_container(&docReq, &docArray);
  cbor_encode_text_stringz(&docReq, "version");
  cbor_encode_text_stringz(&docReq, "1.0");
  cbor_encoder_close_container(&doc, &docReq);
  size_t docSize = cbor_encoder_get_buffer_size(&doc, docBuf);
  LOG(V, "ENV2 CBOR");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, docBuf, docSize, ESP_LOG_VERBOSE);
  auto encrypted = secureCtx.encryptMessageToEndpoint(std::vector<uint8_t>(docBuf, docBuf + docSize));
  if(encrypted.size() > 0){
    LOG(D, "ENC DATA: %s", red_log::bufToHexString(encrypted.data(), encrypted.size()).c_str());

    auto tlv = simple_tlv(0x53, encrypted.data(), encrypted.size());

    std::vector<uint8_t> apdu = {0x0, 0xC3, 0x0, 0x0, (unsigned char)tlv.size()};

    apdu.insert(apdu.end(), tlv.begin(), tlv.end());
    LOG(D, "ENV2 APDU - LENGTH: %d, DATA: %s\n", apdu.size(), red_log::bufToHexString(apdu.data(), apdu.size()).c_str());
    std::vector<uint8_t> env2Res;
    std::vector<uint8_t> attestation_package;
    std::vector<uint8_t> dataStatus;
    std::vector<uint8_t> getData = {0x0, 0xc0, 0x0, 0x0, 0x0};
    LOG(D, "ENV2 APDU Len: %d, Data: %s\n", apdu.size(), red_log::bufToHexString(apdu.data(), apdu.size()).c_str());
    nfc(apdu, dataStatus, false);
    bool getMore = false;
    // HAP-ESPHome patch: a malicious device could keep the GET RESPONSE chain
    // going forever (hanging the main loop) or exhaust the heap. Stop after a
    // generous bound, far above any genuine attestation package.
    size_t guardIterations = 0;
    do
    {
      if (++guardIterations > 256 || attestation_package.size() > 32768) {
        LOG(W, "Attestation response too long, aborting");
        return std::vector<uint8_t>();
      }
      getMore = false;
      nfc(getData, env2Res, false);
      attestation_package.insert(attestation_package.end(), env2Res.begin(), env2Res.end());
      LOG(D, "Data Length: %d - pkg length: %d - free heap size: %lu", env2Res.size(), attestation_package.size(), esp_get_free_heap_size());
      if(env2Res.size() >= 250 && (*(&env2Res.back() - 1) == 0x61 && (env2Res.back() == 0x0 || env2Res.back() >= 0xd0))){
        getMore = true;
        attestation_package.pop_back();
        attestation_package.pop_back();
      } else if (env2Res.size() >= 250) {
        nfc(getData, env2Res, false);
        if (env2Res.size() == 2 && env2Res[0] == 0x61) {
          getMore = true;
        } else if (env2Res.size() > 200) {
          attestation_package.insert(attestation_package.end(), env2Res.begin(),
          env2Res.end());
        }
      }
      env2Res.clear();
    } while (getMore);
    LOG(D, "ATT PKG LENGTH: %d - DATA: %s", attestation_package.size(), red_log::bufToHexString(attestation_package.data(), attestation_package.size()).c_str());
    TLV8 data(true);
    data.parse(attestation_package.data(), attestation_package.size());
    tlv_it tlvStatus = data.find(0x90);
    tlv_it tlvEncMsg = data.find(0x53);
    if (tlvStatus != data.end() && tlvEncMsg != data.end()) {  // HAP-ESPHome patch
      std::vector<uint8_t> encryptedMessage = tlvEncMsg->value;
      auto decrypted_message = secureCtx.decryptMessageFromEndpoint(encryptedMessage);
      if(decrypted_message.size() > 0){
        return decrypted_message;
      }
    }
  }
  return std::vector<uint8_t>();
}

std::tuple<hkIssuer_t*, std::vector<uint8_t>> HKAttestationAuth::verify(std::vector<uint8_t>& decryptedCbor) {
  // HAP-ESPHome patch: the attestation document is attacker controlled once a
  // device completes the key agreement (which needs no secret). Navigate the
  // CBOR without .at()/operator[] (they throw, and exceptions are disabled on
  // ESP-IDF, so a throw aborts and reboots); every lookup and type is checked.
  // Parsing stays non-strict like upstream (trailing bytes are tolerated); only
  // exceptions are turned off.
  hkIssuer_t* foundIssuer = nullptr;
  std::vector<uint8_t> protectedHeaders;
  std::vector<uint8_t> issuerId;
  std::vector<uint8_t> data;
  std::vector<uint8_t> devicePubKey;
  std::vector<uint8_t> signature;

  auto find = [](const json& j, const char* key) -> const json* {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
  };
  auto at = [](const json& j, size_t i) -> const json* {
    if (!j.is_array() || i >= j.size()) return nullptr;
    return &j[i];
  };

  json root = json::from_cbor(decryptedCbor, false, false, nlohmann::json::cbor_tag_handler_t::store);
  if (root.is_discarded()) goto err;
  {
    const json* documents = find(root, "documents");
    if (!documents || !documents->is_array() || documents->empty()) goto err;
    const json* doc0 = at(*documents, 0);
    const json* issuerSigned = doc0 ? find(*doc0, "issuerSigned") : nullptr;
    const json* issuerAuth = issuerSigned ? find(*issuerSigned, "issuerAuth") : nullptr;
    if (!issuerAuth || !issuerAuth->is_array() || issuerAuth->size() < 4) goto err;
    const json* ph = at(*issuerAuth, 0);
    const json* ih = at(*issuerAuth, 1);
    const json* payload = at(*issuerAuth, 2);
    const json* sig = at(*issuerAuth, 3);
    if (!ph || !ph->is_binary() || !sig || !sig->is_binary()) goto err;
    protectedHeaders = ph->get_binary();
    signature = sig->get_binary();
    const json* iid = ih ? find(*ih, "4") : nullptr;
    if (!iid || !iid->is_binary()) goto err;
    issuerId = iid->get_binary();

    if (!payload || !payload->is_binary()) goto err;
    data = payload->get_binary();
    json c = json::from_cbor(data, false, false, json::cbor_tag_handler_t::store);
    if (c.is_discarded() || !c.is_binary() || !c.get_binary().has_subtype()) goto err;

    json d = json::from_cbor(c.get_binary(), false, false, json::cbor_tag_handler_t::store);
    if (d.is_discarded()) goto err;
    const json* dki = find(d, "deviceKeyInfo");
    const json* dk = dki ? find(*dki, "deviceKey") : nullptr;
    const json* kx = dk ? find(*dk, "-2") : nullptr;
    const json* ky = dk ? find(*dk, "-3") : nullptr;
    if (!kx || !kx->is_binary() || !ky || !ky->is_binary()) goto err;
    const auto& deviceKeyX = kx->get_binary();
    const auto& deviceKeyY = ky->get_binary();
    if (deviceKeyX.size() != 32 || deviceKeyY.size() != 32) goto err;
    devicePubKey.push_back(0x04);
    devicePubKey.insert(devicePubKey.end(), deviceKeyX.begin(), deviceKeyX.end());
    devicePubKey.insert(devicePubKey.end(), deviceKeyY.begin(), deviceKeyY.end());
  }

    for (auto &&issuer : issuers)
    {
      // Upstream compares issuer_id against the start of issuerId; keep that,
      // but never read past a shorter issuerId.
      if (!issuer.issuer_id.empty() && issuerId.size() >= issuer.issuer_id.size() &&
          std::equal(issuer.issuer_id.begin(), issuer.issuer_id.end(), issuerId.begin())) {
        LOG(D, "Found Issuer: %s", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str());
        foundIssuer = &issuer;
      }
    }

  if (foundIssuer != nullptr) {
      CborEncoder package;
      // HAP-ESPHome patch: room for every CBOR header (was +8, too tight once
      // the payload needs a 4-byte length); an overflow is rejected below.
      std::vector<uint8_t> packageBuf(strlen("Signature1") + protectedHeaders.size() + data.size() + 32);
      cbor_encoder_init(&package, packageBuf.data(), packageBuf.size(), 0);
      CborEncoder packageArray;
      cbor_encoder_create_array(&package, &packageArray, 4);
      cbor_encode_text_stringz(&packageArray, "Signature1");
      cbor_encode_byte_string(&packageArray, protectedHeaders.data(), protectedHeaders.size());
      cbor_encode_byte_string(&packageArray, {}, 0);
      cbor_encode_byte_string(&packageArray, data.data(), data.size());
      if (cbor_encoder_close_container(&package, &packageArray) != CborNoError ||
          cbor_encoder_get_extra_bytes_needed(&package) != 0) {
        LOG(E, "Attestation package CBOR encode failed");
        goto err;
      }
      size_t package_size = cbor_encoder_get_buffer_size(&package, packageBuf.data());
      LOG(D, "CBOR SIZE: %d", package_size);
      LOG(D, "SIGNED PACKAGE: %s", red_log::bufToHexString(packageBuf.data(), package_size).c_str());

      if (signature.size() < 64 || foundIssuer->issuer_pk.size() < 32) {
        LOG(E, "Attestation signature or issuer key has the wrong length");
        goto err;
      }
      int res = crypto_sign_ed25519_verify_detached(signature.data(), packageBuf.data(), package_size, foundIssuer->issuer_pk.data());
      if (res) {
        LOG(E, "Failed to verify attestation signature: %d", res);
        goto err;
      }
      return std::make_tuple(foundIssuer, devicePubKey);
    }
  err:
    return std::make_tuple(foundIssuer, std::vector<uint8_t>());
}

std::tuple<hkIssuer_t *, std::vector<uint8_t>, KeyFlow> HKAttestationAuth::attest()
{
  attestation_exchange_common_secret.resize(32);
  attestation_exchange_common_secret.reserve(32);
  esp_fill_random(attestation_exchange_common_secret.data(), 32);
  auto attTlv = simple_tlv(0xC0, attestation_exchange_common_secret.data(), 32, NULL, NULL);
  auto opAttTlv = simple_tlv(0x8E, attTlv.data(), attTlv.size(), NULL, NULL);
  std::vector<uint8_t> attComm{0x0};
  attComm.reserve(opAttTlv.size() + 1);
  attComm.insert(attComm.begin() + 1, opAttTlv.begin(), opAttTlv.end());
  LOG(D, "attComm: %s", red_log::bufToHexString(attComm.data(), attComm.size()).c_str());
  auto encryptedCmd = DKSContext.encrypt_command(attComm.data(), attComm.size());

  LOG(V, "encrypted_command: %s", red_log::bufToHexString(std::get<0>(encryptedCmd).data(), std::get<0>(encryptedCmd).size()).c_str());
  LOG(V, "calculated_rmac: %s", red_log::bufToHexString(std::get<1>(encryptedCmd).data(), std::get<1>(encryptedCmd).size()).c_str());
  std::vector<uint8_t> xchApdu = {0x84, 0xc9, 0x0, 0x0, (uint8_t)std::get<0>(encryptedCmd).size()};
  xchApdu.reserve(std::get<0>(encryptedCmd).size() + 5);
  xchApdu.insert(xchApdu.end(), std::get<0>(encryptedCmd).begin(), std::get<0>(encryptedCmd).end());
  LOG(V, "APDU CMD LENGTH: %d, DATA: %s", xchApdu.size(), red_log::bufToHexString(xchApdu.data(), xchApdu.size()).c_str());
  std::vector<uint8_t> xchRes;
  nfc(xchApdu, xchRes, false);
  LOG(D, "APDU RES LENGTH: %d, DATA: %s", xchRes.size(), red_log::bufToHexString(xchRes.data(), xchRes.size()).c_str());
  if (xchRes.size() > 2 && xchRes[xchRes.size() - 2] == 0x90)
  {
    auto env1Data = envelope1Cmd();
    std::vector<uint8_t> env1Res = std::get<0>(env1Data);
    if (env1Res.size() > 2 && env1Res.data()[env1Res.size() - 2] == 0x90)
    {
      auto salt = attestation_salt(std::get<0>(env1Data), std::get<1>(env1Data));
      if(salt.size() > 0){
        auto env2DataDec = envelope2Cmd(salt);
        if (env2DataDec.size() > 0)
        {
          auto verify_result = verify(env2DataDec);
          if (std::get<1>(verify_result).size() > 0) {
            return std::make_tuple(std::get<0>(verify_result), std::get<1>(verify_result), kFlowATTESTATION);
          }
        }
      }
    }
  }
  return std::make_tuple(nullptr, std::vector<uint8_t>(), kFlowFailed);
}