from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light, rp2
from esphome.const import CONF_OUTPUT_ID

DEPENDENCIES = ["rp2"]
ns = cg.esphome_ns.namespace("presto_display")
PrestoAmbient = ns.class_("PrestoAmbient", light.AddressableLight)
CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(PrestoAmbient),
    }
)


async def to_code(config):
    cg.add_define("USE_PRESTO_AMBIENT")
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    await cg.register_component(var, config)
    rp2.add_pio_file(
        "presto_display", "presto_ws2812", (Path(__file__).parent / "ws2812.pio").read_text()
    )
