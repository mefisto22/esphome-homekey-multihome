#pragma once
#include <esphome/core/defines.h>
#ifdef USE_HOMEKEY
// Persistent multi-Home HomeKey reader store.
//
// Every Apple Home that provisions HomeKey on this accessory writes its *own*
// reader private key (and with it its own reader group identifier) through the
// NFC Access Control Point. The upstream implementation kept exactly one
// readerData_t, so provisioning a second, independent Home overwrote the first
// Home's reader key and the first Home's HomeKeys stopped working.
//
// This store keeps one readerData_t ("reader profile") per Home:
//   - the *active* profile belongs to the Home that is currently HAP-paired;
//     all HAP provisioning (issuers, reader key, device credentials) goes there
//   - *archived* profiles belong to Homes that provisioned the reader earlier
//     and were detached with "reset pairing, keep HomeKeys"; they are never
//     modified by HAP traffic but remain valid for NFC authentication
// NFC authentication tries every provisioned profile (ECP hint first).
//
// All state lives in one msgpack blob (NVS namespace HK_DATA, key HKSTORE) so
// every update is a single atomic NVS write. The legacy single-profile blob
// (HK_DATA/READERDATA) is migrated on first boot and left in place as a backup.
#include <HomeKey.h>
#include <nvs.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace homekit {

using HKTransceive = std::function<bool(std::vector<uint8_t> &, std::vector<uint8_t> &, bool)>;

namespace hk_store {

static constexpr const char *NVS_NAMESPACE = "HK_DATA";
static constexpr const char *NVS_KEY_STORE = "HKSTORE";
static constexpr const char *NVS_KEY_LEGACY = "READERDATA";
static constexpr const char *NVS_KEY_QUARANTINE = "HKSTORE_BAD";
// Scratch namespace handed to the HomeKey library during provisioning; the
// library insists on writing its own copy, which we erase immediately after.
static constexpr const char *NVS_SCRATCH_NAMESPACE = "HK_TMP";

static constexpr int FORMAT_VERSION = 2;
// Upper bound for reader profiles (= independent Homes) kept at once.
static constexpr size_t MAX_PROFILES = 8;
// Upper bound for the serialized store; protects NVS from runaway growth.
static constexpr size_t MAX_BLOB_SIZE = 24 * 1024;

enum class DecodeStatus { OK, MALFORMED, UNSUPPORTED_VERSION };

// --- Pure helpers (no NVS / HAP / crypto), unit-tested on the host ---
bool has_reader_key(const readerData_t &profile);
std::vector<uint8_t> encode(const std::vector<readerData_t> &profiles, int active);
DecodeStatus decode(const std::vector<uint8_t> &blob, std::vector<readerData_t> &profiles, int &active);
// Legacy HK_DATA/READERDATA (msgpack of the library's readerData_t to_json).
bool decode_legacy(const std::vector<uint8_t> &blob, readerData_t &out);
// Merge duplicate issuers (by issuer id) and endpoints (by endpoint id).
void normalize(readerData_t &profile);
// Append src's issuers/endpoints into dst (de-duplicated). Reader key of dst wins.
void merge_into(readerData_t &dst, const readerData_t &src);
// ECP frame (18 bytes) advertising the given reader group identifier.
std::vector<uint8_t> ecp_frame(const std::vector<uint8_t> &reader_gid);
// Structural validation of an NFC Access Control Point write before it reaches
// the HomeKey library (which dereferences missing TLV items unchecked).
bool validate_access_control_tlv(const std::vector<uint8_t> &tlv, uint8_t *operation);
// TLV8 lookup with fragment reassembly. Returns false on malformed input.
bool tlv8_find(const uint8_t *data, size_t len, uint8_t tag, std::vector<uint8_t> &out, bool &found);
std::string to_hex(const std::vector<uint8_t> &v);

}  // namespace hk_store

struct HomeKeyAuthResult {
  bool success{false};
  std::vector<uint8_t> issuer_id;
  std::vector<uint8_t> endpoint_id;
  int flow{-1};             // KeyFlow
  int attempts{0};          // number of reader profiles tried
  std::vector<uint8_t> reader_gid;  // profile that authenticated
};

struct HomeKeyStats {
  size_t profiles{0};       // reader profiles holding a reader key (= Homes)
  size_t issuers{0};
  size_t endpoints{0};
  bool active_provisioned{false};
  bool has_active{false};
};

class HomeKeyStore {
 public:
  // Hooks into the HomeKey library (set by the ESP glue; faked in host tests).
  using AuthFn = std::function<std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, KeyFlow>(
      readerData_t &, const HKTransceive &, nvs_handle_t readonly_handle)>;
  using ProvisionFn =
      std::function<std::vector<uint8_t>(readerData_t &, std::vector<uint8_t> &tlv, nvs_handle_t scratch_handle)>;

  static HomeKeyStore &get();

  explicit HomeKeyStore(const char *ns = hk_store::NVS_NAMESPACE,
                        const char *scratch_ns = hk_store::NVS_SCRATCH_NAMESPACE);

  void set_authenticator(AuthFn fn) { this->auth_fn_ = std::move(fn); }
  void set_provisioner(ProvisionFn fn) { this->provision_fn_ = std::move(fn); }

  // Load (or migrate) persisted data. Idempotent; must run after hap_init().
  // If no HAP controller is paired, the active profile is archived (kept).
  void begin(int paired_controller_count);
  bool is_ready() const { return this->begun_; }

  // ---- HAP side (HAP task) ----
  // A controller (Home user) was paired: register it as issuer of the active Home.
  bool add_issuer(const std::vector<uint8_t> &issuer_id, const std::vector<uint8_t> &issuer_pk);
  // NFC Access Control Point write. ok=false => nothing was changed/persisted.
  std::vector<uint8_t> process_access_control(const std::vector<uint8_t> &tlv, bool &ok);
  // The last HAP controller was removed (accessory removed from the active
  // Home): revoke that Home's reader profile only; archived Homes are kept.
  void revoke_active_home();

  // ---- ESPHome side (main loop) ----
  HomeKeyAuthResult authenticate(const HKTransceive &transceive, const std::vector<uint8_t> &hint_gid,
                                 const std::vector<uint8_t> &select_apdu);
  // True if a new Home can be enrolled after a pairing reset.
  bool can_enroll_another_home();
  // Detach the active Home's profile (keep it for NFC). Returns false on error.
  bool archive_active_home();
  // Erase every HomeKey credential (all Homes) from RAM and NVS.
  bool erase_all();

  std::vector<std::vector<uint8_t>> ecp_frames(std::vector<std::vector<uint8_t>> *gids = nullptr);
  HomeKeyStats stats();
  uint32_t generation() const { return this->generation_.load(); }
  void dump_config(const char *tag);

  // Testing / diagnostics
  std::vector<readerData_t> profiles_copy();
  int active_index();
  bool persistence_disabled() const { return this->persistence_disabled_; }

 protected:
  void open_handles_();
  bool load_locked_(int paired_controller_count);
  bool save_locked_();
  bool read_blob_(nvs_handle_t h, const char *key, std::vector<uint8_t> &out, bool &found);
  int ensure_active_locked_();
  void archive_active_locked_();
  void merge_duplicate_gids_locked_();
  void bump_();

  const char *ns_;
  const char *scratch_ns_;
  std::mutex mutex_;
  bool begun_{false};
  bool persistence_disabled_{false};
  nvs_handle_t rw_handle_{0};
  nvs_handle_t ro_handle_{0};
  nvs_handle_t scratch_handle_{0};
  bool rw_ok_{false};
  bool ro_ok_{false};
  bool scratch_ok_{false};
  std::vector<readerData_t> profiles_;
  int active_{-1};
  std::vector<uint8_t> last_saved_;
  std::atomic<uint32_t> generation_{1};
  AuthFn auth_fn_;
  ProvisionFn provision_fn_;
};

}  // namespace homekit
}  // namespace esphome
#endif
