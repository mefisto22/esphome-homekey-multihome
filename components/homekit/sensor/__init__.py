import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sensor
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT

from .. import homekit_ns, homekey_configured

DEPENDENCIES = ["homekit"]

CONF_HOMEKEY_HOMES = "homekey_homes"
CONF_HOMEKEY_ISSUERS = "homekey_issuers"
CONF_HOMEKEY_ENDPOINTS = "homekey_endpoints"

HomeKeyDiagnostics = homekit_ns.class_("HomeKeyDiagnostics", cg.Component)


def _count_schema(icon):
    return sensor.sensor_schema(
        accuracy_decimals=0,
        icon=icon,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HomeKeyDiagnostics),
            # Number of Apple Homes (reader keys) whose HomeKeys are accepted.
            cv.Optional(CONF_HOMEKEY_HOMES): _count_schema("mdi:home-group"),
            # Number of HomeKey issuers (Home users) across all Homes.
            cv.Optional(CONF_HOMEKEY_ISSUERS): _count_schema("mdi:account-key"),
            # Number of HomeKey endpoints (iPhones / Watches) across all Homes.
            cv.Optional(CONF_HOMEKEY_ENDPOINTS): _count_schema("mdi:cellphone-key"),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_HOMEKEY_HOMES, CONF_HOMEKEY_ISSUERS, CONF_HOMEKEY_ENDPOINTS),
)


def _final_validate(config):
    if not homekey_configured(fv.full_config.get()):
        raise cv.Invalid(
            "The homekit sensor platform requires HomeKey (a `homekit: lock:` entry with `nfc_id`)"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if conf := config.get(CONF_HOMEKEY_HOMES):
        cg.add(var.set_homes_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_HOMEKEY_ISSUERS):
        cg.add(var.set_issuers_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_HOMEKEY_ENDPOINTS):
        cg.add(var.set_endpoints_sensor(await sensor.new_sensor(conf)))
