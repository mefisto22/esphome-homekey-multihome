#include <esphome/core/defines.h>
#ifdef USE_LOCK
#include "lock.h"
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

namespace esphome {
namespace homekit {

#ifndef LOG
#define LOG(x, format, ...) \
  ESP_LOG##x(TAG, "%s > " format, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#endif

namespace hk_compat {
inline std::string bufToHexString(const uint8_t *buf, size_t len,
                                  bool ignoreLevel = false) {
  return format_hex_pretty(buf, len);
}
inline std::vector<uint8_t> getHashIdentifier(const uint8_t *key, size_t len,
                                              bool sha256) {
  std::vector<uint8_t> hashable;
  if (sha256) {
    const std::string prefix = "key-identifier";
    hashable.insert(hashable.begin(), prefix.begin(), prefix.end());
  }
  hashable.insert(hashable.end(), key, key + len);
  uint8_t hash[32];
  if (sha256) {
    mbedtls_sha256(hashable.data(), hashable.size(), hash, 0);
  } else {
    mbedtls_sha1(hashable.data(), hashable.size(), hash);
  }
  return std::vector<uint8_t>{hash, hash + (sha256 ? 8 : 6)};
}
}  // namespace hk_compat
#ifdef USE_HOMEKEY
static const std::vector<uint8_t> HOMEKEY_SELECT_APDU{0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00,
                                                     0x00, 0x08, 0x58, 0x01, 0x01, 0x00};

void LockEntity::install_homekey_hooks() {
  static bool installed = false;
  if (installed)
    return;
  installed = true;
#ifdef HOMEKEY_INSECURE_DEBUG_LOGGING
  ESP_LOGW(TAG, "HOMEKEY_INSECURE_DEBUG_LOGGING is enabled: HomeKey secrets may appear in logs!");
#endif
  homekey_install_library_hooks(HomeKeyStore::get());
}

int LockEntity::nfcAccess_write(hap_write_data_t write_data[], int count,
                                void *serv_priv, void *write_priv) {
  LockEntity *parent = (LockEntity *)serv_priv;
  auto &store = HomeKeyStore::get();
  store.begin(hap_get_paired_controller_count());
  int i, ret = HAP_SUCCESS;
  hap_write_data_t *write;
  for (i = 0; i < count; i++) {
    write = &write_data[i];
    /* Setting a default error value */
    *(write->status) = HAP_STATUS_VAL_INVALID;
    if (!strcmp(hap_char_get_type_uuid(write->hc),
                HAP_CHAR_UUID_NFC_ACCESS_CONTROL_POINT)) {
      hap_tlv8_val_t buf = write->val.t;
      auto tlv_rx_data = std::vector<uint8_t>(buf.buf, buf.buf + buf.buflen);
      // Never log the request itself: a "write reader key" request carries the
      // reader private key of the Home.
      ESP_LOGD(TAG, "NFC Access Control Point write, %u bytes", (unsigned) tlv_rx_data.size());
      bool ok = false;
      auto result = store.process_access_control(tlv_rx_data, ok);
      if (!ok) {
        continue;
      }
      if (result.size() > sizeof(parent->tlv8_data)) {
        ESP_LOGE(TAG, "NFC Access Control Point response too large (%u bytes)", (unsigned) result.size());
        *(write->status) = HAP_STATUS_OO_RES;
        continue;
      }
      memcpy(parent->tlv8_data, result.data(), result.size());
      hap_val_t new_val;
      new_val.t.buf = parent->tlv8_data;
      new_val.t.buflen = result.size();
      hap_char_update_val(write->hc, &new_val);
      *(write->status) = HAP_STATUS_SUCCESS;
    } else {
      *(write->status) = HAP_STATUS_RES_ABSENT;
    }
  }
  return ret;
}

void LockEntity::hap_event_handler(hap_event_t event, void *data) {
  auto &store = HomeKeyStore::get();
  if (event == HAP_EVENT_CTRL_PAIRED) {
    store.begin(hap_get_paired_controller_count());
    if (data == nullptr)
      return;
    hap_ctrl_data_t *ctrl = hap_get_controller_data((char *)data);
    if (ctrl == nullptr || !ctrl->valid)
      return;
    // Every Home user (HAP controller) is a HomeKey issuer; its identifier is
    // derived from the controller's long-term public key.
    std::vector<uint8_t> id = hk_compat::getHashIdentifier(ctrl->info.ltpk, 32, true);
    std::vector<uint8_t> pk(ctrl->info.ltpk, ctrl->info.ltpk + 32);
    store.add_issuer(id, pk);
  } else if (event == HAP_EVENT_CTRL_UNPAIRED) {
    store.begin(hap_get_paired_controller_count());
    if (hap_get_paired_controller_count() == 0) {
      // The accessory was removed from its Home. Only that Home's reader
      // profile is revoked; Homes detached earlier with "reset pairing, keep
      // HomeKeys" keep working. (A pairing reset from ESPHome does not emit
      // this event.)
      store.revoke_active_home();
    }
  }
}
#endif

void LockEntity::on_lock_update(lock::Lock *obj) {
  ESP_LOGD("on_lock_update", "%s state: %s", obj->get_name().c_str(),
           LOG_STR_ARG(lock_state_to_string(obj->state)));
  hap_acc_t *acc = hap_acc_get_by_aid(
      hap_get_unique_aid(std::to_string(obj->get_object_id_hash()).c_str()));
  hap_serv_t *hs = hap_acc_get_serv_by_uuid(acc, HAP_SERV_UUID_LOCK_MECHANISM);
  hap_char_t *current_state =
      hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_LOCK_CURRENT_STATE);
  hap_char_t *target_state =
      hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_LOCK_TARGET_STATE);
  hap_val_t c;
  hap_val_t t;
  if (obj->state == lock::LockState::LOCK_STATE_LOCKED ||
      obj->state == lock::LockState::LOCK_STATE_UNLOCKED) {
    c.i = obj->state % 2;
    t.i = obj->state % 2;
    hap_char_update_val(current_state, &c);
    hap_char_update_val(target_state, &t);
  } else if (obj->state == lock::LockState::LOCK_STATE_LOCKING ||
             obj->state == lock::LockState::LOCK_STATE_UNLOCKING) {
    t.i = (obj->state % 5) % 3;
    hap_char_update_val(target_state, &t);
  } else if (obj->state == lock::LockState::LOCK_STATE_JAMMED) {
    c.i = obj->state;
    hap_char_update_val(current_state, &c);
  }
  return;
}

int LockEntity::lock_write(hap_write_data_t write_data[], int count,
                           void *serv_priv, void *write_priv) {
  lock::Lock *lockPtr = (lock::Lock *)serv_priv;
  ESP_LOGD("lock_write", "Write called for Accessory '%s'(%s)",
           lockPtr->get_name().c_str(),
           std::to_string(lockPtr->get_object_id_hash()).c_str());
  int i, ret = HAP_SUCCESS;
  hap_write_data_t *write;
  for (i = 0; i < count; i++) {
    write = &write_data[i];
    if (!strcmp(hap_char_get_type_uuid(write->hc),
                HAP_CHAR_UUID_LOCK_TARGET_STATE)) {
      ESP_LOGD("lock_write", "Target State req: %d", write->val.i);
      hap_char_update_val(write->hc, &(write->val));
      hap_char_t *c = hap_serv_get_char_by_uuid(
          hap_char_get_parent(write->hc), HAP_CHAR_UUID_LOCK_CURRENT_STATE);
      ESP_LOGD("lock_write", "Current State: %d", hap_char_get_val(c)->i);
      hap_char_update_val(c, &(write->val));
      write->val.i ? lockPtr->lock() : lockPtr->unlock();
      *(write->status) = HAP_STATUS_SUCCESS;
    } else {
      *(write->status) = HAP_STATUS_RES_ABSENT;
    }
  }
  return ret;
}

int LockEntity::acc_identify(hap_acc_t *ha) {
  ESP_LOGI(TAG, "Accessory identified");
  return HAP_SUCCESS;
}

LockEntity::LockEntity(lock::Lock *lockPtr)
    : HAPEntity({{MODEL, "HAP-LOCK"}}), ptrToLock(lockPtr) {}
std::string intToFinishString(HKFinish d) {
  switch (d) {
  case TAN:
    return "TAN";
    break;
  case GOLD:
    return "GOLD";
    break;
  case SILVER:
    return "SILVER";
    break;
  case BLACK:
    return "BLACK";
    break;
  default:
    return "UNKNOWN";
    break;
  }
}

#ifdef USE_HOMEKEY
std::string hex_representation(const std::vector<uint8_t> &v) {
  std::string hex_tmp;
  for (auto x : v) {
    std::ostringstream oss;
    oss << std::hex << std::setw(2) << std::setfill('0') << (unsigned)x;
    hex_tmp += oss.str();
  }
  return hex_tmp;
}
void LockEntity::register_onhk_trigger(HKAuthTrigger *trig) {
  triggers_onhk_.push_back(trig);
}
void LockEntity::register_onhkfail_trigger(HKFailTrigger *trig) {
  triggers_onhk_fail_.push_back(trig);
}
void LockEntity::set_hk_hw_finish(HKFinish color) {
  ESP_LOGI(TAG, "SELECTED HK FINISH: %s", intToFinishString(color).c_str());
  hap_tlv8_val_t tlvData = {.buf = hk_color_vals[color].data(),
                            .buflen = hk_color_vals[color].size()};
  hkFinishTlvData = std::make_unique<hap_tlv8_val_t>(tlvData);
}
void LockEntity::refresh_ecp_frames_() {
  auto &store = HomeKeyStore::get();
  this->ecp_generation_ = store.generation();
  auto frames = store.ecp_frames(&this->ecp_gids_);
  this->nfc_ctx_->set_ecp_frames(frames);
}

void LockEntity::homekey_loop() {
  if (this->nfc_ctx_ == nullptr)
    return;
  auto &store = HomeKeyStore::get();
  if (store.generation() == this->ecp_generation_)
    return;
  this->refresh_ecp_frames_();
  auto st = store.stats();
  ESP_LOGI(TAG, "HomeKey reader data changed: %u Home(s), %u issuer(s), %u endpoint(s)", (unsigned) st.profiles,
           (unsigned) st.issuers, (unsigned) st.endpoints);
}

void LockEntity::set_nfc_ctx(pn532::PN532 *ctx) {
  install_homekey_hooks();
  this->nfc_ctx_ = ctx;
  auto trigger = new nfc::NfcOnTagTrigger();
  ctx->register_ontag_trigger(trigger);
  auto automation = new Automation<std::string, nfc::NfcTag>(trigger);
  auto action = new LambdaAction<std::string, nfc::NfcTag>(
      [this](std::string x, nfc::NfcTag tag) -> void { this->on_nfc_target_(); });
  automation->add_actions({action});
}

void LockEntity::on_nfc_target_() {
  pn532::PN532 *ctx = this->nfc_ctx_;
  HKTransceive transceive = [ctx](std::vector<uint8_t> &send, std::vector<uint8_t> &recv,
                                  bool ignoreLog) -> bool {
    auto data = ctx->inDataExchange(send);
    if (data.empty()) {
      return false;
    }
    data.erase(data.begin());
    ESP_LOGV(TAG, "%s", format_hex_pretty(data).c_str());
    recv = std::move(data);
    return true;
  };
  auto versions = ctx->inDataExchange(HOMEKEY_SELECT_APDU);
  if (versions.empty()) {
    ESP_LOGW(TAG, "Target probably not Homekey");
    return;
  }
  ESP_LOGD(TAG, "HK SUPPORTED VERSIONS: %s", format_hex_pretty(versions).c_str());
  if (versions.size() < 2 || versions[versions.size() - 2] != 0x90 || versions[versions.size() - 1] != 0x00) {
    for (auto &&t : triggers_onhk_fail_) {
      t->process();
    }
    ESP_LOGE(TAG, "Invalid response for HK");
    return;
  }
  auto &store = HomeKeyStore::get();
  store.begin(hap_get_paired_controller_count());
  // The reader group advertised in the ECP frame of this polling cycle is the
  // most likely one to have woken the device; try it first.
  std::vector<uint8_t> hint;
  int idx = ctx->get_last_ecp_frame_index();
  if (idx >= 0 && static_cast<size_t>(idx) < this->ecp_gids_.size())
    hint = this->ecp_gids_[idx];
  auto res = store.authenticate(transceive, hint, HOMEKEY_SELECT_APDU);
  if (res.success) {
    ESP_LOGI(TAG, "HomeKey authenticated: issuer %s, endpoint %s, Home group %s (flow %d, %d attempt(s))",
             hex_representation(res.issuer_id).c_str(), hex_representation(res.endpoint_id).c_str(),
             hex_representation(res.reader_gid).c_str(), res.flow, res.attempts);
    for (auto &&t : triggers_onhk_) {
      t->process(hex_representation(res.issuer_id), hex_representation(res.endpoint_id));
    }
  } else {
    ESP_LOGW(TAG, "HomeKey authentication failed (%d reader profile(s) tried)", res.attempts);
    for (auto &&t : triggers_onhk_fail_) {
      t->process();
    }
  }
}
#endif

void LockEntity::setup() {
  hap_acc_cfg_t acc_cfg = {
      .model = strdup(accessory_info[MODEL]),
      .manufacturer = strdup(accessory_info[MANUFACTURER]),
      .fw_rev = strdup(accessory_info[FW_REV]),
      .hw_rev = NULL,
      .pv = strdup("1.1.0"),
      .cid = HAP_CID_BRIDGE,
      .identify_routine = acc_identify,
  };
  hap_acc_t *accessory = nullptr;
  hap_serv_t *lockMechanism = nullptr;
  std::string accessory_name = ptrToLock->get_name();
  if (accessory_info[NAME] == NULL) {
    acc_cfg.name = strdup(accessory_name.c_str());
  } else {
    acc_cfg.name = strdup(accessory_info[NAME]);
  }
  if (accessory_info[SN] == NULL) {
    acc_cfg.serial_num =
        strdup(std::to_string(ptrToLock->get_object_id_hash()).c_str());
  } else {
    acc_cfg.serial_num = strdup(accessory_info[SN]);
  }
#ifdef USE_HOMEKEY
  if (this->nfc_ctx_ != nullptr)
    acc_cfg.hw_finish = hkFinishTlvData.get();
#endif
  accessory = hap_acc_create(&acc_cfg);
  lockMechanism =
      hap_serv_lock_mechanism_create(ptrToLock->state, ptrToLock->state);

  ESP_LOGD(TAG, "ID HASH: %lu", ptrToLock->get_object_id_hash());
  hap_serv_set_priv(lockMechanism, ptrToLock);

  /* Set the write callback for the service */
  hap_serv_set_write_cb(lockMechanism, lock_write);

  /* Add the Lock Service to the Accessory Object */
  hap_acc_add_serv(accessory, lockMechanism);
  hap_acc_add_serv(accessory, hap_serv_lock_management_create(&management, strdup("1.0.0")));

#ifdef USE_HOMEKEY
  if (this->nfc_ctx_ != nullptr) {
    // Loads (or migrates) the persisted HomeKey data. HAP was initialised by
    // homekit_base before any lock existed, so the controller count is valid.
    HomeKeyStore::get().begin(hap_get_paired_controller_count());
    this->refresh_ecp_frames_();
    hap_register_event_handler(hap_event_handler);
    hap_serv_t *nfcAccess = nullptr;
    nfcAccess = hap_serv_nfc_access_create(0, &management, &nfcSupportedConf);
    hap_serv_set_priv(nfcAccess, this);
    hap_serv_set_write_cb(nfcAccess, nfcAccess_write);
    hap_acc_add_serv(accessory, nfcAccess);
  }
#endif

  /* Add the Accessory to the HomeKit Database */
  hap_add_bridged_accessory(
      accessory, hap_get_unique_aid(
                     std::to_string(ptrToLock->get_object_id_hash()).c_str()));
  if (!ptrToLock->is_internal())
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 4, 0)
    ptrToLock->add_on_state_callback([this](lock::LockState /* state */) {
      LockEntity::on_lock_update(ptrToLock);
    });
#else
    ptrToLock->add_on_state_callback(
        [this]() { LockEntity::on_lock_update(ptrToLock); });
#endif

  ESP_LOGI(TAG, "Lock '%s' linked to HomeKit", accessory_name.c_str());
}
} // namespace homekit
} // namespace esphome
#endif
