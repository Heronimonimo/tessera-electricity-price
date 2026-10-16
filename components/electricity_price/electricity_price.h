#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::electricity_price {

struct Shared;

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tile and card in setup().
class ElectricityPrice : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 private:
  Shared *shared_ = nullptr;  // what a card takes over from the tile that opened it
};

}  // namespace esphome::electricity_price
