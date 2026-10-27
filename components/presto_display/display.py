from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display, rp2
from esphome.const import CONF_ID

DEPENDENCIES = ["rp2"]
ns = cg.esphome_ns.namespace("presto_display")
PrestoDisplay = ns.class_("PrestoDisplay", display.DisplayBuffer)
PrestoFrameDisplay = ns.class_("PrestoFrameDisplay", PrestoDisplay)
# LVGL's partial renderer: paced block writes into the back framebuffer, presented at
# vertical blank by `present_frame()` from LVGL's `on_draw_end`.
CONF_PARTIAL_FRAMES = "partial_frames"
CONF_MIN_REPORT_PIXELS = "min_report_pixels"
CONF_NATIVE_SCENE = "native_scene"


def _validate(config):
    if config[CONF_NATIVE_SCENE] and not config[CONF_PARTIAL_FRAMES]:
        raise cv.Invalid("'native_scene' needs 'partial_frames: true'")
    if not config[CONF_PARTIAL_FRAMES] and CONF_MIN_REPORT_PIXELS in config:
        raise cv.Invalid(f"'{CONF_MIN_REPORT_PIXELS}' needs '{CONF_PARTIAL_FRAMES}: true'")
    if config[CONF_PARTIAL_FRAMES] and "lambda" in config:
        raise cv.Invalid(f"'{CONF_PARTIAL_FRAMES}' is drawn by LVGL; remove the lambda")
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(PrestoDisplay),
            cv.Optional(CONF_PARTIAL_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_MIN_REPORT_PIXELS): cv.int_range(min=0, max=480 * 480),
            cv.Optional(CONF_NATIVE_SCENE, default=False): cv.boolean,
        }
    ).extend(cv.polling_component_schema("1s")),
    _validate,
)


async def to_code(config):
    if config[CONF_NATIVE_SCENE]:
        cg.add_define("USE_PRESTO_NATIVE_SCENE")
    if config[CONF_PARTIAL_FRAMES]:
        cg.add_define("USE_PRESTO_PARTIAL_FRAMES")
        config[CONF_ID].type = PrestoFrameDisplay
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    if CONF_MIN_REPORT_PIXELS in config:
        cg.add(var.set_min_report_pixels(config[CONF_MIN_REPORT_PIXELS]))
    for program in ("parallel", "timing", "palette"):
        rp2.add_pio_file(
            "presto_display",
            "presto_st7701_" + program,
            (Path(__file__).parent / ("st7701_" + program + ".pio")).read_text(),
        )
    if "lambda" in config:
        writer = await cg.process_lambda(
            config["lambda"], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(writer))
