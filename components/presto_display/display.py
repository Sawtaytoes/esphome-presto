from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display, rp2
from esphome.const import CONF_ID

DEPENDENCIES = ["rp2"]
ns = cg.esphome_ns.namespace("presto_display")
PrestoDisplay = ns.class_("PrestoDisplay", display.DisplayBuffer)
CONFIG_SCHEMA = display.FULL_DISPLAY_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(PrestoDisplay),
}).extend(cv.polling_component_schema("1s"))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    for program in ("parallel", "timing", "palette"):
        rp2.add_pio_file("presto_display", "presto_st7701_" + program,
                         (Path(__file__).parent / ("st7701_" + program + ".pio")).read_text())
    if "lambda" in config:
        writer = await cg.process_lambda(config["lambda"], [(display.DisplayRef, "it")], return_type=cg.void)
        cg.add(var.set_writer(writer))
