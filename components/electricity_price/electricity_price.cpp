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

constexpr int MAX_SLOTS = 192;  // two days of 96 quarters
constexpr int TILE_BARS = 96;   // a tile draws at most this many bars (memory); the card draws them all

// ---- The prices: what the sensor says, one price per slot (a quarter of an hour when the sensor has them, else an hour) ----
struct Prices {
  float slot[MAX_SLOTS] = {};
  int per = 24;   // slots per day: 96 or 24
  int days = 0;   // 0 (no graph), 1 (today) or 2 (today and tomorrow)
  float current = 0;
  bool has_current = false;
  bool chosen = false;  // the tile has a sensor
  char unit[16] = "";
  float lo = 0, hi = 0, avg = 0;  // of today
  int count() const { return per * days; }
};

// Put a list of a day's prices (24 hours, 96 quarters, or 23/25 hours and 92/100 quarters on the days the clock
// changes) into `per` slots.
static bool fold(JsonVariantConst list, float scale, int per, float *out) {
  JsonArrayConst a = list.as<JsonArrayConst>();
  const int n = static_cast<int>(a.size());
  if (n < 23) return false;
  float sum[96] = {};
  int seen[96] = {};
  int i = 0;
  for (JsonVariantConst v : a) {
    if (v.is<float>()) {
      const int s = std::min(per - 1, i * per / n);
      sum[s] += v.as<float>() * scale;
      seen[s]++;
    }
    i++;
  }
  for (int s = 0; s < per; s++) out[s] = seen[s] ? sum[s] / seen[s] : NAN;
  return true;
}

static void read_prices(JsonObjectConst data, bool cents, Prices &p) {
  p.days = 0;
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
  const int today = a["today"].as<JsonArrayConst>().size();
  p.per = today >= 90 ? 96 : 24;
  if (fold(a["today"], scale, p.per, p.slot)) {
    p.days = 1;
    if (fold(a["tomorrow"], scale, p.per, p.slot + p.per)) p.days = 2;
  }
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int n = 0;
  for (int s = 0; s < (p.days ? p.per : 0); s++) {
    if (std::isnan(p.slot[s])) continue;
    lo = std::min(lo, p.slot[s]);
    hi = std::max(hi, p.slot[s]);
    sum += p.slot[s];
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

// The slot of the moment now in today's `per` slots; -1 while the clock is not set.
static int slot_now(uint32_t epoch, int per) {
  if (!epoch) return -1;
  const tessera::LocalTime t = tessera::local_time(epoch);
  return (t.hour * 60 + t.minute) * per / 1440;
}

// ---- The graph: a bar for every slot (or for a few slots together when there are more than it has bars) ----
class Chart {
 public:
  void create(lv_obj_t *parent, int x, int y, int w, int h, int cap) {
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
    cap_ = std::min(cap, MAX_SLOTS);
    for (int i = 0; i < cap_; i++) {
      bars_[i] = ui::block(parent, theme::TRACK);
      lv_obj_set_style_radius(bars_[i], ui::px(1), 0);
      lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The past is grey, the bar of now the accent, the bars to come green, amber or red by their place in the range.
  void update(const Prices &p, int now) {
    const int n = p.count();
    const int group = n > cap_ ? (n + cap_ - 1) / cap_ : 1;
    const int bars = n ? (n + group - 1) / group : 0;
    float v[MAX_SLOTS];
    float lo = 0, hi = 0, mn = INFINITY, mx = -INFINITY;
    for (int b = 0; b < bars; b++) {
      float sum = 0;
      int seen = 0;
      for (int s = b * group; s < std::min(n, (b + 1) * group); s++) {
        if (std::isnan(p.slot[s])) continue;
        sum += p.slot[s];
        seen++;
      }
      v[b] = seen ? sum / seen : NAN;
      if (std::isnan(v[b])) continue;
      lo = std::min(lo, v[b]);
      hi = std::max(hi, v[b]);
      mn = std::min(mn, v[b]);
      mx = std::max(mx, v[b]);
    }
    const float span = hi - lo > 0.0001f ? hi - lo : 1.0f;
    const int zero = static_cast<int>(h_ * hi / span);
    const int now_bar = now < 0 ? -1 : now / group;
    for (int i = 0; i < cap_; i++) {
      lv_obj_t *bar = bars_[i];
      if (i >= bars || std::isnan(v[i])) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
      const int slot = w_ / bars;
      const int gap = slot >= 6 ? 2 : (slot >= 3 ? 1 : 0);
      const int off = (w_ - slot * bars) / 2;
      const int len = std::max(2, static_cast<int>(std::fabs(v[i]) / span * h_));
      lv_obj_set_size(bar, std::max(1, slot - gap), len);
      lv_obj_set_pos(bar, x_ + off + i * slot, y_ + (v[i] >= 0 ? zero - len : zero));
      if (i < now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::TRACK);
      } else if (i == now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::ACCENT);
      } else {
        const float at = mx > mn ? (v[i] - mn) / (mx - mn) : 0.5f;
        ui::set_fill(bar, at < 0.34f ? Tone::GOOD : at < 0.67f ? Tone::BUSY : Tone::ALERT, theme::TRACK);
      }
    }
  }

 private:
  lv_obj_t *bars_[MAX_SLOTS]{};
  int x_ = 0, y_ = 0, w_ = 1, h_ = 1, cap_ = 0;
};

// What a card takes over from the tile that opened it: the tile's options and its last prices.
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
    if (chart_on_) chart_.create(c.parent, 0, head_ + gap, width_, height_ - head_ - gap, TILE_BARS);
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
      chart_.update(ok ? prices_ : Prices(), slot_now(epoch, prices_.per));
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
    chart_.create(c.parent, 0, y, width_, chart_h, MAX_SLOTS);
    const int days_y = y + chart_h + gap / 2;
    today_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(today_, width_ / 2, days_h);
    lv_obj_set_pos(today_, 0, days_y);
    tomorrow_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(tomorrow_, width_ / 2, days_h);
    lv_obj_set_pos(tomorrow_, width_ / 2, days_y);
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
    const bool graph = ok && prices_.days > 0;
    ui::set_text(stat_[0], graph ? number(prices_.lo, decimals_) : "");
    ui::set_text(stat_[1], graph ? number(prices_.avg, decimals_) : "");
    ui::set_text(stat_[2], graph ? number(prices_.hi, decimals_) : "");
    chart_.update(graph ? prices_ : Prices(), slot_now(epoch, prices_.per));
    ui::set_text(today_, plugin_->text(graph ? "today" : "no_graph"));
    ui::set_text(tomorrow_, prices_.days == 2 ? plugin_->text("tomorrow") : "");
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
  // Two tiles for the two integrations: the inspector lists only sensors that have the attributes of that one.
  add_tile("nordpool", [this]() { return new PriceTile(this, shared_); });
  add_tile("entsoe", [this]() { return new PriceTile(this, shared_); });
  add_card("prices", [this]() { return new PriceCard(this, shared_); }, false);
}

}  // namespace esphome::electricity_price
