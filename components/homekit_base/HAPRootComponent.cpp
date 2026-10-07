#include "HAPRootComponent.h"
#include <esp_random.h>
#include "sodium/randombytes.h"

namespace esphome
{
    namespace homekit
    {
        /* Mandatory identify routine for the accessory.
        * In a real accessory, something like LED blink should be implemented
        * got visual identification
        */
        static int acc_identify(hap_acc_t *ha)
        {
            ESP_LOGI("HAP", "Accessory identified");
            return HAP_SUCCESS;
        }

        void HAPRootComponent::factory_reset() {
            // Resets HAP pairings only. HomeKey credentials live in their own
            // NVS namespace and are kept (see the homekit button platform for
            // a full HomeKey factory reset).
            hap_reset_pairings();
        }

        static const char *const SETUP_CODE_NVS_NAMESPACE = "hap_esph";
        static const char *const SETUP_CODE_NVS_KEY = "setup_code";

        bool HAPRootComponent::is_valid_setup_code(const std::string &code) {
            if (code.size() != 10 || code[3] != '-' || code[6] != '-')
                return false;
            std::string digits;
            for (size_t i = 0; i < code.size(); i++) {
                if (i == 3 || i == 6)
                    continue;
                if (code[i] < '0' || code[i] > '9')
                    return false;
                digits.push_back(code[i]);
            }
            // Codes the HomeKit specification forbids.
            if (digits == "12345678" || digits == "87654321")
                return false;
            if (std::all_of(digits.begin(), digits.end(), [&](char c) { return c == digits[0]; }))
                return false;
            return true;
        }

        bool HAPRootComponent::load_setup_code_override_(std::string &code) {
            nvs_handle_t h;
            if (nvs_open(SETUP_CODE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
                return false;
            char buf[16] = {0};
            size_t len = sizeof(buf);
            esp_err_t err = nvs_get_str(h, SETUP_CODE_NVS_KEY, buf, &len);
            nvs_close(h);
            if (err != ESP_OK)
                return false;
            code = buf;
            return is_valid_setup_code(code);
        }

        bool HAPRootComponent::set_setup_code_override(const std::string &code) {
            if (!is_valid_setup_code(code)) {
                ESP_LOGE(TAG, "Invalid HomeKit setup code, expected XXX-XX-XXX (not trivial like 111-11-111)");
                return false;
            }
            nvs_handle_t h;
            esp_err_t err = nvs_open(SETUP_CODE_NVS_NAMESPACE, NVS_READWRITE, &h);
            if (err == ESP_OK) {
                err = nvs_set_str(h, SETUP_CODE_NVS_KEY, code.c_str());
                if (err == ESP_OK)
                    err = nvs_commit(h);
                nvs_close(h);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Saving HomeKit setup code failed: %s", esp_err_to_name(err));
                return false;
            }
            ESP_LOGI(TAG, "HomeKit setup code override saved; it is used after the next reboot");
            return true;
        }

        bool HAPRootComponent::clear_setup_code_override() {
            nvs_handle_t h;
            esp_err_t err = nvs_open(SETUP_CODE_NVS_NAMESPACE, NVS_READWRITE, &h);
            if (err != ESP_OK)
                return false;
            err = nvs_erase_key(h, SETUP_CODE_NVS_KEY);
            if (err == ESP_OK)
                nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "HomeKit setup code override cleared; the YAML setup_code is used after the next reboot");
            return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
        }

        static const char *randombytes_esp32xx_implementation_name(void)
        {
            return CONFIG_IDF_TARGET;
        }

        HAPRootComponent::HAPRootComponent(const char* setup_code, const char* setup_id, std::map<AInfo, const char*> info)
        {
          const struct randombytes_implementation randombytes_esp32_implementation = {
              .implementation_name = randombytes_esp32xx_implementation_name,
              .random = esp_random,
              .stir = NULL,
              .uniform = NULL,
              .buf = esp_fill_random,
              .close = NULL,
          };
          randombytes_set_implementation(&randombytes_esp32_implementation);
            ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
            ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
            ESP_LOGI(TAG, "%s", esp_err_to_name(nvs_flash_init()));
            std::map<AInfo, const char*> merged_info;
            merged_info.merge(info);
            merged_info.merge(this->accessory_info);
            this->accessory_info.swap(merged_info);
            hap_acc_t* accessory;
            /* Initialize the HAP core */
            hap_init(HAP_TRANSPORT_WIFI);

            /* Initialise the mandatory parameters for Accessory which will be added as
            * the mandatory services internally
            */
            hap_cfg_t hap_cfg;
            hap_get_config(&hap_cfg);
            hap_cfg.task_stack_size = 8192;
            hap_cfg.task_priority = 2;
            hap_set_config(&hap_cfg);
            hap_acc_cfg_t cfg = {
                .name = strdup(accessory_info[NAME]),
                .model = strdup(accessory_info[MODEL]),
                .manufacturer = strdup(accessory_info[MANUFACTURER]),
                .serial_num = strdup(accessory_info[SN]),
                .fw_rev = strdup(accessory_info[FW_REV]),
                .hw_rev = "1.0",
                .pv = "1.1.0",
                .cid = HAP_CID_BRIDGE,
                .identify_routine = acc_identify,
            };

            /* Create accessory object */
            accessory = hap_acc_create(&cfg);
            if (!accessory) {
                ESP_LOGE(TAG, "Failed to create accessory");
                hap_acc_delete(accessory);
                vTaskDelete(NULL);
            }

            /* Add a dummy Product Data */
            uint8_t product_data[] = {'E','S','P','3','2','H','A','P'};
            hap_acc_add_product_data(accessory, product_data, sizeof(product_data));

            /* Add Wi-Fi Transport service required for HAP Spec R16 */
            hap_acc_add_wifi_transport_service(accessory, 0);

            /* Add the Accessory to the HomeKit Database */
            hap_add_accessory(accessory);
            /* Unique Setup code of the format xxx-xx-xxx. Default: 111-22-333 */
            std::string override_code;
            if (load_setup_code_override_(override_code)) {
                this->setup_code_overridden_ = true;
                hap_set_setup_code(override_code.c_str());
            } else {
                hap_set_setup_code(setup_code);
            }
            /* Unique four character Setup Id. Default: ES32 */
            hap_set_setup_id(setup_id);
        }

        void HAPRootComponent::setup() {
            // hap_http_debug_enable();
            // hap_set_debug_level(HAP_DEBUG_LEVEL_INFO);
            // esp_log_level_set("HAP", ESP_LOG_INFO);
            hap_start();
            ESP_LOGI(TAG, "HAP Bridge started!");
        }

        void HAPRootComponent::loop() {
        }

        void HAPRootComponent::dump_config() {
            ESP_LOGCONFIG(TAG, "HomeKit bridge:");
            ESP_LOGCONFIG(TAG, "  Setup code source: %s", this->setup_code_overridden_ ? "runtime override (NVS)" : "YAML");
        }

    }  // namespace homekit
}  // namespace esphome
