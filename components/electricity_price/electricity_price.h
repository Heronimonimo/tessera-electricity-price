#pragma once
#include <string>
#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "formula.h"

namespace esphome::electricity_price {

struct Shared;

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tiles, its card and its top bar item in
// setup().
class ElectricityPrice : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // The entities of plugin.yaml the top bar's item reads: the price sensor chosen when the plugin was added, its unit,
  // and the two settings, the formula and the unit to show.
  void set_price(sensor::Sensor *s) { price_ = s; }
  void set_unit(text_sensor::TextSensor *s) { unit_ = s; }
  void set_bar_formula(text::Text *t) { bar_formula_ = t; }
  void set_bar_unit(select::Select *s) { bar_unit_ = s; }

 private:
  tessera::BarItem bar();

  Shared *shared_ = nullptr;  // what a card takes over from the tile that opened it
  sensor::Sensor *price_{};
  text_sensor::TextSensor *unit_{};
  text::Text *bar_formula_{};
  select::Select *bar_unit_{};
  // The bar's formula as last read, read again only when the setting changes.
  std::string formula_text_;
  Formula formula_;
  bool formula_ok_ = true;
};

}  // namespace esphome::electricity_price
