import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC

from .. import homekit_ns, homekey_configured

DEPENDENCIES = ["homekit"]

CONF_HOMEKEY_PROVISIONED = "homekey_provisioned"
CONF_HOMEKEY_ACTIVE_HOME_PROVISIONED = "homekey_active_home_provisioned"

HomeKeyDiagnostics = homekit_ns.class_("HomeKeyDiagnostics", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HomeKeyDiagnostics),
            # ON when at least one Home has written a reader key.
            cv.Optional(CONF_HOMEKEY_PROVISIONED): binary_sensor.binary_sensor_schema(
                icon="mdi:key-chain",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # ON when the currently HomeKit-paired Home has written its reader key.
            cv.Optional(CONF_HOMEKEY_ACTIVE_HOME_PROVISIONED): binary_sensor.binary_sensor_schema(
                icon="mdi:home-lock",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_HOMEKEY_PROVISIONED, CONF_HOMEKEY_ACTIVE_HOME_PROVISIONED),
)


def _final_validate(config):
    if not homekey_configured(fv.full_config.get()):
        raise cv.Invalid(
            "The homekit binary_sensor platform requires HomeKey (a `homekit: lock:` entry with `nfc_id`)"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if conf := config.get(CONF_HOMEKEY_PROVISIONED):
        cg.add(var.set_provisioned_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_HOMEKEY_ACTIVE_HOME_PROVISIONED):
        cg.add(var.set_active_home_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
