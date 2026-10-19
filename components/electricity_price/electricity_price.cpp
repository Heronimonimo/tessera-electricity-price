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

constexpr int MAX_DAY = 100;    // a day's slots, at most: 100 quarters on the day the clock goes back
constexpr int MAX_SLOTS = 2 * MAX_DAY;  // today and tomorrow
constexpr int MAX_CELLS = 60;  // as many as the meter's bar has
constexpr int TILE_BARS = 100;  // a tile draws at most this many bars (memory); the card draws them all

// ---- The prices: what the sensor says, one price per slot (a quarter of an hour when the sensor has them, else an hour) ----
struct Prices {
  float slot[MAX_SLOTS] = {};
  int today = 0;     // today's slots: 24 hours or 96 quarters, 23/25 or 92/100 on the days the clock changes; 0: no graph
  int tomorrow = 0;  // tomorrow's, in today's resolution, after today's; 0 until the sensor has them
  float current = 0;
  bool has_current = false;
  bool chosen = false;  // the tile has a sensor
  char unit[16] = "";
  float lo = 0, hi = 0, avg = 0;  // of today
  int lo_slot = 0, hi_slot = 0;   // the slots of today they are in
  int count() const { return today + tomorrow; }
};

// A list of a day's prices into `out`, one slot each: 24 hours or 96 quarters, and 23/25 hours or 92/100 quarters on
// the days the clock changes. A list finer than quarters is joined to at most MAX_DAY slots. Returns the slots, 0 for
// no list.
static int read_day(JsonVariantConst list, float scale, float *out) {
  JsonArrayConst a = list.as<JsonArrayConst>();
  const int n = static_cast<int>(a.size());
  if (n < 23) return 0;
  const int join = (n + MAX_DAY - 1) / MAX_DAY;
  const int slots = (n + join - 1) / join;
  float sum[MAX_DAY] = {};
  int seen[MAX_DAY] = {};
  int i = 0;
  for (JsonVariantConst v : a) {
    if (v.is<float>()) {
      sum[i / join] += v.as<float>() * scale;
      seen[i / join]++;
    }
    i++;
  }
  for (int s = 0; s < slots; s++) out[s] = seen[s] ? sum[s] / seen[s] : NAN;
  return slots;
}

// A day of `n` slots in quarters or in hours, so that tomorrow has today's: an hour becomes four quarters, four quarters
// one hour. Returns its slots now.
static int resolve(float *day, int n, bool quarters) {
  if ((n >= 90) == quarters) return n;
  if (quarters) {
    if (n * 4 > MAX_DAY) return 0;
    for (int s = n * 4 - 1; s >= 0; s--) day[s] = day[s / 4];
    return n * 4;
  }
  const int hours = (n + 3) / 4;
  for (int h = 0; h < hours; h++) {
    float sum = 0;
    int seen = 0;
    for (int s = h * 4; s < std::min(n, h * 4 + 4); s++) {
      if (std::isnan(day[s])) continue;
      sum += day[s];
      seen++;
    }
    day[h] = seen ? sum / seen : NAN;
  }
  return hours;
}

static void read_prices(JsonObjectConst data, bool cents, Prices &p) {
  p.today = p.tomorrow = 0;
  p.has_current = false;
  p.chosen = !data["state"].isNull();
  if (!p.chosen) return;
  // A price per MWh is made one per kWh, so the units, the decimals and the graph's labels read as for a sensor per kWh.
  snprintf(p.unit, sizeof(p.unit), "%s", data["attributes"]["unit_of_measurement"] | "");
  float scale = cents ? 100.0f : 1.0f;
  if (char *slash = strchr(p.unit, '/')) {
    if (strcmp(slash + 1, "MWh") == 0) {
      scale /= 1000.0f;
      snprintf(slash + 1, sizeof(p.unit) - (slash + 1 - p.unit), "kWh");
    }
  }
  const char *state = data["state"] | "";
  char *end = nullptr;
  const float v = strtof(state, &end);
  if (end != state && *end == 0) {
    p.current = v * scale;
    p.has_current = true;
  }
  JsonObjectConst a = data["attributes"];
  p.today = read_day(a["today"], scale, p.slot);
  if (p.today) p.tomorrow = resolve(p.slot + p.today, read_day(a["tomorrow"], scale, p.slot + p.today), p.today >= 90);
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int n = 0;
  p.lo_slot = p.hi_slot = 0;
  for (int s = 0; s < p.today; s++) {
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

// The moment the day of `epoch` began on the screen's clock: its midnight, also on a day the clock changes, when the
// time of day is an hour off the time since midnight.
static uint32_t midnight_of(uint32_t epoch) {
  uint32_t m = epoch;
  for (int i = 0; i < 3; i++) {
    const tessera::LocalTime t = tessera::local_time(m);
    const uint32_t since = t.hour * 3600 + t.minute * 60 + t.second;
    if (since == 0) break;
    // Past the clock's change, a step back of the time of day lands just before midnight: then forward to it.
    m = m != epoch && since >= 12 * 3600 ? m + (86400 - since) : m - since;
  }
  return m;
}

// When each slot is: today's from its midnight, tomorrow's from the next, each as long as its day divided by its
// slots (an hour or a quarter, also on the days of 23 and 25 hours).
struct Day {
  uint32_t start = 0, next = 0;  // today's midnight and tomorrow's; 0 while the clock is not set
  uint32_t step = 3600, next_step = 3600;
  int today = 0;
  bool known() const { return start != 0; }
  // When slot s begins; at(count()) is when the last one ends.
  uint32_t at(int s) const {
    return s < today ? start + static_cast<uint32_t>(s) * step : next + static_cast<uint32_t>(s - today) * next_step;
  }
  // The slot of the moment `epoch`, -1 while the clock is not set.
  int slot(uint32_t epoch) const {
    if (!known() || !today) return -1;
    if (epoch < next) return static_cast<int>((epoch - start) / step);
    return today + static_cast<int>((epoch - next) / next_step);
  }
};

static Day day_of(const Prices &p, uint32_t epoch) {
  Day d;
  d.today = p.today;
  if (!epoch || !p.today) return d;
  d.start = midnight_of(epoch);
  d.next = midnight_of(d.start + 26 * 3600);
  d.step = (d.next - d.start) / p.today;
  d.next_step = p.tomorrow ? (midnight_of(d.next + 26 * 3600) - d.next) / p.tomorrow : d.step;
  return d;
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

  // The bar under a finger at x, y (the chart's own pixels), or -1.
  int bar_at(int x, int y) const {
    if (bars_now_ <= 0 || y < ay_ - label_h_ || y > ay_ + ah_ + label_h_ || x < ax_ || x >= ax_ + aw_) return -1;
    const int i = static_cast<int>(static_cast<long long>(x - ax_) * bars_now_ / aw_);
    return i >= 0 && i < bars_now_ && !std::isnan(value_[i]) ? i : -1;
  }
  float bar_value(int i) const { return value_[i]; }
  // The first and the one past the last slot (of all the days) that bar i stands for.
  int bar_from(int i) const { return i * group_; }
  int bar_to(int i, int n) const { return std::min(n, (i + 1) * group_); }
  void select(int i) { sel_ = i; }

  // The past is grey, the bar of now the accent, the bars to come green, amber or red by their place in the range.
  void update(const Prices &p, const Day &d, uint32_t epoch) {
    const int n = p.count();
    const int now = d.slot(epoch);
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
    group_ = group;
    bars_now_ = bars;
    for (int b = 0; b < bars; b++) value_[b] = v[b];
    if (sel_ >= bars) sel_ = -1;
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
    // The bars share the whole width of the plot, also when it does not divide by their number.
    const int slot = aw_ / bars;
    const int gap = slot >= 6 ? 2 : (slot >= 3 ? 1 : 0);
    const auto edge_x = [&](int i) { return static_cast<int>(static_cast<long long>(aw_) * i / bars); };
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
      lv_obj_set_size(bar, std::max(1, edge_x(i + 1) - edge_x(i) - gap), len);
      lv_obj_set_pos(bar, ax_ + edge_x(i), v[i] >= 0 ? zero - len : zero);
      if (i == sel_) {
        ui::set_fill(bar, Tone::NORMAL, theme::INK);
      } else if (i < now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::TRACK);
      } else if (i == now_bar) {
        ui::set_fill(bar, Tone::NORMAL, theme::ACCENT);
      } else {
        const float at = mx > mn ? (v[i] - mn) / (mx - mn) : 0.5f;
        ui::set_fill(bar, at < 0.34f ? Tone::GOOD : at < 0.67f ? Tone::BUSY : Tone::ALERT, theme::TRACK);
      }
    }

    // The line between the days.
    if (p.tomorrow > 0) {
      mid_x_ = ax_ + edge_x(p.today / group);
      lv_obj_clear_flag(divider_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(divider_, 1, ah_);
      lv_obj_set_pos(divider_, mid_x_, ay_);
      ui::set_fill(divider_, Tone::NORMAL, theme::TICK);
    } else {
      lv_obj_add_flag(divider_, LV_OBJ_FLAG_HIDDEN);
    }

    // The times under the bars: the clock hours that fit, a mark over each, as the history graph writes them. Each
    // stands where its slot begins, so a day of 23 or 25 hours has its hours where they are.
    for (int i = 0; i < times_n_; i++) {
      lv_obj_add_flag(marks_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!d.known()) return;
    const int hours = std::max(1, static_cast<int>((d.at(n) - d.start + 1800) / 3600));
    const int between = ui::px(ui::large() ? 10 : 6);
    const int sample = ui::text_width(tessera::clock_text(d.start + 8 * 3600), Font::BODY);
    const float per_hour = static_cast<float>(aw_) / hours;
    int every = 12;
    for (int h : {1, 2, 3, 4, 6, 12})
      if (hours / h <= times_n_ && (sample + between) <= h * per_hour) { every = h; break; }
    int last = ax_ - 1000, k = 0;
    for (int s = 0; s < n && k < times_n_; s++) {
      // Only a slot that begins on a whole hour, and of those the hours of the clock every `every`.
      const uint32_t at = d.at(s), from = s < d.today ? d.start : d.next;
      if ((at - from) % 3600 != 0) continue;
      const tessera::LocalTime t = tessera::local_time(at);
      if (t.minute != 0 || t.hour % every != 0) continue;
      const std::string words = tessera::clock_text(at);
      const int tw = ui::text_width(words, Font::BODY);
      const int cx = ax_ + static_cast<int>(static_cast<long long>(aw_) * s / n);
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
  int group_ = 1, bars_now_ = 0, sel_ = -1;
  float value_[MAX_SLOTS]{};
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
    decimals_ = std::min<int>(static_cast<int>(std::lround(c.options["fixed"] | 3.0f)), cents_ ? 1 : 4);  // 3 or 3.0
    head_ = ui::line_height(Font::HEADLINE);
    const int gap = ui::px(6);
    const int axis = ui::line_height(Font::BODY) + ui::px(ui::large() ? 8 : 4);
    chart_on_ = c.columns >= 2 && c.rows >= 2 && height_ - head_ - gap >= axis + ui::px(36);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    name_text_ = c.name ? c.name : "";
    name_ = ui::label(c.parent, Font::TITLE, theme::MUTED);
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
    std::string name = name_text_;
    // A tapped bar shows its own price, and its time in place of the name, for a few seconds.
    if (sel_ >= 0 && (!ok || !chart_on_ || epoch >= sel_until_)) { sel_ = -1; chart_.select(-1); }
    const Day day = day_of(prices_, epoch);
    if (sel_ >= 0) {
      big = number(chart_.bar_value(sel_), decimals_);
      name = slot_times(day);
    }
    // A message ("Choose a price sensor") that does not fit beside the name takes the whole row.
    if (!ok && ui::text_width(big, Font::BODY_LARGE) > width_ * 60 / 100) name.clear();
    if (chart_on_) {
      // The name at the left and the price with its unit at the right, over the graph.
      place_row(big, small, name, head_);
      chart_.update(ok ? prices_ : Prices(), day, epoch);
    } else {
      // The meter's short card: the name at the left and the price with its unit at the right, in one row, the
      // cells under them.
      place_row(big, small, name, height_ - cells_h_ - ui::px(6));
      // Each cell wears the colour of its place in the range, as the graph's bars do; the cells past the price are
      // a dim tint of theirs.
      const bool known = ok && prices_.today > 0 && prices_.hi > prices_.lo;
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
    if (chart_on_ && prices_.today > 0) {
      const tessera::Point p = tessera::tap_point();
      const int hit = p.x >= 0 ? chart_.bar_at(p.x, p.y) : -1;
      // A tap on a bar shows it; a tap on the one shown, or beside the bars, opens the card.
      if (hit >= 0 && hit != sel_) {
        sel_ = hit;
        sel_until_ = last_epoch_ + 8;
        chart_.select(hit);
        on_tick(last_epoch_);
        return;
      }
    }
    shared_->cents = cents_;
    shared_->decimals = decimals_;
    shared_->prices = prices_;
    tessera::open_card(plugin_->plugin_id(), "prices", entity_, tile_);
  }

 private:
  // The name at the left and the price with its unit at the right in one row, `room` high; without a name the price
  // has the whole row.
  void place_row(const std::string &big, const std::string &small, const std::string &name, int room) {
    const std::string words = small.empty() ? big : big + " " + small;
    // The name keeps the tile-label face as the meter bar writes it; the price takes the largest face that leaves the
    // name its room, and only a name that fits no face steps down.
    const int apart = ui::px(8), most = name.empty() ? width_ - 2 : width_ * 60 / 100;
    Font face = Font::BODY, nface = Font::BODY;
    bool settled = false;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY}) {
      if (ui::text_width(words, f) > most || ui::line_height(f) > room) continue;
      if (name.empty() || ui::text_width(name, Font::TITLE) <= width_ - ui::text_width(words, f) - apart) {
        face = f;
        settled = true;
        break;
      }
    }
    if (!settled)
      for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY})
        if (ui::text_width(words, f) <= most && ui::line_height(f) <= room) { face = f; break; }
    const int vh = ui::line_height(face);
    const int vw = std::min(most, ui::text_width(words, face));
    for (Font f : {Font::TITLE, Font::BODY_LARGE, Font::BODY})
      if (ui::text_width(name, f) <= width_ - vw - apart) { nface = f; break; }
    ui::set_font(value_, face);
    ui::set_text(value_, words);
    ui::set_text(unit_, "");
    lv_obj_set_size(value_, std::max(1, vw + 2), vh);
    lv_obj_set_pos(value_, width_ - vw - 2, (room - vh) / 2);
    ui::set_font(name_, nface);
    ui::set_text(name_, name);
    const int nh = ui::line_height(nface);
    lv_obj_set_size(name_, std::max(1, width_ - vw - ui::px(8)), nh);
    lv_obj_set_pos(name_, 0, (room - vh) / 2 + (vh - nh) * 8 / 10);
    ui::set_color(name_, theme::MUTED);
  }

  // "18:00–18:15" of the shown bar.
  std::string slot_times(const Day &day) const {
    if (!day.known()) return "";
    const std::string a = tessera::clock_text(day.at(chart_.bar_from(sel_)));
    const std::string b = tessera::clock_text(day.at(chart_.bar_to(sel_, prices_.count())));
    return a + (ui::covers("\u2013", Font::BODY) ? "\u2013" : "-") + b;
  }

  const tessera::Plugin *plugin_;
  Shared *shared_;
  int sel_ = -1;
  uint32_t sel_until_ = 0;
  std::string entity_, name_text_;
  Prices prices_;
  Chart chart_;
  bool cents_ = false, chart_on_ = false;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, tile_ = -1;
  int cells_n_ = 0, cells_h_ = 0, cell_gap_ = 1;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *name_{}, *cells_[MAX_CELLS]{};
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
    const int text_h = ui::line_height(Font::BODY_LARGE);
    head_ = std::max(ui::line_height(Font::VALUE), 2 * text_h);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    // The header as the history card has it: the price at the left, the average in the middle, and at the right the
    // highest and the lowest with the time slot they are in.
    // The words are the size of the history card's: the font of the words on a key.
    left_w_ = width_ * 32 / 100;
    right_x_ = width_ * 54 / 100;
    avg_cap_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(avg_cap_, right_x_ - left_w_, text_h);
    lv_obj_set_style_text_align(avg_cap_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_cap_, left_w_, (head_ - 2 * text_h) / 2);
    avg_ = ui::label(c.parent, Font::BODY_LARGE, theme::INK);
    lv_obj_set_size(avg_, right_x_ - left_w_, text_h);
    lv_obj_set_style_text_align(avg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_, left_w_, (head_ - 2 * text_h) / 2 + text_h);
    for (int i = 0; i < 2; i++) {
      extreme_[i] = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
      lv_obj_set_size(extreme_[i], width_ - right_x_, text_h);
      lv_obj_set_style_text_align(extreme_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(extreme_[i], right_x_, (head_ - 2 * text_h) / 2 + i * text_h);
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
    // A tapped bar shows its own price and time slot for a few seconds, then the card is as it was.
    if (sel_ >= 0 && (!ok || prices_.today == 0 || epoch >= sel_until_)) { sel_ = -1; chart_.select(-1); }
    std::string big = ok ? number(prices_.current, decimals_) : plugin_->text(prices_.chosen ? "unavailable" : "choose");
    const std::string small = ok ? unit_text(prices_, cents_) : "";
    // A message in place of the price has the whole header: the average, the highest and the lowest are empty then.
    const int room = ok ? left_w_ : width_;
    Font face = Font::BODY;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY})
      if (ui::text_width(big, f) + (ok ? gap + ui::text_width(small, Font::BODY) : 0) <= room) { face = f; break; }
    ui::set_font(value_, face);
    ui::set_text(value_, big);
    ui::set_text(unit_, small);
    const int vh = ui::line_height(face), sh = ui::line_height(Font::BODY);
    const int vw = std::min(room, ui::text_width(big, face));
    lv_obj_set_size(value_, std::max(1, vw + 2), vh);
    lv_obj_set_pos(value_, 0, (head_ - vh) / 2);
    lv_obj_set_size(unit_, std::max(1, left_w_ - vw - gap), sh);
    lv_obj_set_pos(unit_, vw + gap, (head_ - vh) / 2 + vh - sh - ui::px(2));
    const bool graph = ok && prices_.today > 0;
    const Day day = day_of(prices_, epoch);
    if (graph && sel_ >= 0) {
      // The tapped bar: its time slot, and its price under it, while the price of now stays where it is.
      ui::set_text(avg_cap_, slot_times(day));
      ui::set_text(avg_, with_unit(chart_.bar_value(sel_), right_x_ - left_w_));
    } else {
      ui::set_text(avg_cap_, graph ? plugin_->text("average") : "");
      ui::set_text(avg_, graph ? with_unit(prices_.avg, right_x_ - left_w_) : "");
    }
    ui::set_text(extreme_[0], graph ? extreme_text("high", true, day, epoch) : "");
    ui::set_text(extreme_[1], graph ? extreme_text("low", false, day, epoch) : "");
    chart_.update(graph ? prices_ : Prices(), day, epoch);
    ui::set_fill(surface_, Tone::NORMAL, theme::CARD);
    lv_obj_set_style_border_color(surface_, ui::color(theme::LINE), 0);
    ui::set_text(today_, plugin_->text(graph ? "today" : "no_graph"));
    ui::set_color(today_, theme::SUBTLE);
    const bool second = graph && prices_.tomorrow > 0 && chart_.midnight_x() > 0;
    ui::set_text(tomorrow_, second ? plugin_->text("tomorrow") : "");
    ui::set_color(tomorrow_, theme::SUBTLE);
    if (second) {
      const int tx = chart_.midnight_x() + ui::px(ui::large() ? 8 : 4);
      lv_obj_set_pos(tomorrow_, tx, lv_obj_get_y(today_));
      lv_obj_set_width(tomorrow_, std::max(1, width_ - tx));
    }
  }

  // A tap on a bar shows it; a tap on the one shown, or elsewhere, puts the card back.
  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    const int hit = p.x >= 0 && prices_.today > 0 ? chart_.bar_at(p.x, p.y) : -1;
    sel_ = hit >= 0 && hit != sel_ ? hit : -1;
    sel_until_ = last_epoch_ + 8;
    chart_.select(sel_);
    on_tick(last_epoch_);
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
  // "18:00–18:15" of the shown bar; only the start when it does not fit in the middle.
  std::string slot_times(const Day &day) const {
    if (!day.known()) return "";
    const std::string a = tessera::clock_text(day.at(chart_.bar_from(sel_)));
    const std::string b = tessera::clock_text(day.at(chart_.bar_to(sel_, prices_.count())));
    const std::string both = a + (ui::covers("\u2013", Font::BODY_LARGE) ? "\u2013" : "-") + b;
    return ui::text_width(both, Font::BODY_LARGE) <= right_x_ - left_w_ ? both : a;
  }

  // "High 0.381 · 18:00–18:15": the price and the time slot it is in; only the start when the whole does not fit.
  // The price with its unit behind it; the price alone when both do not fit in `room`.
  std::string with_unit(float value, int room) const {
    const std::string n = number(value, decimals_), unit = unit_text(prices_, cents_);
    const std::string both = unit.empty() ? n : n + " " + unit;
    return ui::text_width(both, Font::BODY_LARGE) <= room ? both : n;
  }

  // The highest or lowest price from the slot of now on (today's rest and tomorrow); the clock not set: all of today.
  bool extreme(bool high, int now, float &value, int &slot) const {
    bool found = false;
    for (int s = std::max(now, 0); s < prices_.count(); s++) {
      const float v = prices_.slot[s];
      if (std::isnan(v) || (found && (high ? v <= value : v >= value))) continue;
      value = v;
      slot = s;
      found = true;
    }
    return found;
  }

  std::string extreme_text(const char *key, bool high, const Day &day, uint32_t epoch) const {
    float value = 0;
    int slot = 0;
    const int now = day.slot(epoch);
    if (!extreme(high, now, value, slot)) return "";
    std::string words = tessera::fill(plugin_->text(key), "value", with_unit(value, width_));
    if (ui::text_width(words, Font::BODY_LARGE) > width_ - right_x_)
      words = tessera::fill(plugin_->text(key), "value", number(value, decimals_));
    if (!day.known()) return words;
    const std::string when = slot >= prices_.today ? std::string(plugin_->text("tomorrow")) + " " : std::string();
    const std::string from = when + tessera::clock_text(day.at(slot));
    const std::string dash = ui::covers("\u2013", Font::BODY_LARGE) ? "\u2013" : "-";
    const std::string both = words + " \u00b7 " + from + dash + tessera::clock_text(day.at(slot + 1));
    if (ui::text_width(both, Font::BODY_LARGE) <= width_ - right_x_) return both;
    const std::string start = words + " \u00b7 " + from;
    return ui::text_width(start, Font::BODY_LARGE) <= width_ - right_x_ ? start : words;
  }

  const tessera::Plugin *plugin_;
  Shared *shared_;
  Prices prices_;
  Chart chart_;
  bool cents_ = false;
  int sel_ = -1;
  uint32_t sel_until_ = 0;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, left_w_ = 0, right_x_ = 0;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *avg_cap_{}, *avg_{}, *extreme_[2]{}, *today_{}, *tomorrow_{}, *surface_{};
};

void ElectricityPrice::setup() {
  shared_ = new Shared();
  // Two tiles for the two integrations: the inspector lists only sensors that have the attributes of that one.
  add_tile("nordpool", [this]() { return new PriceTile(this, shared_); });
  add_tile("entsoe", [this]() { return new PriceTile(this, shared_); });
  add_tile("gespot", [this]() { return new PriceTile(this, shared_); });
  add_card("prices", [this]() { return new PriceCard(this, shared_); }, true);
}

}  // namespace esphome::electricity_price
