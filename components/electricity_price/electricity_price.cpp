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
constexpr int MAX_CELLS = 60;  // as many as the meter's bar has
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
  int lo_slot = 0, hi_slot = 0;   // the slots of today they are in
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
  p.lo_slot = p.hi_slot = 0;
  for (int s = 0; s < (p.days ? p.per : 0); s++) {
    if (std::isnan(p.slot[s])) continue;
    if (p.slot[s] < lo) { lo = p.slot[s]; p.lo_slot = s; }
    if (p.slot[s] > hi) { hi = p.slot[s]; p.hi_slot = s; }
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

// ---- The graph, in the look of Tessera's history graph: price labels on the left, hairlines across, the bars, and
// clock times under them with a small mark each ----
constexpr int MAX_TICKS = 7;   // lines across, at most
constexpr int MAX_TIMES = 24;  // times under the graph, at most

class Chart {
 public:
  // The graph takes the rectangle x, y, w, h: its plot, the price labels left of it and the times under it.
  // `bars` is how many bars it may draw, `times` how many times it may write under the bars.
  void create(lv_obj_t *parent, int x, int y, int w, int h, int bars, int times, bool cents) {
    cents_ = cents;
    bars_n_ = std::min(bars, MAX_SLOTS);
    times_n_ = std::min(times, MAX_TIMES);
    const bool big = ui::large();
    label_h_ = ui::line_height(Font::BODY);
    const int side = ui::px(big ? 8 : 5), under = ui::px(big ? 8 : 4);
    label_w_ = ui::text_width(cents ? "-000" : "-0.00", Font::BODY);
    x_ = x;
    ax_ = x + label_w_ + side;
    aw_ = std::max(10, w - label_w_ - side);
    ay_ = y;
    ah_ = std::max(10, h - label_h_ - under);
    time_y_ = ay_ + ah_ + under;
    mark_y_ = ay_ + ah_ + ui::px(big ? 1 : 1);
    mark_h_ = ui::px(big ? 5 : 3);
    for (int i = 0; i < MAX_TICKS; i++) {
      grid_[i] = hairline(parent, theme::GRID);
      price_[i] = ui::label(parent, Font::BODY, theme::SUBTLE);
      lv_obj_set_style_text_align(price_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
    }
    divider_ = hairline(parent, theme::TICK);
    for (int i = 0; i < bars_n_; i++) {
      bars_[i] = ui::block(parent, theme::TRACK);
      lv_obj_set_style_radius(bars_[i], ui::px(1), 0);
      lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < times_n_; i++) {
      marks_[i] = hairline(parent, theme::TICK);
      time_[i] = ui::label(parent, Font::BODY, theme::SUBTLE);
      lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Where the second day starts, for a label above the graph; 0 without a second day.
  int midnight_x() const { return mid_x_; }
  int left() const { return ax_; }

  // The past is grey, the bar of now the accent, the bars to come green, amber or red by their place in the range.
  void update(const Prices &p, int now, uint32_t epoch) {
    const int n = p.count();
    const int group = n > bars_n_ ? (n + bars_n_ - 1) / bars_n_ : 1;
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
    mid_x_ = 0;
    if (!bars) {
      for (int i = 0; i < bars_n_; i++) lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
      for (int i = 0; i < MAX_TICKS; i++) {
        lv_obj_add_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
      }
      for (int i = 0; i < times_n_; i++) {
        lv_obj_add_flag(marks_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_add_flag(divider_, LV_OBJ_FLAG_HIDDEN);
      return;
    }

    // The lines across: a round step (1, 2 or 5 times a power of ten) that gives a few of them, from below the
    // lowest bar to above the highest.
    const float least = cents_ ? 1.0f : 0.01f;
    const int want = std::clamp(ah_ / (label_h_ * 2), 2, 5);
    const float raw = std::max(hi - lo, least) / want;
    const float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    float step = 10 * mag;
    for (float m : {1.0f, 2.0f, 5.0f})
      if (m * mag >= raw) { step = m * mag; break; }
    step = std::max(step, least);
    float bottom = 0, top = 0;
    int lines = 0;
    for (;;) {
      bottom = std::floor(lo / step + 0.0001f) * step;
      top = std::ceil(hi / step - 0.0001f) * step;
      if (top <= bottom) top = bottom + step;
      lines = static_cast<int>(std::lround((top - bottom) / step)) + 1;
      if (lines <= MAX_TICKS) break;
      step *= 2;
    }
    const float range = top - bottom;
    for (int i = 0; i < MAX_TICKS; i++) {
      if (i >= lines) {
        lv_obj_add_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const float value = bottom + step * i;
      const int ty = ay_ + static_cast<int>(std::lround(ah_ * (top - value) / range));
      lv_obj_clear_flag(grid_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(grid_[i], ax_, std::min(ty, ay_ + ah_ - 1));
      ui::set_fill(grid_[i], Tone::NORMAL, theme::GRID);
      lv_obj_clear_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(price_[i], label_w_ + 2, label_h_);
      lv_obj_set_pos(price_[i], x_, std::max(ay_ - label_h_ / 2, ty - label_h_ / 2));
      ui::set_text(price_[i], number(std::fabs(value) < step / 100 ? 0.0f : value, cents_ ? 0 : 2));
      ui::set_color(price_[i], theme::SUBTLE);
    }

    // The bars, from the line of zero.
    const int slot = aw_ / bars;
    const int gap = slot >= 6 ? 2 : (slot >= 3 ? 1 : 0);
    const int off = (aw_ - slot * bars) / 2;
    const int zero = ay_ + static_cast<int>(std::lround(ah_ * top / range));
    const int now_bar = now < 0 ? -1 : now / group;
    for (int i = 0; i < bars_n_; i++) {
      lv_obj_t *bar = bars_[i];
      if (i >= bars || std::isnan(v[i])) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
      const int len = std::max(2, static_cast<int>(std::lround(std::fabs(v[i]) / range * ah_)));
      lv_obj_set_size(bar, std::max(1, slot - gap), len);
      lv_obj_set_pos(bar, ax_ + off + i * slot, v[i] >= 0 ? zero - len : zero);
      if (i < now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::TRACK);
      } else if (i == now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::ACCENT);
      } else {
        const float at = mx > mn ? (v[i] - mn) / (mx - mn) : 0.5f;
        ui::set_fill(bar, at < 0.34f ? Tone::GOOD : at < 0.67f ? Tone::BUSY : Tone::ALERT, theme::TRACK);
      }
    }

    // The line between the days.
    if (p.days == 2) {
      mid_x_ = ax_ + off + (p.per / group) * slot;
      lv_obj_clear_flag(divider_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(divider_, 1, ah_);
      lv_obj_set_pos(divider_, mid_x_, ay_);
      ui::set_fill(divider_, Tone::NORMAL, theme::TICK);
    } else {
      lv_obj_add_flag(divider_, LV_OBJ_FLAG_HIDDEN);
    }

    // The times under the bars: the clock hours that fit, a mark over each, as the history graph writes them.
    for (int i = 0; i < times_n_; i++) {
      lv_obj_add_flag(marks_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!epoch) return;
    const tessera::LocalTime t = tessera::local_time(epoch);
    const uint32_t midnight = epoch - (t.hour * 3600 + t.minute * 60 + t.second);
    const int hours = 24 * p.days, between = ui::px(ui::large() ? 10 : 6);
    const int sample = ui::text_width(tessera::clock_text(midnight + 8 * 3600), Font::BODY);
    const float per_hour = static_cast<float>(slot * bars) / hours;
    int every = 12;
    for (int h : {1, 2, 3, 4, 6, 12})
      if (hours / h <= times_n_ && (sample + between) <= h * per_hour) { every = h; break; }
    int last = ax_ - 1000, k = 0;
    for (int h = 0; h < hours && k < times_n_; h += every) {
      const std::string words = tessera::clock_text(midnight + (h % 24) * 3600);
      const int tw = ui::text_width(words, Font::BODY);
      const int cx = ax_ + off + static_cast<int>(std::lround(per_hour * h));
      const int left = std::max(ax_ - ui::px(ui::large() ? 8 : 4), cx - tw / 2);
      if (left <= last + between || left + tw > ax_ + aw_) continue;
      lv_obj_clear_flag(marks_[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(marks_[k], 1, mark_h_);
      lv_obj_set_pos(marks_[k], cx, mark_y_);
      ui::set_fill(marks_[k], Tone::NORMAL, theme::TICK);
      lv_obj_clear_flag(time_[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(time_[k], tw + 2, label_h_);
      lv_obj_set_pos(time_[k], left, time_y_);
      ui::set_text(time_[k], words);
      ui::set_color(time_[k], theme::SUBTLE);
      last = left + tw;
      k++;
    }
  }

 private:
  static lv_obj_t *hairline(lv_obj_t *parent, theme::Role role) {
    lv_obj_t *line = ui::block(parent, role);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_size(line, 1, 1);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return line;
  }

  lv_obj_t *bars_[MAX_SLOTS]{}, *grid_[MAX_TICKS]{}, *price_[MAX_TICKS]{}, *marks_[MAX_TIMES]{}, *time_[MAX_TIMES]{};
  lv_obj_t *divider_{};
  bool cents_ = false;
  int x_ = 0, ax_ = 0, ay_ = 0, aw_ = 1, ah_ = 1, label_w_ = 0, label_h_ = 0, time_y_ = 0, mark_y_ = 0, mark_h_ = 0;
  int bars_n_ = 0, times_n_ = 0, mid_x_ = 0;
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
    cents_ = strcmp(c.options["unit"] | "eur", "cents") == 0;
    decimals_ = std::min<int>(c.options["fixed"] | 3, cents_ ? 1 : 4);
    head_ = ui::line_height(Font::HEADLINE);
    const int gap = ui::px(6);
    const int axis = ui::line_height(Font::BODY) + ui::px(ui::large() ? 8 : 4);
    chart_on_ = c.columns * c.rows >= 2 && height_ - head_ - gap >= axis + ui::px(36);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    if (chart_on_) {
      chart_.create(c.parent, 0, head_ + gap, width_, height_ - head_ - gap, TILE_BARS, 12, cents_);
      return;
    }
    // The meter's bar of cells (Tessera's sensor meter): narrow cells along the bottom of the tile, lit up to where
    // the price stands between the day's lowest and highest.
    cells_h_ = std::clamp(height_ * 20 / 100, ui::px(10), ui::px(26));
    const int cell = std::clamp(cells_h_ / 12, std::max(2, ui::px(3)), std::max(2, ui::px(5)));
    cell_gap_ = std::max(1, cell * 3 / 5);
    cells_n_ = std::clamp((width_ + cell_gap_) / (cell + cell_gap_), 10, MAX_CELLS);
    for (int i = 0; i < cells_n_; i++) {
      cells_[i] = ui::block(c.parent, theme::TRACK);
      lv_obj_set_style_radius(cells_[i], 0, 0);
      const int a = static_cast<int>(static_cast<long>(width_) * i / cells_n_);
      const int e = static_cast<int>(static_cast<long>(width_) * (i + 1) / cells_n_) - cell_gap_;
      lv_obj_set_size(cells_[i], std::max(1, e - a), cells_h_);
      lv_obj_set_pos(cells_[i], a, height_ - cells_h_);
    }
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
      chart_.update(ok ? prices_ : Prices(), slot_now(epoch, prices_.per), epoch);
    } else {
      // The meter's short card: the price and the unit on one row, the cells under them.
      const int room = height_ - cells_h_ - ui::px(6);
      Font face = Font::TITLE;
      for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE})
        if (ui::text_width(big, f) + gap + ui::text_width(small, Font::BODY) <= width_ && ui::line_height(f) <= room) { face = f; break; }
      ui::set_font(value_, face);
      ui::set_text(value_, big);
      const int vh = ui::line_height(face);
      const int vw = std::min(width_, ui::text_width(big, face));
      lv_obj_set_size(value_, std::max(1, vw + 2), vh);
      lv_obj_set_pos(value_, 0, 0);
      lv_obj_set_size(unit_, std::max(1, width_ - vw - gap), small_h);
      lv_obj_set_pos(unit_, vw + gap, std::max(0, vh - small_h - ui::px(2)));
      // Each cell wears the colour of its place in the range, as the graph's bars do; the cells past the price are
      // a dim tint of theirs.
      const bool known = ok && prices_.days > 0 && prices_.hi > prices_.lo;
      const float share = known ? std::clamp((prices_.current - prices_.lo) / (prices_.hi - prices_.lo), 0.0f, 1.0f) : 0.0f;
      const int lit = static_cast<int>(std::lround(share * cells_n_));
      for (int i = 0; i < cells_n_; i++) {
        const float at = (i + 0.5f) / cells_n_;
        if (!known) {
          ui::set_fill(cells_[i], Tone::NORMAL, theme::TRACK);
          lv_obj_set_style_bg_opa(cells_[i], LV_OPA_COVER, 0);
          continue;
        }
        ui::set_fill(cells_[i], at < 0.34f ? Tone::GOOD : at < 0.67f ? Tone::BUSY : Tone::ALERT, theme::TRACK);
        lv_obj_set_style_bg_opa(cells_[i], i < lit ? LV_OPA_COVER : LV_OPA_30, 0);
      }
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
  int cells_n_ = 0, cells_h_ = 0, cell_gap_ = 1;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *cells_[MAX_CELLS]{};
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
    // The header as the history card has it: the price at the left, the average in the middle, and at the right the
    // highest and the lowest with the time slot they are in.
    const int body_h = ui::line_height(Font::BODY), title_h = ui::line_height(Font::TITLE);
    left_w_ = width_ * 36 / 100;
    right_x_ = width_ * 58 / 100;
    avg_cap_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(avg_cap_, right_x_ - left_w_, body_h);
    lv_obj_set_style_text_align(avg_cap_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_cap_, left_w_, (head_ - body_h - title_h) / 2);
    avg_ = ui::label(c.parent, Font::TITLE, theme::INK);
    lv_obj_set_size(avg_, right_x_ - left_w_, title_h);
    lv_obj_set_style_text_align(avg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_, left_w_, (head_ - body_h - title_h) / 2 + body_h);
    for (int i = 0; i < 2; i++) {
      extreme_[i] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_size(extreme_[i], width_ - right_x_, body_h);
      lv_obj_set_style_text_align(extreme_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(extreme_[i], right_x_, (head_ - 2 * body_h) / 2 + i * body_h);
    }
    int y = head_ + gap;
    // The graph on a card of its own, as the history graph sits on one: a day label row, then the plot.
    const bool big = ui::large();
    const int edge = ui::px(big ? 14 : 8), days_h = ui::line_height(Font::BODY);
    const int card_h = std::max(ui::px(60), height_ - y - ui::px(4));
    surface_ = ui::block(c.parent, theme::CARD);
    lv_obj_set_size(surface_, width_, card_h);
    lv_obj_set_pos(surface_, 0, y);
    lv_obj_set_style_radius(surface_, ui::px(big ? 16 : 10), 0);
    lv_obj_set_style_border_width(surface_, 1, 0);
    const int top = y + ui::px(big ? 8 : 5);
    today_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_size(today_, width_ / 2, days_h);
    tomorrow_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_size(tomorrow_, width_ / 2, days_h);
    lv_obj_set_pos(today_, edge, top);
    const int plot_y = top + days_h + ui::px(big ? 8 : 4);
    chart_.create(c.parent, edge, plot_y, width_ - 2 * edge, y + card_h - ui::px(big ? 10 : 6) - plot_y, MAX_SLOTS, MAX_TIMES,
                  cents_);
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
      if (ui::text_width(big, f) + gap + ui::text_width(small, Font::BODY) <= left_w_) { face = f; break; }
    ui::set_font(value_, face);
    ui::set_text(value_, big);
    ui::set_text(unit_, small);
    const int vh = ui::line_height(face), sh = ui::line_height(Font::BODY);
    const int vw = std::min(left_w_, ui::text_width(big, face));
    lv_obj_set_size(value_, std::max(1, vw + 2), vh);
    lv_obj_set_pos(value_, 0, (head_ - vh) / 2);
    lv_obj_set_size(unit_, std::max(1, left_w_ - vw - gap), sh);
    lv_obj_set_pos(unit_, vw + gap, (head_ - vh) / 2 + vh - sh - ui::px(2));
    const bool graph = ok && prices_.days > 0;
    ui::set_text(avg_cap_, graph ? plugin_->text("average") : "");
    ui::set_text(avg_, graph ? number(prices_.avg, decimals_) : "");
    ui::set_text(extreme_[0], graph ? extreme_text("high", prices_.hi, prices_.hi_slot, epoch) : "");
    ui::set_text(extreme_[1], graph ? extreme_text("low", prices_.lo, prices_.lo_slot, epoch) : "");
    chart_.update(graph ? prices_ : Prices(), slot_now(epoch, prices_.per), epoch);
    ui::set_fill(surface_, Tone::NORMAL, theme::CARD);
    lv_obj_set_style_border_color(surface_, ui::color(theme::LINE), 0);
    ui::set_text(today_, plugin_->text(graph ? "today" : "no_graph"));
    ui::set_color(today_, theme::SUBTLE);
    const bool second = graph && prices_.days == 2 && chart_.midnight_x() > 0;
    ui::set_text(tomorrow_, second ? plugin_->text("tomorrow") : "");
    ui::set_color(tomorrow_, theme::SUBTLE);
    if (second) lv_obj_set_pos(tomorrow_, chart_.midnight_x() + ui::px(ui::large() ? 8 : 4), lv_obj_get_y(today_));
  }

  void on_theme() override {
    ui::set_color(value_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    ui::set_color(avg_cap_, theme::MUTED);
    ui::set_color(avg_, theme::INK);
    ui::set_color(extreme_[0], theme::MUTED);
    ui::set_color(extreme_[1], theme::MUTED);
    on_tick(last_epoch_);
  }

 private:
  // "High 0.381 · 18:00–18:15": the price and the time slot it is in; only the start when the whole does not fit.
  std::string extreme_text(const char *key, float value, int slot, uint32_t epoch) const {
    std::string words = tessera::fill(plugin_->text(key), "value", number(value, decimals_));
    if (!epoch) return words;
    const tessera::LocalTime t = tessera::local_time(epoch);
    const uint32_t midnight = epoch - (t.hour * 3600 + t.minute * 60 + t.second);
    const int minutes = 1440 / prices_.per;
    const std::string from = tessera::clock_text(midnight + slot * minutes * 60);
    const std::string dash = ui::covers("\u2013", Font::BODY) ? "\u2013" : "-";
    const std::string both = words + " \u00b7 " + from + dash + tessera::clock_text(midnight + (slot + 1) * minutes * 60);
    if (ui::text_width(both, Font::BODY) <= width_ - right_x_) return both;
    return words + " \u00b7 " + from;
  }

  const tessera::Plugin *plugin_;
  Shared *shared_;
  Prices prices_;
  Chart chart_;
  bool cents_ = false;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, left_w_ = 0, right_x_ = 0;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *avg_cap_{}, *avg_{}, *extreme_[2]{}, *today_{}, *tomorrow_{}, *surface_{};
};

void ElectricityPrice::setup() {
  shared_ = new Shared();
  // Two tiles for the two integrations: the inspector lists only sensors that have the attributes of that one.
  add_tile("nordpool", [this]() { return new PriceTile(this, shared_); });
  add_tile("entsoe", [this]() { return new PriceTile(this, shared_); });
  add_card("prices", [this]() { return new PriceCard(this, shared_); }, false);
}

}  // namespace esphome::electricity_price
