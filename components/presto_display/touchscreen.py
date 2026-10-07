"""Presto's FT6236 over its dedicated I2C1 bus, with bounded recovery."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import touchscreen
from esphome.const import CONF_ID

from .display import ns

DEPENDENCIES = ["rp2"]
PrestoTouch = ns.class_("PrestoTouch", touchscreen.Touchscreen)
CONFIG_SCHEMA = touchscreen.TOUCHSCREEN_SCHEMA.extend({cv.GenerateID(): cv.declare_id(PrestoTouch)})


async def to_code(config):
    cg.add_define("USE_PRESTO_TOUCH")
    var = cg.new_Pvariable(config[CONF_ID])
    await touchscreen.register_touchscreen(var, config)
