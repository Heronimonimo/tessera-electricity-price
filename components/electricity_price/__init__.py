"""Electricity price: the ESPHome side. The tiles and the card are in electricity_price.cpp, and so is the top bar's
item, which reads the price sensor and its two settings from the entities of plugin.yaml. smart_display.register_plugin()
reads the id, version, tile memory and the screen's texts from the manifest and translations/ beside this folder
(docs/FIRMWARE_API.md)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select, sensor, smart_display, text, text_sensor
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display"]

CONF_PRICE = "price"
CONF_UNIT = "unit"
CONF_BAR_FORMULA = "bar_formula"
CONF_BAR_UNIT = "bar_unit"

electricity_price_ns = cg.esphome_ns.namespace("electricity_price")
ElectricityPrice = electricity_price_ns.class_("ElectricityPrice", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ElectricityPrice),
        cv.Required(CONF_PRICE): cv.use_id(sensor.Sensor),
        cv.Required(CONF_UNIT): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_BAR_FORMULA): cv.use_id(text.Text),
        cv.Required(CONF_BAR_UNIT): cv.use_id(select.Select),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_price(await cg.get_variable(config[CONF_PRICE])))
    cg.add(var.set_unit(await cg.get_variable(config[CONF_UNIT])))
    cg.add(var.set_bar_formula(await cg.get_variable(config[CONF_BAR_FORMULA])))
    cg.add(var.set_bar_unit(await cg.get_variable(config[CONF_BAR_UNIT])))
