import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import output
from esphome.const import CONF_DISPLAY_ID, CONF_ID

from .display import PrestoDisplay, ns

DEPENDENCIES = ["rp2"]
PrestoBacklight = ns.class_("PrestoBacklight", output.FloatOutput)
CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(PrestoBacklight),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(PrestoDisplay),
    }
)


async def to_code(config):
    cg.add_define("USE_PRESTO_BACKLIGHT")
    var = cg.new_Pvariable(config[CONF_ID])
    await output.register_output(var, config)
    parent = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(parent))
