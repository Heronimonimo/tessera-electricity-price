#include "electricity_price.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome::electricity_price {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

constexpr int HOURS = 48;  // today and tomorrow, an hour each: a day of 96 quarters is folded to 24 bars

// ---- The prices: what the sensor says, folded to one price per hour ----
struct Prices {
  float hour[HOURS] = {};
  int count = 0;  // 0 (no graph), 24 (today) or 48 (today and tomorrow)
  float current = 0;
  bool has_current = false;
  bool chosen = false;  // the tile has a sensor
  char unit[16] = "";
  float lo = 0, hi = 0, avg = 0;  // of today
};

// Fold a list of prices (hourly, or 96 quarters, or 92 and 100 on the days the clock changes) to 24 hours.
static bool fold(JsonVariantConst list, float scale, float *out) {
  JsonArrayConst a = list.as<JsonArrayConst>();
  const int n = static_cast<int>(a.size());
  if (n < 24) return false;
  float sum[24] = {};
  int seen[24] = {};
  int i = 0;
  for (JsonVariantConst v : a) {
    if (v.is<float>()) {
      const int h = std::min(23, i * 24 / n);
      sum[h] += v.as<float>() * scale;
      seen[h]++;
    }
    i++;
  }
  for (int h = 0; h < 24; h++) out[h] = seen[h] ? sum[h] / seen[h] : NAN;
  return true;
}

static void read_prices(JsonObjectConst data, bool cents, Prices &p) {
  p.count = 0;
  p.has_current = false;
  p.chosen = !data["state"].isNull();
  if (!p.chosen) return;
  const float scale = cents ? 100.0f : 1.0f;
  const char *state = data["state"] | "";
  char *end = nullptr;
  const float v = strtof(state, &end);
  if (end != state && *end == 0) {
    p.current = v * scale;
    p.has_current = true;
  }
  strlcpy(p.unit, data["attributes"]["unit_of_measurement"] | "", sizeof(p.unit));
  JsonObjectConst a = data["attributes"];
  // Nord Pool names its lists raw_today, ENTSO-e prices_today; the manifest maps both, one of them is empty.
  if (fold(a["np_today"], scale, p.hour) || fold(a["en_today"], scale, p.hour)) {
    p.count = 24;
    if (fold(a["np_tomorrow"], scale, p.hour + 24) || fold(a["en_tomorrow"], scale, p.hour + 24)) p.count = 48;
  }
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int n = 0;
  for (int h = 0; h < std::min(24, p.count); h++) {
    if (std::isnan(p.hour[h])) continue;
    lo = std::min(lo, p.hour[h]);
    hi = std::max(hi, p.hour[h]);
    sum += p.hour[h];
    n++;
  }
  p.lo = n ? lo : 0;
  p.hi = n ? hi : 0;
  p.avg = n ? sum / n : 0;
}

static std::string number(float v, int decimals) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// "EUR/kWh" as "€/kWh", or "ct/kWh" when the prices are in cents.
static std::string unit_text(const Prices &p, bool cents) {
  std::string unit = p.unit;
  const size_t slash = unit.find('/');
  const std::string per = slash == std::string::npos ? "kWh" : unit.substr(slash + 1);
  std::string money = slash == std::string::npos ? unit : unit.substr(0, slash);
  if (cents) {
    money = "ct";
  } else if (money == "EUR" && ui::covers("\u20ac", Font::BODY)) {
    money = "\u20ac";
  } else if (money == "USD") {
    money = "$";
  }
  return money.empty() ? per : money + "/" + per;
}

static int hour_now(uint32_t epoch) { return epoch ? tessera::local_time(epoch).hour : -1; }

// ---- The graph: a bar for every hour, from the zero line ----
class Chart {
 public:
  void create(lv_obj_t *parent, int x, int y, int w, int h) {
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
    for (int i = 0; i < HOURS; i++) {
      bars_[i] = ui::block(parent, theme::TRACK);
      lv_obj_set_style_radius(bars_[i], ui::px(2), 0);
      lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The past is grey, the hour now the accent, the hours to come green, amber or red by their place in the day's range.
  void update(const Prices &p, int now) {
    const int n = p.count;
    float lo = 0, hi = 0, mn = INFINITY, mx = -INFINITY;
    for (int i = 0; i < n; i++) {
      if (std::isnan(p.hour[i])) continue;
      lo = std::min(lo, p.hour[i]);
      hi = std::max(hi, p.hour[i]);
      mn = std::min(mn, p.hour[i]);
      mx = std::max(mx, p.hour[i]);
    }
    const float span = hi - lo > 0.0001f ? hi - lo : 1.0f;
    const int zero = static_cast<int>(h_ * hi / span);
    for (int i = 0; i < HOURS; i++) {
      lv_obj_t *bar = bars_[i];
      if (i >= n || std::isnan(p.hour[i])) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
      const int slot = w_ / n;
      const int gap = slot >= 8 ? 2 : 1;
      const int off = (w_ - slot * n) / 2;
      const int len = std::max(2, static_cast<int>(std::fabs(p.hour[i]) / span * h_));
      lv_obj_set_size(bar, std::max(1, slot - gap), len);
      lv_obj_set_pos(bar, x_ + off + i * slot, y_ + (p.hour[i] >= 0 ? zero - len : zero));
      if (i < now) {
        ui::set_fill(bar, Tone::NORMAL, theme::TRACK);
      } else if (i == now) {
        ui::set_fill(bar, Tone::NORMAL, theme::ACCENT);
      } else {
        const float at = mx > mn ? (p.hour[i] - mn) / (mx - mn) : 0.5f;
        ui::set_fill(bar, at < 0.34f ? Tone::GOOD : at < 0.67f ? Tone::BUSY : Tone::ALERT, theme::TRACK);
      }
    }
  }

 private:
  lv_obj_t *bars_[HOURS]{};
  int x_ = 0, y_ = 0, w_ = 1, h_ = 1;
};

// What a card needs from the tile that opened it: the tile's options and its last prices.
struct Shared {
  bool cents = false;
  int decimals = 3;
  Prices prices;
};

class PriceTile : public tessera::Tile {
 public:
  PriceTile(const tessera::Plugin *plugin, Shared *shared) : plugin_(plugin), shared_(shared) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    entity_ = c.entity;
    cents_ = c.options["cents"] | false;
    decimals_ = std::min<int>(c.options["fixed"] | 3, cents_ ? 1 : 4);
    head_ = ui::line_height(Font::HEADLINE);
    const int gap = ui::px(6);
    chart_on_ = c.columns * c.rows >= 2 && height_ - head_ - gap >= ui::px(36);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    if (chart_on_) chart_.create(c.parent, 0, head_ + gap, width_, height_ - head_ - gap);
  }

  void on_state(JsonObjectConst data) override { read_prices(data, cents_, prices_); }

  void on_tick(uint32_t epoch) override {
    std::string big, small;
    if (!prices_.chosen) {
      big = plugin_->text("choose");
    } else if (!prices_.has_current) {
      big = plugin_->text("unavailable");
    } else {
      big = number(prices_.current, decimals_);
      small = unit_text(prices_, cents_);
    }
    const bool ok = prices_.chosen && prices_.has_current;
    const int gap = ui::px(6);
    const int small_h = ui::line_height(Font::BODY);
    ui::set_text(unit_, small);
    if (chart_on_) {
      // Price and unit on one line over the graph.
      Font face = Font::TITLE;
      for (Font f : {Font::HEADLINE, Font::TITLE})
        if (ui::text_width(big, f) + gap + ui::text_width(small, Font::BODY) <= width_) { face = f; break; }
      ui::set_font(value_, face);
      ui::set_text(value_, big);
      const int vh = ui::line_height(face);
      const int vw = std::min(width_, ui::text_width(big, face));
      lv_obj_set_size(value_, std::max(1, vw + 2), vh);
      lv_obj_set_pos(value_, 0, (head_ - vh) / 2);
      lv_obj_set_size(unit_, std::max(1, width_ - vw - gap), small_h);
      lv_obj_set_pos(unit_, vw + gap, (head_ - vh) / 2 + vh - small_h - ui::px(2));
      chart_.update(ok ? prices_ : Prices(), hour_now(epoch));
    } else {
      // The price in the middle with the unit under it.
      Font face = Font::TITLE;
      for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE})
        if (ui::text_width(big, f) <= width_ && ui::line_height(f) + small_h <= height_) { face = f; break; }
      ui::set_font(value_, face);
      ui::set_text(value_, big);
      const int vh = ui::line_height(face);
      const int sh = small.empty() ? 0 : small_h;
      const int top = std::max(0, (height_ - vh - sh) / 2);
      lv_obj_set_size(value_, width_, vh);
      lv_obj_set_style_text_align(value_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(value_, 0, top);
      lv_obj_set_size(unit_, width_, std::max(1, sh));
      lv_obj_set_style_text_align(unit_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(unit_, 0, top + vh);
    }
    last_epoch_ = epoch;
  }

  void on_theme() override {
    ui::set_color(value_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    on_tick(last_epoch_);
  }

  void on_tap() override {
    shared_->cents = cents_;
    shared_->decimals = decimals_;
    shared_->prices = prices_;
    tessera::open_card(plugin_->plugin_id(), "prices", entity_, tile_);
  }

 private:
  const tessera::Plugin *plugin_;
  Shared *shared_;
  std::string entity_;
  Prices prices_;
  Chart chart_;
  bool cents_ = false, chart_on_ = false;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, tile_ = -1;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{};
};

// ---- The card: the price now, the day's lowest, highest and average, and the graph of today and tomorrow ----
class PriceCard : public tessera::Card {
 public:
  PriceCard(const tessera::Plugin *plugin, Shared *shared) : plugin_(plugin), shared_(shared) {}

  void open(const tessera::CardContext &c) override {
    width_ = c.width;
    height_ = c.height;
    prices_ = shared_->prices;
    cents_ = shared_->cents;
    decimals_ = shared_->decimals;
    const int gap = ui::px(8);
    head_ = ui::line_height(Font::VALUE);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    int y = head_ + gap;
    const int col = width_ / 3;
    const int cap_h = ui::line_height(Font::BODY), val_h = ui::line_height(Font::TITLE);
    const char *keys[3] = {"low", "average", "high"};
    for (int i = 0; i < 3; i++) {
      cap_[i] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(cap_[i], col, cap_h);
      lv_obj_set_pos(cap_[i], i * col, y);
      ui::set_text(cap_[i], plugin_->text(keys[i]));
      stat_[i] = ui::label(c.parent, Font::TITLE, theme::INK);
      lv_obj_set_size(stat_[i], col, val_h);
      lv_obj_set_pos(stat_[i], i * col, y + cap_h);
    }
    y += cap_h + val_h + gap;
    const int days_h = ui::line_height(Font::BODY);
    const int chart_h = std::max(ui::px(20), height_ - y - days_h - gap);
    chart_.create(c.parent, 0, y, width_, chart_h);
    const int days_y = y + chart_h + gap / 2;
    today_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(today_, width_ / 2, days_h);
    lv_obj_set_pos(today_, 0, days_y);
    ui::set_text(today_, plugin_->text("today"));
    tomorrow_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(tomorrow_, width_ / 2, days_h);
    lv_obj_set_pos(tomorrow_, width_ / 2, days_y);
    ui::set_text(tomorrow_, plugin_->text("tomorrow"));
    on_tick(0);
  }

  void on_state(JsonObjectConst data) override {
    read_prices(data, cents_, prices_);
    on_tick(last_epoch_);
  }

  void on_tick(uint32_t epoch) override {
    last_epoch_ = epoch;
    const bool ok = prices_.chosen && prices_.has_current;
    const int gap = ui::px(8);
    const std::string big = ok ? number(prices_.current, decimals_) : plugin_->text(prices_.chosen ? "unavailable" : "choose");
    const std::string small = ok ? unit_text(prices_, cents_) : "";
    Font face = Font::TITLE;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE})
      if (ui::text_width(big, f) + gap + ui::text_width(small, Font::BODY) <= width_) { face = f; break; }
    ui::set_font(value_, face);
    ui::set_text(value_, big);
    ui::set_text(unit_, small);
    const int vh = ui::line_height(face), sh = ui::line_height(Font::BODY);
    const int vw = std::min(width_, ui::text_width(big, face));
    lv_obj_set_size(value_, std::max(1, vw + 2), vh);
    lv_obj_set_pos(value_, 0, (head_ - vh) / 2);
    lv_obj_set_size(unit_, std::max(1, width_ - vw - gap), sh);
    lv_obj_set_pos(unit_, vw + gap, (head_ - vh) / 2 + vh - sh - ui::px(2));
    const bool graph = ok && prices_.count > 0;
    ui::set_text(stat_[0], graph ? number(prices_.lo, decimals_) : "");
    ui::set_text(stat_[1], graph ? number(prices_.avg, decimals_) : "");
    ui::set_text(stat_[2], graph ? number(prices_.hi, decimals_) : "");
    chart_.update(graph ? prices_ : Prices(), hour_now(epoch));
    ui::set_text(today_, graph ? plugin_->text("today") : plugin_->text("no_graph"));
    ui::set_text(tomorrow_, prices_.count == 48 ? plugin_->text("tomorrow") : "");
  }

  void on_theme() override {
    ui::set_color(value_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    for (int i = 0; i < 3; i++) {
      ui::set_color(cap_[i], theme::MUTED);
      ui::set_color(stat_[i], theme::INK);
    }
    ui::set_color(today_, theme::MUTED);
    ui::set_color(tomorrow_, theme::MUTED);
    on_tick(last_epoch_);
  }

 private:
  const tessera::Plugin *plugin_;
  Shared *shared_;
  Prices prices_;
  Chart chart_;
  bool cents_ = false;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *cap_[3]{}, *stat_[3]{}, *today_{}, *tomorrow_{};
};

void ElectricityPrice::setup() {
  shared_ = new Shared();
  add_tile("price", [this]() { return new PriceTile(this, shared_); });
  add_card("prices", [this]() { return new PriceCard(this, shared_); }, false);
}

}  // namespace esphome::electricity_price
