import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import button
from esphome.const import DEVICE_CLASS_RESTART, ENTITY_CATEGORY_CONFIG

from .. import homekit_ns, homekey_configured

DEPENDENCIES = ["homekit"]

CONF_RESET_PAIRING_KEEP_HOMEKEYS = "reset_pairing_keep_homekeys"
CONF_FACTORY_RESET_HOMEKEYS = "factory_reset_homekeys"

HomeKeyButton = homekit_ns.class_("HomeKeyButton", button.Button)
HomeKeyButtonAction = homekit_ns.enum("HomeKeyButtonAction", is_class=True)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            # Resets HomeKit (HAP) pairing so another Apple Home can pair, but
            # keeps the HomeKey reader keys / credentials of all Homes.
            cv.Optional(CONF_RESET_PAIRING_KEEP_HOMEKEYS): button.button_schema(
                HomeKeyButton,
                device_class=DEVICE_CLASS_RESTART,
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:home-export-outline",
            ),
            # Resets HomeKit pairing AND erases every HomeKey credential.
            cv.Optional(CONF_FACTORY_RESET_HOMEKEYS): button.button_schema(
                HomeKeyButton,
                device_class=DEVICE_CLASS_RESTART,
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:key-remove",
            ),
        }
    ),
    cv.has_at_least_one_key(CONF_RESET_PAIRING_KEEP_HOMEKEYS, CONF_FACTORY_RESET_HOMEKEYS),
)


def _final_validate(config):
    if not homekey_configured(fv.full_config.get()):
        raise cv.Invalid(
            "The homekit button platform requires HomeKey (a `homekit: lock:` entry with `nfc_id`)"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    if conf := config.get(CONF_RESET_PAIRING_KEEP_HOMEKEYS):
        b = await button.new_button(conf)
        cg.add(b.set_action(HomeKeyButtonAction.RESET_PAIRING_KEEP_HOMEKEYS))
    if conf := config.get(CONF_FACTORY_RESET_HOMEKEYS):
        b = await button.new_button(conf)
        cg.add(b.set_action(HomeKeyButtonAction.FACTORY_RESET_HOMEKEYS))
