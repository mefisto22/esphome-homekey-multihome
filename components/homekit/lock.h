#pragma once
#include <esphome/core/defines.h>
#ifdef USE_LOCK
#include <esphome/core/application.h>
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include "hap_entity.h"
#ifdef USE_HOMEKEY
#include <nvs.h>
#include "const.h"
#include <HK_HomeKit.h>
#include <hkAuthContext.h>
#include <esphome/components/pn532/pn532.h>
#include <esphome/core/base_automation.h>
#include "automation.h"
#include "homekey_store.h"
#include "homekey_library.h"
#endif

namespace esphome
{
  namespace homekit
  {
    class LockEntity : public HAPEntity
    {
    private:
      static constexpr const char* TAG = "LockEntity";
      lock::Lock* ptrToLock;
      hap_tlv8_val_t management = {
        .buf = 0,
        .buflen = 0
      };
      #ifdef USE_HOMEKEY
      // HomeKey reader data lives in the shared HomeKeyStore (one per device):
      // HAP pairing and the reader keys Apple Homes write are bridge-wide, so
      // every HomeKey lock on this bridge serves the same set of Homes.
      uint8_t tlv8_data[128];
      pn532::PN532* nfc_ctx_{nullptr};
      // Reader group of each ECP frame currently loaded into the PN532.
      std::vector<std::vector<uint8_t>> ecp_gids_;
      uint32_t ecp_generation_{0};
      std::vector<uint8_t> nfcSupportedConfBuffer{ 0x01, 0x01, 0x10, 0x02, 0x01, 0x10 };
      std::map<HKFinish, std::vector<uint8_t>> hk_color_vals = { {TAN, {0x01, 0x04, 0xce, 0xd5, 0xda, 0x00}}, {GOLD, {0x01, 0x04, 0xaa, 0xd6, 0xec, 0x00}}, {SILVER, {0x01, 0x04, 0xe3, 0xe3, 0xe3, 0x00}}, {BLACK, {0x01, 0x04, 0x00, 0x00, 0x00, 0x00}} };
      std::unique_ptr<hap_tlv8_val_t> hkFinishTlvData;
      hap_tlv8_val_t nfcSupportedConf = {
        .buf = nfcSupportedConfBuffer.data(),
        .buflen = nfcSupportedConfBuffer.size()
      };
      std::vector<HKAuthTrigger *> triggers_onhk_;
      std::vector<HKFailTrigger *> triggers_onhk_fail_;
      static int nfcAccess_write(hap_write_data_t write_data[], int count, void* serv_priv, void* write_priv);
      static void hap_event_handler(hap_event_t event, void* data);
      static void install_homekey_hooks();
      void refresh_ecp_frames_();
      void on_nfc_target_();
      #endif
      static void on_lock_update(lock::Lock* obj);
      static int lock_write(hap_write_data_t write_data[], int count, void* serv_priv, void* write_priv);
      static int acc_identify(hap_acc_t* ha);


      public:
      LockEntity(lock::Lock* lockPtr);
      void setup();
      #ifdef USE_HOMEKEY
      void set_nfc_ctx(pn532::PN532* ctx);
      void set_hk_hw_finish(HKFinish color);
      void register_onhk_trigger(HKAuthTrigger* trig);
      void register_onhkfail_trigger(HKFailTrigger* trig);
      bool homekey_enabled() const { return nfc_ctx_ != nullptr; }
      // Called from the main loop: pushes reader changes (new Home, new reader
      // key, factory reset) to the PN532 ECP frames.
      void homekey_loop();
      #endif
    };
  }
}
#endif
