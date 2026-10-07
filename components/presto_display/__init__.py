"""Pimoroni Presto hardware and optional atomic RGB565 receiver."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_DISPLAY_ID, CONF_ID

from .display import PrestoDisplay, ns

AUTO_LOAD = ["text_sensor"]
PrestoRemote = ns.class_("PrestoRemote", cg.Component)
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrestoRemote),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(PrestoDisplay),
        cv.Required("events_id"): cv.use_id(text_sensor.TextSensor),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_define("USE_PRESTO_REMOTE")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    cg.add(var.set_events(await cg.get_variable(config["events_id"])))
