#include "homekey_store.h"
#ifdef USE_HOMEKEY
#include <esphome/core/log.h>
#include <algorithm>
#include <cstring>

namespace esphome {
namespace homekit {

static const char *const TAG = "homekey.store";

namespace hk_store {

using json = nlohmann::json;

std::string to_hex(const std::vector<uint8_t> &v) {
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  out.reserve(v.size() * 2);
  for (uint8_t b : v) {
    out.push_back(HEX[b >> 4]);
    out.push_back(HEX[b & 0x0F]);
  }
  return out;
}

bool has_reader_key(const readerData_t &p) {
  // reader_pk_x comes from mbedtls_mpi_size() in the library and is shorter
  // than 32 bytes when the X coordinate has leading zero bytes.
  if (p.reader_sk.size() != 32 || p.reader_pk.empty() || p.reader_pk_x.empty() || p.reader_pk_x.size() > 32 ||
      p.reader_gid.size() != 8 || p.reader_id.empty())
    return false;
  // A freshly constructed readerData_t carries an all-zero placeholder key.
  return std::any_of(p.reader_sk.begin(), p.reader_sk.end(), [](uint8_t b) { return b != 0; });
}

// ---------------------------------------------------------------------------
// Decoding. ESP-IDF builds use -fno-exceptions, where any nlohmann type error
// becomes abort() (and a boot loop when it happens while loading). Every value
// is therefore type-checked before it is converted.
// ---------------------------------------------------------------------------
static bool get_bytes(const json &obj, const char *key, std::vector<uint8_t> &out) {
  auto it = obj.find(key);
  if (it == obj.end())
    return true;  // keep caller's default
  const json &v = *it;
  if (v.is_null()) {
    out.clear();
    return true;
  }
  if (v.is_binary()) {
    const auto &b = v.get_binary();
    out.assign(b.begin(), b.end());
    return true;
  }
  if (v.is_array()) {
    std::vector<uint8_t> tmp;
    tmp.reserve(v.size());
    for (const auto &e : v) {
      if (!e.is_number_integer())
        return false;
      int64_t n = e.get<int64_t>();
      if (n < 0 || n > 255)
        return false;
      tmp.push_back(static_cast<uint8_t>(n));
    }
    out.swap(tmp);
    return true;
  }
  return false;
}

template<typename T> static bool get_int(const json &obj, const char *key, T &out) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null())
    return true;
  if (!it->is_number_integer())
    return false;
  out = static_cast<T>(it->get<int64_t>());
  return true;
}

static bool parse_endpoint(const json &j, hkEndpoint_t &ep) {
  if (!j.is_object())
    return false;
  return get_bytes(j, "endpointId", ep.endpoint_id) && get_bytes(j, "publicKey", ep.endpoint_pk) &&
         get_bytes(j, "endpoint_key_x", ep.endpoint_pk_x) && get_bytes(j, "persistent_key", ep.endpoint_prst_k) &&
         get_int(j, "last_used_at", ep.last_used_at) && get_int(j, "counter", ep.counter) &&
         get_int(j, "key_type", ep.key_type);
}

static bool parse_issuer(const json &j, hkIssuer_t &iss) {
  if (!j.is_object())
    return false;
  if (!get_bytes(j, "issuerId", iss.issuer_id) || !get_bytes(j, "publicKey", iss.issuer_pk) ||
      !get_bytes(j, "issuer_key_x", iss.issuer_pk_x))
    return false;
  auto it = j.find("endpoints");
  if (it == j.end() || it->is_null())
    return true;
  if (!it->is_array())
    return false;
  for (const auto &e : *it) {
    hkEndpoint_t ep;
    if (!parse_endpoint(e, ep))
      return false;
    iss.endpoints.push_back(std::move(ep));
  }
  return true;
}

static bool parse_reader(const json &j, readerData_t &r) {
  if (!j.is_object())
    return false;
  if (!get_bytes(j, "reader_private_key", r.reader_sk) || !get_bytes(j, "reader_public_key", r.reader_pk) ||
      !get_bytes(j, "reader_key_x", r.reader_pk_x) || !get_bytes(j, "group_identifier", r.reader_gid) ||
      !get_bytes(j, "unique_identifier", r.reader_id))
    return false;
  auto it = j.find("issuers");
  if (it == j.end() || it->is_null())
    return true;
  if (!it->is_array())
    return false;
  for (const auto &e : *it) {
    hkIssuer_t iss;
    if (!parse_issuer(e, iss))
      return false;
    r.issuers.push_back(std::move(iss));
  }
  return true;
}

static json parse_msgpack(const std::vector<uint8_t> &blob) {
  if (blob.empty())
    return json(json::value_t::discarded);
  // strict=true, allow_exceptions=false: malformed input yields "discarded".
  return json::from_msgpack(blob, true, false);
}

bool decode_legacy(const std::vector<uint8_t> &blob, readerData_t &out) {
  json j = parse_msgpack(blob);
  if (j.is_discarded())
    return false;
  readerData_t tmp;
  if (!parse_reader(j, tmp))
    return false;
  out = std::move(tmp);
  return true;
}

DecodeStatus decode(const std::vector<uint8_t> &blob, std::vector<readerData_t> &profiles, int &active) {
  json j = parse_msgpack(blob);
  if (j.is_discarded() || !j.is_object())
    return DecodeStatus::MALFORMED;
  auto v = j.find("v");
  if (v == j.end() || !v->is_number_integer())
    return DecodeStatus::MALFORMED;
  if (v->get<int64_t>() != FORMAT_VERSION)
    return v->get<int64_t>() > FORMAT_VERSION ? DecodeStatus::UNSUPPORTED_VERSION : DecodeStatus::MALFORMED;
  int64_t act = -1;
  auto a = j.find("active");
  if (a != j.end()) {
    if (!a->is_number_integer())
      return DecodeStatus::MALFORMED;
    act = a->get<int64_t>();
  }
  auto rs = j.find("readers");
  if (rs == j.end() || !rs->is_array() || rs->size() > MAX_PROFILES)
    return DecodeStatus::MALFORMED;
  std::vector<readerData_t> tmp;
  for (const auto &e : *rs) {
    readerData_t r;
    if (!parse_reader(e, r))
      return DecodeStatus::MALFORMED;
    tmp.push_back(std::move(r));
  }
  if (act < -1 || act >= static_cast<int64_t>(tmp.size()))
    act = -1;
  profiles = std::move(tmp);
  active = static_cast<int>(act);
  return DecodeStatus::OK;
}

std::vector<uint8_t> encode(const std::vector<readerData_t> &profiles, int active) {
  json readers = json::array();
  for (const auto &p : profiles) {
    json issuers = json::array();
    for (const auto &iss : p.issuers) {
      json endpoints = json::array();
      for (const auto &ep : iss.endpoints) {
        endpoints.push_back({{"endpointId", json::binary(ep.endpoint_id)},
                             {"last_used_at", ep.last_used_at},
                             {"counter", ep.counter},
                             {"key_type", ep.key_type},
                             {"publicKey", json::binary(ep.endpoint_pk)},
                             {"endpoint_key_x", json::binary(ep.endpoint_pk_x)},
                             {"persistent_key", json::binary(ep.endpoint_prst_k)}});
      }
      issuers.push_back({{"issuerId", json::binary(iss.issuer_id)},
                         {"publicKey", json::binary(iss.issuer_pk)},
                         {"issuer_key_x", json::binary(iss.issuer_pk_x)},
                         {"endpoints", std::move(endpoints)}});
    }
    readers.push_back({{"reader_private_key", json::binary(p.reader_sk)},
                       {"reader_public_key", json::binary(p.reader_pk)},
                       {"reader_key_x", json::binary(p.reader_pk_x)},
                       {"group_identifier", json::binary(p.reader_gid)},
                       {"unique_identifier", json::binary(p.reader_id)},
                       {"issuers", std::move(issuers)}});
  }
  json root = {{"v", FORMAT_VERSION}, {"active", active}, {"readers", std::move(readers)}};
  return json::to_msgpack(root);
}

static void merge_endpoint(hkIssuer_t &iss, const hkEndpoint_t &ep) {
  for (auto &e : iss.endpoints) {
    if (e.endpoint_id != ep.endpoint_id)
      continue;
    // Later entries are newer (the library appends on ATTESTATION and updates
    // in place on STANDARD), so a later persistent key wins.
    if (!ep.endpoint_prst_k.empty())
      e.endpoint_prst_k = ep.endpoint_prst_k;
    if (e.endpoint_pk.empty())
      e.endpoint_pk = ep.endpoint_pk;
    if (e.endpoint_pk_x.empty())
      e.endpoint_pk_x = ep.endpoint_pk_x;
    if (e.key_type == 0)
      e.key_type = ep.key_type;
    e.last_used_at = std::max(e.last_used_at, ep.last_used_at);
    e.counter = std::max(e.counter, ep.counter);
    return;
  }
  iss.endpoints.push_back(ep);
}

static void merge_issuers(std::vector<hkIssuer_t> &dst, const std::vector<hkIssuer_t> &src) {
  for (const auto &iss : src) {
    hkIssuer_t *target = nullptr;
    for (auto &d : dst) {
      if (d.issuer_id == iss.issuer_id) {
        target = &d;
        break;
      }
    }
    if (target == nullptr) {
      hkIssuer_t n;
      n.issuer_id = iss.issuer_id;
      n.issuer_pk = iss.issuer_pk;
      n.issuer_pk_x = iss.issuer_pk_x;
      dst.push_back(std::move(n));
      target = &dst.back();
    } else {
      if (target->issuer_pk.empty())
        target->issuer_pk = iss.issuer_pk;
      if (target->issuer_pk_x.empty())
        target->issuer_pk_x = iss.issuer_pk_x;
    }
    for (const auto &ep : iss.endpoints)
      merge_endpoint(*target, ep);
  }
}

void normalize(readerData_t &profile) {
  std::vector<hkIssuer_t> out;
  merge_issuers(out, profile.issuers);
  profile.issuers.swap(out);
}

void merge_into(readerData_t &dst, const readerData_t &src) {
  merge_issuers(dst.issuers, src.issuers);
  normalize(dst);
}

static void crc16a(const uint8_t *data, size_t size, uint8_t *result) {
  uint16_t w_crc = 0x6363;
  for (size_t i = 0; i < size; ++i) {
    uint8_t byte = data[i];
    byte = (byte ^ (w_crc & 0x00FF));
    byte = ((byte ^ (byte << 4)) & 0xFF);
    w_crc = ((w_crc >> 8) ^ (byte << 8) ^ (byte << 3) ^ (byte >> 4)) & 0xFFFF;
  }
  result[0] = static_cast<uint8_t>(w_crc & 0xFF);
  result[1] = static_cast<uint8_t>((w_crc >> 8) & 0xFF);
}

std::vector<uint8_t> ecp_frame(const std::vector<uint8_t> &reader_gid) {
  std::vector<uint8_t> frame{0x6A, 0x02, 0xCB, 0x02, 0x06, 0x02, 0x11, 0x00};
  frame.resize(18, 0);
  std::copy_n(reader_gid.begin(), std::min<size_t>(reader_gid.size(), 8), frame.begin() + 8);
  crc16a(frame.data(), 16, frame.data() + 16);
  return frame;
}

bool tlv8_find(const uint8_t *data, size_t len, uint8_t tag, std::vector<uint8_t> &out, bool &found) {
  found = false;
  out.clear();
  bool appending = false;
  size_t i = 0;
  while (i < len) {
    if (i + 2 > len)
      return false;
    uint8_t t = data[i];
    uint8_t l = data[i + 1];
    if (i + 2 + l > len)
      return false;
    if (t == tag && (!found || appending)) {
      found = true;
      out.insert(out.end(), data + i + 2, data + i + 2 + l);
      appending = (l == 255);  // a 255-byte item continues in the next one
    } else {
      appending = false;
    }
    i += 2 + l;
  }
  return true;
}

static bool tlv8_get(const std::vector<uint8_t> &buf, uint8_t tag, std::vector<uint8_t> &out, bool &found) {
  return tlv8_find(buf.data(), buf.size(), tag, out, found);
}

bool validate_access_control_tlv(const std::vector<uint8_t> &tlv, uint8_t *operation) {
  std::vector<uint8_t> op, rkr, dcr;
  bool has_op, has_rkr, has_dcr;
  if (!tlv8_get(tlv, kReader_Operation, op, has_op) || !has_op || op.size() != 1)
    return false;
  if (!tlv8_get(tlv, kReader_Reader_Key_Request, rkr, has_rkr) ||
      !tlv8_get(tlv, kReader_Device_Credential_Request, dcr, has_dcr))
    return false;
  if (operation != nullptr)
    *operation = op[0];
  std::vector<uint8_t> tmp;
  bool f;
  // Nested TLVs must at least be well-formed.
  if (has_rkr && !tlv8_get(rkr, 0xFF, tmp, f))
    return false;
  if (has_dcr && !tlv8_get(dcr, 0xFF, tmp, f))
    return false;
  switch (op[0]) {
    case kReader_Operation_Read:
      return has_rkr;
    case kReader_Operation_Write:
      if (has_rkr)
        return true;  // set_reader_key() validates its own fields
      if (!has_dcr)
        return false;
      {
        // provision_device_cred() dereferences these without checking.
        std::vector<uint8_t> v;
        bool present;
        if (!tlv8_get(dcr, kDevice_Req_Issuer_Key_Identifier, v, present) || !present || v.size() != 8)
          return false;
        if (!tlv8_get(dcr, kDevice_Req_Public_Key, v, present) || !present || v.size() != 64)
          return false;
        if (!tlv8_get(dcr, kDevice_Req_Key_Type, v, present) || !present || v.empty())
          return false;
      }
      return true;
    case kReader_Operation_Remove:
      return has_rkr || has_dcr;
    default:
      return false;
  }
}

}  // namespace hk_store

using namespace hk_store;

HomeKeyStore &HomeKeyStore::get() {
  static HomeKeyStore instance;
  return instance;
}

HomeKeyStore::HomeKeyStore(const char *ns, const char *scratch_ns) : ns_(ns), scratch_ns_(scratch_ns) {}

void HomeKeyStore::bump_() { this->generation_.fetch_add(1); }

void HomeKeyStore::open_handles_() {
  esp_err_t err = nvs_open(this->ns_, NVS_READWRITE, &this->rw_handle_);
  this->rw_ok_ = err == ESP_OK;
  if (!this->rw_ok_)
    ESP_LOGE(TAG, "nvs_open(%s) failed: %s", this->ns_, esp_err_to_name(err));
  // Handed to the HomeKey library during NFC authentication: the library tries
  // to persist a single-profile blob itself, which must not clobber our store.
  err = nvs_open(this->ns_, NVS_READONLY, &this->ro_handle_);
  this->ro_ok_ = err == ESP_OK;
  err = nvs_open(this->scratch_ns_, NVS_READWRITE, &this->scratch_handle_);
  this->scratch_ok_ = err == ESP_OK;
  if (this->scratch_ok_) {
    // Scrub anything a previous (interrupted) provisioning left behind.
    nvs_erase_all(this->scratch_handle_);
    nvs_commit(this->scratch_handle_);
  }
}

bool HomeKeyStore::read_blob_(nvs_handle_t h, const char *key, std::vector<uint8_t> &out, bool &found) {
  found = false;
  out.clear();
  size_t len = 0;
  esp_err_t err = nvs_get_blob(h, key, nullptr, &len);
  if (err == ESP_ERR_NVS_NOT_FOUND)
    return true;
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Reading %s failed: %s", key, esp_err_to_name(err));
    return false;
  }
  out.resize(len);
  err = nvs_get_blob(h, key, out.data(), &len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Reading %s failed: %s", key, esp_err_to_name(err));
    out.clear();
    return false;
  }
  out.resize(len);
  found = true;
  return true;
}

void HomeKeyStore::begin(int paired_controller_count) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (this->begun_)
    return;
  this->begun_ = true;
  this->open_handles_();
  this->load_locked_(paired_controller_count);
  this->bump_();
}

bool HomeKeyStore::load_locked_(int paired_controller_count) {
  if (!this->rw_ok_) {
    ESP_LOGE(TAG, "NVS unavailable - HomeKey data cannot be loaded or persisted");
    this->persistence_disabled_ = true;
    return false;
  }
  bool loaded = false;
  std::vector<uint8_t> blob;
  bool found = false;
  if (this->read_blob_(this->rw_handle_, NVS_KEY_STORE, blob, found) && found) {
    std::vector<readerData_t> profiles;
    int active = -1;
    switch (decode(blob, profiles, active)) {
      case DecodeStatus::OK:
        this->profiles_ = std::move(profiles);
        this->active_ = active;
        this->last_saved_ = blob;
        loaded = true;
        break;
      case DecodeStatus::UNSUPPORTED_VERSION:
        // Written by a newer firmware: never overwrite it from here.
        ESP_LOGE(TAG, "HomeKey store was written by a newer firmware; HomeKey is disabled and the data is left "
                      "untouched. Update the firmware (or factory reset HomeKeys).");
        this->persistence_disabled_ = true;
        return false;
      case DecodeStatus::MALFORMED: {
        ESP_LOGE(TAG, "HomeKey store (%u bytes) is corrupted; keeping a copy in %s and falling back",
                 (unsigned) blob.size(), NVS_KEY_QUARANTINE);
        size_t qlen = 0;
        if (nvs_get_blob(this->rw_handle_, NVS_KEY_QUARANTINE, nullptr, &qlen) == ESP_ERR_NVS_NOT_FOUND) {
          nvs_set_blob(this->rw_handle_, NVS_KEY_QUARANTINE, blob.data(), blob.size());
          nvs_commit(this->rw_handle_);
        }
        break;
      }
    }
  }
  if (!loaded) {
    if (this->read_blob_(this->rw_handle_, NVS_KEY_LEGACY, blob, found) && found) {
      readerData_t legacy;
      if (decode_legacy(blob, legacy)) {
        ESP_LOGI(TAG, "Migrating legacy HomeKey data (READERDATA, %u bytes, %u issuers) to the multi-Home store",
                 (unsigned) blob.size(), (unsigned) legacy.issuers.size());
        // The legacy blob always belongs to the Home that provisioned last.
        this->profiles_.clear();
        this->profiles_.push_back(std::move(legacy));
        this->active_ = 0;
        loaded = true;
        // READERDATA is intentionally kept as a read-only backup for downgrades.
      } else {
        ESP_LOGE(TAG, "Legacy HomeKey data (READERDATA) is corrupted; leaving it untouched and starting empty");
      }
    }
  }
  for (auto &p : this->profiles_)
    normalize(p);
  if (this->active_ >= 0 && paired_controller_count <= 0) {
    // HAP pairings are gone (pairing reset, possibly from an older "reset
    // pairings" button): the Home that owned the active profile can no longer
    // reach us. Keep its HomeKeys working, but never let the next Home write
    // into (and overwrite) its reader key.
    ESP_LOGI(TAG, "No HomeKit controller paired: keeping the previous Home's HomeKey profile as archived");
    this->archive_active_locked_();
  }
  this->merge_duplicate_gids_locked_();
  if (!loaded && this->profiles_.empty()) {
    // Nothing usable found (fresh device, or data we could not read and left
    // untouched): do not write anything until there is something to store.
    this->last_saved_ = encode(this->profiles_, this->active_);
    return true;
  }
  return this->save_locked_();
}

bool HomeKeyStore::save_locked_() {
  if (this->persistence_disabled_ || !this->rw_ok_)
    return false;
  std::vector<uint8_t> blob = encode(this->profiles_, this->active_);
  if (blob == this->last_saved_)
    return true;
  if (blob.size() > MAX_BLOB_SIZE) {
    ESP_LOGE(TAG, "HomeKey store too large (%u bytes > %u), not saved", (unsigned) blob.size(),
             (unsigned) MAX_BLOB_SIZE);
    return false;
  }
  // A single nvs_set_blob is atomic: after a power loss NVS returns either the
  // previous or the new blob, never a mix.
  esp_err_t err = nvs_set_blob(this->rw_handle_, NVS_KEY_STORE, blob.data(), blob.size());
  if (err == ESP_OK)
    err = nvs_commit(this->rw_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Persisting HomeKey store failed: %s", esp_err_to_name(err));
    return false;
  }
  this->last_saved_ = std::move(blob);
  ESP_LOGD(TAG, "HomeKey store saved (%u bytes)", (unsigned) this->last_saved_.size());
  return true;
}

int HomeKeyStore::ensure_active_locked_() {
  if (this->active_ >= 0)
    return this->active_;
  if (this->profiles_.size() >= MAX_PROFILES) {
    ESP_LOGE(TAG, "Maximum number of HomeKey reader profiles (%u) reached; factory reset HomeKeys to enroll more",
             (unsigned) MAX_PROFILES);
    return -1;
  }
  this->profiles_.emplace_back();
  this->active_ = static_cast<int>(this->profiles_.size()) - 1;
  ESP_LOGI(TAG, "Started a new HomeKey reader profile for the newly paired Home");
  return this->active_;
}

void HomeKeyStore::archive_active_locked_() {
  if (this->active_ < 0)
    return;
  if (!has_reader_key(this->profiles_[this->active_])) {
    // Without a reader key the profile can never authenticate anything.
    this->profiles_.erase(this->profiles_.begin() + this->active_);
  }
  this->active_ = -1;
  this->merge_duplicate_gids_locked_();
}

void HomeKeyStore::merge_duplicate_gids_locked_() {
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < this->profiles_.size() && !changed; i++) {
      if (!has_reader_key(this->profiles_[i]))
        continue;
      for (size_t j = i + 1; j < this->profiles_.size(); j++) {
        if (!has_reader_key(this->profiles_[j]) || this->profiles_[i].reader_gid != this->profiles_[j].reader_gid)
          continue;
        // Same Home re-provisioned the reader (e.g. re-paired after a pairing
        // reset): fold the older profile into the one we keep.
        size_t keep = (static_cast<int>(j) == this->active_) ? j : i;
        size_t drop = keep == i ? j : i;
        ESP_LOGI(TAG, "Merging duplicate HomeKey reader profile for group %s",
                 to_hex(this->profiles_[keep].reader_gid).c_str());
        merge_into(this->profiles_[keep], this->profiles_[drop]);
        this->profiles_.erase(this->profiles_.begin() + drop);
        if (this->active_ == static_cast<int>(drop))
          this->active_ = -1;  // cannot happen (active is always kept), defensive
        else if (this->active_ > static_cast<int>(drop))
          this->active_--;
        changed = true;
        break;
      }
    }
  }
}

bool HomeKeyStore::add_issuer(const std::vector<uint8_t> &issuer_id, const std::vector<uint8_t> &issuer_pk) {
  if (issuer_id.size() != 8 || issuer_pk.size() != 32)
    return false;
  std::lock_guard<std::mutex> lock(this->mutex_);
  int idx = this->ensure_active_locked_();
  if (idx < 0)
    return false;
  readerData_t &p = this->profiles_[idx];
  for (auto &iss : p.issuers) {
    if (iss.issuer_id == issuer_id) {
      ESP_LOGD(TAG, "Issuer %s already registered", to_hex(issuer_id).c_str());
      if (iss.issuer_pk.empty())
        iss.issuer_pk = issuer_pk;
      this->save_locked_();
      return true;
    }
  }
  hkIssuer_t iss;
  iss.issuer_id = issuer_id;
  iss.issuer_pk = issuer_pk;
  p.issuers.push_back(std::move(iss));
  ESP_LOGI(TAG, "Registered HomeKey issuer %s for the paired Home", to_hex(issuer_id).c_str());
  // Kept in RAM even if persisting fails; the next successful save includes it.
  if (!this->save_locked_())
    ESP_LOGW(TAG, "Issuer kept in RAM only (will be persisted with the next change)");
  this->bump_();
  return true;
}

std::vector<uint8_t> HomeKeyStore::process_access_control(const std::vector<uint8_t> &tlv, bool &ok) {
  ok = false;
  uint8_t op = 0;
  if (!validate_access_control_tlv(tlv, &op)) {
    ESP_LOGW(TAG, "Rejected malformed NFC Access Control Point request (%u bytes)", (unsigned) tlv.size());
    return {};
  }
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (!this->provision_fn_)
    return {};
  if (this->persistence_disabled_) {
    ESP_LOGE(TAG, "HomeKey store is read-only (see earlier errors); refusing provisioning");
    return {};
  }
  const auto snapshot = this->profiles_;
  const int snapshot_active = this->active_;
  const auto saved_before = this->last_saved_;
  int idx = this->ensure_active_locked_();
  if (idx < 0)
    return {};
  ESP_LOGI(TAG, "NFC Access Control Point: operation %u (%s)", op,
           op == kReader_Operation_Read ? "read" : op == kReader_Operation_Write ? "write" : "remove");
  std::vector<uint8_t> request = tlv;
  std::vector<uint8_t> response =
      this->provision_fn_(this->profiles_[idx], request, this->scratch_ok_ ? this->scratch_handle_ : 0);
  if (this->scratch_ok_) {
    // The library stored a private copy (including the reader private key).
    nvs_erase_all(this->scratch_handle_);
    nvs_commit(this->scratch_handle_);
  }
  std::vector<uint8_t> rkr;
  bool is_reader_key_op = false;
  tlv8_find(tlv.data(), tlv.size(), kReader_Reader_Key_Request, rkr, is_reader_key_op);
  if (response.empty() && op == kReader_Operation_Write && is_reader_key_op) {
    // set_reader_key() failed after (possibly) modifying the profile: undo.
    // Note: an empty response is NOT an error for device credential writes -
    // the pinned library never fills that response (it adds the TLV to the
    // wrong object) although the credential is stored, and Apple Home accepts
    // it; that is the behaviour the existing installations rely on.
    this->profiles_ = snapshot;
    this->active_ = snapshot_active;
    ESP_LOGW(TAG, "HomeKey provisioning operation failed; no change applied");
    return {};
  }
  normalize(this->profiles_[idx]);
  this->merge_duplicate_gids_locked_();
  if (!this->save_locked_()) {
    // Never report success for something that would be lost on reboot, and
    // never leave a half-applied change in RAM.
    this->profiles_ = snapshot;
    this->active_ = snapshot_active;
    ESP_LOGE(TAG, "Provisioning change could not be persisted; rolled back");
    return {};
  }
  if (this->last_saved_ != saved_before) {
    const readerData_t &a = this->profiles_[this->active_];
    ESP_LOGI(TAG, "Active Home: reader group %s, %u issuer(s); %u reader profile(s) stored",
             has_reader_key(a) ? to_hex(a.reader_gid).c_str() : "<none>", (unsigned) a.issuers.size(),
             (unsigned) this->profiles_.size());
    this->bump_();
  }
  ok = true;
  return response;
}

void HomeKeyStore::revoke_active_home() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (this->active_ < 0)
    return;
  ESP_LOGW(TAG, "Accessory was removed from its Home: revoking that Home's HomeKeys (group %s); %u other Home "
                "profile(s) kept",
           to_hex(this->profiles_[this->active_].reader_gid).c_str(), (unsigned) (this->profiles_.size() - 1));
  this->profiles_.erase(this->profiles_.begin() + this->active_);
  this->active_ = -1;
  this->save_locked_();
  if (this->rw_ok_ && !this->persistence_disabled_) {
    // The legacy backup may hold the revoked Home's reader key.
    if (nvs_erase_key(this->rw_handle_, NVS_KEY_LEGACY) == ESP_OK)
      nvs_commit(this->rw_handle_);
  }
  this->bump_();
}

HomeKeyAuthResult HomeKeyStore::authenticate(const HKTransceive &transceive, const std::vector<uint8_t> &hint_gid,
                                             const std::vector<uint8_t> &select_apdu) {
  HomeKeyAuthResult res;
  std::lock_guard<std::mutex> lock(this->mutex_);
  std::vector<size_t> order;
  auto push = [&](size_t i) {
    if (i < this->profiles_.size() && has_reader_key(this->profiles_[i]) &&
        std::find(order.begin(), order.end(), i) == order.end())
      order.push_back(i);
  };
  // 1. profile whose ECP frame woke the device, 2. active Home, 3. the rest
  if (!hint_gid.empty()) {
    for (size_t i = 0; i < this->profiles_.size(); i++)
      if (this->profiles_[i].reader_gid == hint_gid)
        push(i);
  }
  if (this->active_ >= 0)
    push(this->active_);
  for (size_t i = 0; i < this->profiles_.size(); i++)
    push(i);
  if (order.empty() || !this->auth_fn_) {
    ESP_LOGW(TAG, "No HomeKey reader key provisioned yet");
    std::vector<uint8_t> fail{0x80, 0x3C, 0x00, 0x00}, r;
    transceive(fail, r, false);
    return res;
  }
  for (size_t k = 0; k < order.size(); k++) {
    const bool last = (k + 1 == order.size());
    readerData_t &profile = this->profiles_[order[k]];
    if (k > 0) {
      // Start a fresh transaction on the same field session.
      std::vector<uint8_t> sel = select_apdu, r;
      if (!transceive(sel, r, false) || r.size() < 2 || r[r.size() - 2] != 0x90 || r[r.size() - 1] != 0x00) {
        ESP_LOGD(TAG, "Re-select failed, device left the field");
        break;
      }
    }
    ESP_LOGD(TAG, "Trying reader profile %s (%u/%u)", to_hex(profile.reader_gid).c_str(), (unsigned) (k + 1),
             (unsigned) order.size());
    // While other profiles remain, swallow the "transaction failed" control
    // flow so the device stays in the transaction for the next attempt.
    HKTransceive wrapped = [&transceive, last](std::vector<uint8_t> &send, std::vector<uint8_t> &recv,
                                               bool ignore_log) -> bool {
      if (!last && send.size() >= 3 && send[0] == 0x80 && send[1] == 0x3C && send[2] == kCmdFlowFailed) {
        recv.clear();
        return true;
      }
      return transceive(send, recv, ignore_log);
    };
    res.attempts++;
    auto r = this->auth_fn_(profile, wrapped, this->ro_ok_ ? this->ro_handle_ : 0);
    if (!std::get<0>(r).empty() && std::get<2>(r) != kFlowFailed) {
      res.success = true;
      res.issuer_id = std::get<0>(r);
      res.endpoint_id = std::get<1>(r);
      res.flow = std::get<2>(r);
      res.reader_gid = profile.reader_gid;
      if (res.flow == kFlowSTANDARD || res.flow == kFlowATTESTATION) {
        // New persistent key (STANDARD) or new endpoint (ATTESTATION).
        normalize(profile);
        if (!this->save_locked_())
          ESP_LOGW(TAG, "Updated endpoint data could not be persisted");
        this->bump_();
      }
      return res;
    }
  }
  return res;
}

bool HomeKeyStore::can_enroll_another_home() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (this->persistence_disabled_)
    return false;
  size_t keyed = std::count_if(this->profiles_.begin(), this->profiles_.end(), has_reader_key);
  return keyed < MAX_PROFILES;
}

bool HomeKeyStore::archive_active_home() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (this->active_ < 0)
    return true;
  const auto snapshot = this->profiles_;
  const int snapshot_active = this->active_;
  this->archive_active_locked_();
  if (!this->save_locked_()) {
    this->profiles_ = snapshot;
    this->active_ = snapshot_active;
    return false;
  }
  this->bump_();
  return true;
}

bool HomeKeyStore::erase_all() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  this->profiles_.clear();
  this->active_ = -1;
  this->last_saved_.clear();
  bool ok = true;
  if (this->rw_ok_) {
    esp_err_t err = nvs_erase_all(this->rw_handle_);
    if (err == ESP_OK)
      err = nvs_commit(this->rw_handle_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Erasing HomeKey data failed: %s", esp_err_to_name(err));
      ok = false;
    }
  }
  if (this->scratch_ok_) {
    nvs_erase_all(this->scratch_handle_);
    nvs_commit(this->scratch_handle_);
  }
  // Nothing may be written back until the reboot that follows a factory reset.
  this->persistence_disabled_ = true;
  this->bump_();
  ESP_LOGW(TAG, "All HomeKey credentials erased");
  return ok;
}

std::vector<std::vector<uint8_t>> HomeKeyStore::ecp_frames(std::vector<std::vector<uint8_t>> *gids) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  std::vector<std::vector<uint8_t>> frames;
  std::vector<size_t> order;
  if (this->active_ >= 0 && has_reader_key(this->profiles_[this->active_]))
    order.push_back(this->active_);
  for (size_t i = 0; i < this->profiles_.size(); i++)
    if (static_cast<int>(i) != this->active_ && has_reader_key(this->profiles_[i]))
      order.push_back(i);
  if (gids != nullptr)
    gids->clear();
  for (size_t i : order) {
    frames.push_back(ecp_frame(this->profiles_[i].reader_gid));
    if (gids != nullptr)
      gids->push_back(this->profiles_[i].reader_gid);
  }
  if (frames.empty()) {
    // Unprovisioned: same all-zero group frame as before.
    frames.push_back(ecp_frame({}));
    if (gids != nullptr)
      gids->push_back({});
  }
  return frames;
}

HomeKeyStats HomeKeyStore::stats() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  HomeKeyStats s;
  for (const auto &p : this->profiles_) {
    if (has_reader_key(p))
      s.profiles++;
    s.issuers += p.issuers.size();
    for (const auto &iss : p.issuers)
      s.endpoints += iss.endpoints.size();
  }
  s.has_active = this->active_ >= 0;
  s.active_provisioned = s.has_active && has_reader_key(this->profiles_[this->active_]);
  return s;
}

void HomeKeyStore::dump_config(const char *tag) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  ESP_LOGCONFIG(tag, "  HomeKey reader profiles (Homes): %u/%u%s", (unsigned) this->profiles_.size(),
                (unsigned) MAX_PROFILES, this->persistence_disabled_ ? " [READ-ONLY]" : "");
  for (size_t i = 0; i < this->profiles_.size(); i++) {
    const auto &p = this->profiles_[i];
    size_t eps = 0;
    for (const auto &iss : p.issuers)
      eps += iss.endpoints.size();
    // Group / unique identifiers are public (broadcast over NFC); keys are never logged.
    ESP_LOGCONFIG(tag, "    #%u %s group=%s reader_id=%s issuers=%u endpoints=%u", (unsigned) i,
                  static_cast<int>(i) == this->active_ ? "[active]  " : "[archived]",
                  has_reader_key(p) ? to_hex(p.reader_gid).c_str() : "<no reader key>", to_hex(p.reader_id).c_str(),
                  (unsigned) p.issuers.size(), (unsigned) eps);
    for (const auto &iss : p.issuers)
      ESP_LOGCONFIG(tag, "       issuer %s endpoints=%u", to_hex(iss.issuer_id).c_str(),
                    (unsigned) iss.endpoints.size());
  }
}

std::vector<readerData_t> HomeKeyStore::profiles_copy() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return this->profiles_;
}

int HomeKeyStore::active_index() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return this->active_;
}

}  // namespace homekit
}  // namespace esphome
#endif
