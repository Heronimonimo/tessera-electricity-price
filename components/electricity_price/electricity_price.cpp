#include "electricity_price.h"
#include "formula.h"
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
constexpr int MAX_SLOTS = 3 * MAX_DAY;  // today, tomorrow and, for the hours ahead, a day of Carnot's forecast
constexpr int CHART_SLOTS = 2 * MAX_DAY;  // a graph draws at most this many bars: two days, or 48 hours ahead
constexpr int MAX_FORECAST = 80;  // forecast hours kept: 48 ahead, and today's hours the sensor may put first
constexpr int MAX_CHEAP = 6;  // cheapest hours marked, at most
constexpr int MAX_CELLS = 60;  // as many as the meter's bar has
constexpr int TILE_BARS = 100;  // a tile draws at most this many bars; the card draws them all

// ---- The prices: what the sensor says, one price per slot (a quarter of an hour when the sensor has them, else an hour) ----
struct Prices {
  float slot[MAX_SLOTS] = {};  // the delivery price, the tariff's formula applied
  tessera::List<float> feed;   // the feed-in price per slot; empty (and no memory) while the tariff has none
  int today = 0;     // today's slots: 24 hours or 96 quarters, 23/25 or 92/100 on the days the clock changes; 0: no graph
  int tomorrow = 0;  // tomorrow's, in today's resolution, after today's; 0 until the sensor has them
  float current = 0, feed_current = 0;  // the price of now: today's list at now, else the sensor's state
  bool has_current = false;
  float state_current = 0, state_feed = 0;  // the sensor's state through the tariff
  bool state_ok = false;
  bool has_feed = false;  // the tariff has a feed-in price: `feed` and `feed_current` hold it
  bool chosen = false;  // the tile has a sensor
  // The slots hold yesterday and today in place of today and tomorrow: a graph that starts at an hour of the day, before
  // that hour, shows yesterday from that hour on. `today` and `tomorrow` then count yesterday's slots and today's.
  bool back = false;
  char unit[16] = "";
  float lo = 0, hi = 0, avg = 0;  // of today
  int lo_slot = 0, hi_slot = 0;   // the slots of today they are in
  // Carnot's forecast of the sensor (attribute `forecast`), an hour each, through the tariff only once it is placed
  // after the known prices (place_forecast): `extra` slots after tomorrow's, 0 while the graph shows whole days.
  float forecast[MAX_FORECAST] = {};
  int forecast_n = 0;
  uint32_t forecast_start = 0;  // when its first hour begins
  int extra = 0;
  int known() const { return today + tomorrow; }
  int count() const { return today + tomorrow + extra; }
};

// A list of a day's prices into `out`, one slot each: 24 hours or 96 quarters, and 23/25 hours or 92/100 quarters on
// the days the clock changes. A list finer than quarters is joined to at most MAX_DAY slots. Returns the slots, 0 for
// no list.
// `n` prices of the list `a` from its price `from` on, as read_day reads a whole one.
static int read_span(JsonArrayConst a, int from, int n, float scale, float *out) {
  if (n < 23 || from < 0) return 0;
  const int join = (n + MAX_DAY - 1) / MAX_DAY;
  const int slots = (n + join - 1) / join;
  float sum[MAX_DAY] = {};
  int seen[MAX_DAY] = {};
  int k = 0;
  for (JsonVariantConst v : a) {
    const int i = k++ - from;
    if (i < 0) continue;
    if (i >= n) break;
    if (v.is<float>()) {
      sum[i / join] += v.as<float>() * scale;
      seen[i / join]++;
    }
  }
  for (int s = 0; s < slots; s++) out[s] = seen[s] ? sum[s] / seen[s] : NAN;
  return slots;
}

static int read_day(JsonVariantConst list, float scale, float *out) {
  JsonArrayConst a = list.as<JsonArrayConst>();
  return read_span(a, 0, static_cast<int>(a.size()), scale, out);
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

static uint32_t midnight_of(uint32_t epoch);

// The EPEX Spot sensor (HACS `epex_spot`) keeps one list of yesterday, today and, from the afternoon, tomorrow, each
// slot with its start. Today's slots begin at the screen's midnight and tomorrow's at the next: both are cut out of the
// list by the start of its first slot and the length of a slot (an hour, or a quarter when the sensor has them). With
// `back` the two days cut out are yesterday and today. False while the clock is not set or the list does not reach the
// first of them. After Nekromonger's tessera-strompreis.
static bool read_epex(JsonObjectConst a, float scale, Prices &p, bool back) {
  JsonArrayConst list = a["prices"].as<JsonArrayConst>();
  const int n = static_cast<int>(list.size());
  const uint32_t first = a["first"].as<uint32_t>(), second = a["second"].as<uint32_t>(), now = tessera::epoch();
  if (n < 2 || !first || !now) return false;
  const uint32_t step = second > first && second - first <= 3600 ? second - first : 3600;
  const uint32_t today_mid = midnight_of(now);
  const uint32_t mid = back ? midnight_of(today_mid - 1) : today_mid, next = midnight_of(mid + 26 * 3600),
                 after = midnight_of(next + 26 * 3600);
  if (mid < first) return false;
  const int from = static_cast<int>((mid - first) / step), today = static_cast<int>((next - mid) / step);
  if (from + today > n) return false;
  p.today = read_span(list, from, today, scale, p.slot);
  if (!p.today) return false;
  const int to = from + today, tomorrow = std::min(static_cast<int>((after - next) / step), n - to);
  if (tomorrow > 0) p.tomorrow = resolve(p.slot + p.today, read_span(list, to, tomorrow, scale, p.slot + p.today), p.today >= 90);
  return true;
}

// The colour of a price: green, amber or red. By its place between the lowest and highest price shown, unless the tile
// sets prices of its own: below `cheap` green, above `expensive` red, amber between (either may be left empty).
struct Levels {
  float cheap = NAN, expensive = NAN;
  bool fixed() const { return !std::isnan(cheap) || !std::isnan(expensive); }
  Tone tone(float v, float mn, float mx) const {
    if (fixed()) return v < cheap ? Tone::GOOD : v > expensive ? Tone::ALERT : Tone::BUSY;  // NaN compares false
    const float at = mx > mn ? (v - mn) / (mx - mn) : 0.5f;
    return at < 0.34f ? Tone::GOOD : at < 0.67f ? Tone::BUSY : Tone::ALERT;
  }
};

// What the person pays and gets per kWh: the formulas typed for delivery and for feed-in, in p, the sensor's price per
// kWh in its own currency. An empty delivery formula is p; an empty feed-in formula is no feed-in price. With them the
// tile's way of showing the prices, which its card takes over.
struct Tariff {
  Formula delivery, feed_in;
  bool feed = false;   // a feed-in formula was typed
  bool hourly = false; // show the average of each hour in place of its four quarters
  bool today_only = false;  // the graph holds today alone (24 hours), never tomorrow's prices
  int ahead = 0;            // the graph shows the hours from now on, this many (24 or 48); 0: whole days
  int start = -1;           // or 24 hours from this hour of the day (0 to 23), today's or yesterday's; -1: none
  Levels levels;            // the prices of the colours, when the tile sets them
  int cheap = 0;            // how many of the cheapest hours to come the graph marks; 0: none
  bool cheap_block = false; // those hours in one row (the cheapest stretch), else each on its own
  bool error = false;  // one of them is not a formula: the tile says so in place of a price that would be wrong
};

static Tariff tariff_of(JsonObjectConst options) {
  Tariff t;
  const bool delivery = t.delivery.parse(options["delivery"] | "");
  const bool feed_in = t.feed_in.parse(options["feed_in"] | "");
  t.feed = t.feed_in.set();
  t.error = !delivery || !feed_in;
  t.hourly = strcmp(options["resolution"] | "quarter", "hour") == 0;
  // Today and tomorrow, the next 24 hours, or 24 hours from an hour of the day, which wins (an empty field is not in
  // the options).
  t.start = std::clamp(static_cast<int>(std::lround(options["start"] | -1.0f)), -1, 23);
  t.ahead = t.start < 0 && (options["ahead"] | false) ? 24 : 0;
  t.levels.cheap = options["cheap_below"] | NAN;
  t.levels.expensive = options["expensive_above"] | NAN;
  t.cheap = std::clamp(static_cast<int>(std::lround(options["cheap"] | 0.0f)), 0, MAX_CHEAP);
  t.cheap_block = strcmp(options["cheap_mode"] | "single", "block") == 0;
  return t;
}

// When a graph of 24 hours from the hour `start` (0 to 23) begins: today at that hour, or yesterday before it.
static uint32_t window_start(uint32_t epoch, int start) {
  const uint32_t today = midnight_of(epoch) + static_cast<uint32_t>(start) * 3600;
  return epoch >= today ? today : midnight_of(midnight_of(epoch) - 1) + static_cast<uint32_t>(start) * 3600;
}

static void read_prices(JsonObjectConst data, bool cents, const Tariff &tariff, Prices &p) {
  p.today = p.tomorrow = p.extra = p.forecast_n = 0;
  p.forecast_start = 0;
  const uint32_t now = tessera::epoch();
  // Before the start hour the graph begins yesterday. The prices are read again with every state of the sensor, which
  // changes with its slot, so they turn round at the start hour (after a slot of the same price, with the next one).
  p.back = tariff.start >= 0 && now && window_start(now, tariff.start) < midnight_of(now);
  p.has_current = p.has_feed = p.state_ok = false;
  p.chosen = !data["state"].isNull();
  if (!p.chosen || tariff.error) return;
  if (tariff.feed && p.feed.size() < MAX_SLOTS) p.feed.resize(MAX_SLOTS);
  // A price per MWh is made one per kWh, so the units, the decimals and the graph's labels read as for a sensor per kWh.
  snprintf(p.unit, sizeof(p.unit), "%s", data["attributes"]["unit_of_measurement"] | "");
  float scale = 1.0f;  // to the sensor's currency per kWh, which the tariff's formula takes
  const float shown = cents ? 100.0f : 1.0f;
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
    p.state_current = p.current = tariff.delivery(v * scale) * shown;
    p.state_feed = p.feed_current = tariff.feed_in(v * scale) * shown;
    p.state_ok = p.has_current = true;
    p.has_feed = tariff.feed;
  }
  JsonObjectConst a = data["attributes"];
  // Today's slots moved behind an empty yesterday, for a list without yesterday.
  const auto after_empty_yesterday = [&p](int n) {
    if (!n) return;
    for (int s = n - 1; s >= 0; s--) p.slot[n + s] = p.slot[s];
    for (int s = 0; s < n; s++) p.slot[s] = NAN;
    p.today = p.tomorrow = n;
  };
  if (!a["prices"].isNull()) {
    // EPEX Spot keeps yesterday; a list that does not reach back to yesterday's midnight gives today alone.
    if (!read_epex(a, scale, p, p.back) && p.back) {
      p.today = p.tomorrow = 0;
      if (read_epex(a, scale, p, false)) after_empty_yesterday(p.today);
    }
  } else if (p.back) {
    // Nord Pool, ENTSO-e, GE-Spot and Energi Data Service have no yesterday: its slots stay empty, today's after them.
    after_empty_yesterday(read_day(a["today"], scale, p.slot));
  } else {
    p.today = read_day(a["today"], scale, p.slot);
    if (p.today && !tariff.today_only)
      p.tomorrow = resolve(p.slot + p.today, read_day(a["tomorrow"], scale, p.slot + p.today), p.today >= 90);
  }
  // Hour prices: each hour the average of its quarters. Both days are joined in place, then tomorrow's hours move up to
  // right after today's.
  if (p.today && tariff.ahead > 0 && !p.back) {
    for (JsonVariantConst v : a["outlook"].as<JsonArrayConst>()) {
      if (p.forecast_n >= MAX_FORECAST) break;
      p.forecast[p.forecast_n++] = v.is<float>() ? v.as<float>() * scale : NAN;
    }
    p.forecast_start = static_cast<uint32_t>(a["outlook_start"] | 0.0);
  }
  if (tariff.hourly && p.today >= 90) {
    const int hours = resolve(p.slot, p.today, false);
    const int next = p.tomorrow ? resolve(p.slot + p.today, p.tomorrow, false) : 0;
    memmove(p.slot + hours, p.slot + p.today, next * sizeof(float));
    p.today = hours;
    p.tomorrow = next;
  }
  for (int s = 0; s < p.count(); s++) {
    const float kwh = p.slot[s];
    p.slot[s] = std::isnan(kwh) ? NAN : tariff.delivery(kwh) * shown;
    if (tariff.feed) p.feed[s] = std::isnan(kwh) ? NAN : tariff.feed_in(kwh) * shown;
  }
  if (p.today && tariff.feed) p.has_feed = true;
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

static const Prices NO_PRICES{};

static std::string number(float v, int decimals) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// "EUR/kWh" as "€/kWh", or "ct/kWh" when the prices are in cents; Danish prices ("DKK/kWh", "kr/kWh") in kr, or in øre
// (hundredths of a krone, "ore" where the font lacks the letter).
static std::string unit_text(const std::string &unit, bool cents) {
  const size_t slash = unit.find('/');
  const std::string per = slash == std::string::npos ? "kWh" : unit.substr(slash + 1);
  std::string money = slash == std::string::npos ? unit : unit.substr(0, slash);
  const bool krone = money == "DKK" || money == "kr";
  if (cents) {
    money = !krone ? "ct" : ui::covers("\u00f8re", Font::BODY) ? "\u00f8re" : "ore";
  } else if (krone) {
    money = "kr";
  } else if (money == "EUR" && ui::covers("\u20ac", Font::BODY)) {
    money = "\u20ac";
  } else if (money == "USD") {
    money = "$";
  }
  return money.empty() ? per : money + "/" + per;
}

static std::string unit_text(const Prices &p, bool cents) { return unit_text(std::string(p.unit), cents); }

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
// After the known prices, the forecast's slots run on from `third`, the end of the last known day.
struct Day {
  uint32_t start = 0, next = 0;  // today's midnight and tomorrow's; 0 while the clock is not set
  uint32_t step = 3600, next_step = 3600;
  uint32_t third = 0, third_step = 3600;  // where the known prices end, and the forecast's slot length
  int today = 0, tomorrow = 0;
  bool known() const { return start != 0; }
  // When slot s begins; at(count()) is when the last one ends.
  uint32_t at(int s) const {
    if (s < today) return start + static_cast<uint32_t>(s) * step;
    if (s < today + tomorrow) return next + static_cast<uint32_t>(s - today) * next_step;
    return third + static_cast<uint32_t>(s - today - tomorrow) * third_step;
  }
  // The slot of the moment `epoch`, -1 while the clock is not set.
  int slot(uint32_t epoch) const {
    if (!known() || !today) return -1;
    if (epoch < next) return static_cast<int>((epoch - start) / step);
    if (tomorrow && epoch < third) return today + static_cast<int>((epoch - next) / next_step);
    return today + tomorrow + static_cast<int>((epoch - third) / third_step);
  }
};

static Day day_of(const Prices &p, uint32_t epoch);

// The price of now: today's or tomorrow's slot of the moment `epoch` where the clock is set and the list has it, else
// the sensor's state. The state is not always the price of now: ENTSO-e keeps its lists on the sensor of the day's
// average, whose state is that average. The list is also what the bar of now shows, so the two agree.
static void price_now(Prices &p, uint32_t epoch) {
  const int s = day_of(p, epoch).slot(epoch);
  if (s >= 0 && s < p.count() && !std::isnan(p.slot[s])) {
    p.current = p.slot[s];
    p.feed_current = p.has_feed ? p.feed[s] : 0;
    p.has_current = true;
  } else {
    p.current = p.state_current;
    p.feed_current = p.state_feed;
    p.has_current = p.state_ok;
  }
}

static Day day_of(const Prices &p, uint32_t epoch) {
  Day d;
  d.today = p.today;
  if (!epoch || !p.today) return d;
  d.start = p.back ? midnight_of(midnight_of(epoch) - 1) : midnight_of(epoch);
  d.next = midnight_of(d.start + 26 * 3600);
  d.step = (d.next - d.start) / p.today;
  d.tomorrow = p.tomorrow;
  d.third = p.tomorrow ? midnight_of(d.next + 26 * 3600) : d.next;
  d.next_step = p.tomorrow ? (d.third - d.next) / p.tomorrow : d.step;
  d.third_step = p.today >= 90 ? 900 : 3600;
  return d;
}

// The slots a graph shows, from `first` to before `stop`: all of them; with `ahead` the hours from the start of the hour
// of now on, that many, as far as the list goes, so the past is out of the graph; with `start` the 24 hours from that
// hour of the day, today's or yesterday's before it.
static void window_of(const Prices &p, const Day &d, uint32_t epoch, int ahead, int start, int &first, int &stop) {
  first = 0;
  stop = p.count();
  if (!stop) return;
  if (start >= 0) {
    if (!d.known() || !epoch) return;
    const uint32_t begin = window_start(epoch, start);
    if (begin < d.start) return;   // the prices have not turned round to yesterday yet: all of them until they do
    first = std::min(d.slot(begin), stop - 1);
    const uint32_t until = begin + 24 * 3600;
    int s = first;
    while (s < stop && d.at(s) < until) s++;
    stop = std::max(s, first + 1);
    return;
  }
  const int at_now = d.slot(epoch);
  if (ahead > 0 && at_now >= 0 && at_now < stop) {
    first = at_now;
    for (int i = 0; i < 3 && first > 0; i++) {
      const uint32_t from = first < d.today ? d.start : d.next;
      if ((d.at(first) - from) % 3600 == 0) break;
      first--;
    }
    const uint32_t until = d.at(first) + static_cast<uint32_t>(ahead) * 3600;
    int s = first;
    while (s < stop && d.at(s) < until) s++;
    stop = s;
  }
}

// With a window of hours: the lowest, highest and average known price in it, in place of today's, for the meter and
// the card.
static void range_of_window(Prices &p, const Day &d, uint32_t epoch, const Tariff &t) {
  if (t.ahead <= 0 && t.start < 0) return;
  int from = 0, to = 0;
  window_of(p, d, epoch, t.ahead, t.start, from, to);
  to = std::min(to, p.known());
  float lo = INFINITY, hi = -INFINITY, sum = 0;
  int n = 0;
  for (int s = from; s < to; s++) {
    if (std::isnan(p.slot[s])) continue;
    if (p.slot[s] < lo) { lo = p.slot[s]; p.lo_slot = s; }
    if (p.slot[s] > hi) { hi = p.slot[s]; p.hi_slot = s; }
    sum += p.slot[s];
    n++;
  }
  if (!n) return;
  p.lo = lo;
  p.hi = hi;
  p.avg = sum / n;
}

// Carnot's forecast after the known prices, for a graph of the hours ahead: each hour once, or as four quarters when
// the prices are in quarters. Energi Data Service starts it where its prices end (tomorrow, or the day after once
// tomorrow's are in), but on the last day of a month it also keeps today's hours: those are skipped, and what follows
// them starts after the known prices. Without a forecast, or for whole days, there is nothing after tomorrow.
static void place_forecast(Prices &p, const Day &d, const Tariff &tariff, bool cents) {
  p.extra = 0;
  if (tariff.ahead <= 0 || !d.known() || !p.today || !p.forecast_n || !p.forecast_start) return;
  const float shown = cents ? 100.0f : 1.0f;
  const int per = d.third_step == 900 ? 4 : 1;
  uint32_t from = p.forecast_start;
  int i = 0;
  if (from < d.third) {
    i = static_cast<int>(((from < d.next ? d.next : d.third) - from + 3599) / 3600);
    from = d.third;
  }
  int s = p.known();
  const bool feed = p.feed.size() >= MAX_SLOTS;
  for (uint32_t at = d.third; at < from && s < MAX_SLOTS; at += d.third_step, s++) {
    p.slot[s] = NAN;
    if (feed) p.feed[s] = NAN;
  }
  for (; i < p.forecast_n && s + per <= MAX_SLOTS; i++) {
    const float kwh = p.forecast[i];
    const float v = std::isnan(kwh) ? NAN : tariff.delivery(kwh) * shown;
    const float f = std::isnan(kwh) ? NAN : tariff.feed_in(kwh) * shown;
    for (int q = 0; q < per; q++, s++) {
      p.slot[s] = v;
      if (feed) p.feed[s] = f;
    }
  }
  p.extra = s - p.known();
}

// The cheapest hours to come: whole clock hours from the hour of now up to `end` (a slot), of known prices only, each
// hour the average of its slots. Either the `want` cheapest each on its own, in the order of the clock, or the
// cheapest stretch of `want` hours in a row. `start` holds the first slot of each, `per` the slots of an hour.
struct Cheap {
  int start[MAX_CHEAP] = {};
  int n = 0, per = 1;
};

static Cheap cheapest(const Prices &p, const Day &d, uint32_t epoch, int end, int want, bool block) {
  Cheap c;
  c.per = p.today >= 90 ? 4 : 1;
  const int now = d.slot(epoch);
  end = std::min(end, p.known());
  if (want <= 0 || now < 0 || now >= end) return c;
  const int from = now - now % c.per;  // a day has whole hours of slots, so an hour starts at a multiple of `per`
  const int hours = (end - from) / c.per;
  float avg[3 * 25];
  const int n = std::min(hours, 3 * 25);
  for (int h = 0; h < n; h++) {
    float sum = 0;
    avg[h] = 0;
    for (int q = 0; q < c.per; q++) {
      const float v = p.slot[from + h * c.per + q];
      if (std::isnan(v)) { avg[h] = NAN; break; }
      sum += v;
    }
    if (!std::isnan(avg[h])) avg[h] = sum / c.per;
  }
  if (block) {
    int best = -1;
    float least = INFINITY;
    for (int h = 0; h + want <= n; h++) {
      float sum = 0;
      bool whole = true;
      for (int k = h; k < h + want && whole; k++) {
        if (std::isnan(avg[k])) whole = false;
        else sum += avg[k];
      }
      if (whole && sum < least) { least = sum; best = h; }
    }
    if (best < 0) return c;
    for (int k = 0; k < want; k++) c.start[c.n++] = from + (best + k) * c.per;
    return c;
  }
  bool taken[3 * 25] = {};
  for (int k = 0; k < want; k++) {
    int best = -1;
    for (int h = 0; h < n; h++)
      if (!taken[h] && !std::isnan(avg[h]) && (best < 0 || avg[h] < avg[best])) best = h;
    if (best < 0) break;
    taken[best] = true;
  }
  for (int h = 0; h < n && c.n < MAX_CHEAP; h++)
    if (taken[h]) c.start[c.n++] = from + h * c.per;
  return c;
}

// ---- The graph, in the look of Tessera's history graph: price labels on the left, hairlines across, the bars, and
// clock times under them with a small mark each ----
constexpr int MAX_TICKS = 7;   // lines across, at most
constexpr int MAX_TIMES = 24;  // times under the graph, at most
// The lines across the graph at its price labels: none, solid or dotted.
enum class Grid : uint8_t { NONE, SOLID, DOTTED };

static Grid grid_of(const char *option) {
  return strcmp(option, "solid") == 0 ? Grid::SOLID : strcmp(option, "dotted") == 0 ? Grid::DOTTED : Grid::NONE;
}

// An object without a look of its own that `cb` draws: it takes no taps and no drags, so the tile's and the card's
// on_tap find what was tapped. One object stands for all the bars, lines and marks drawn in it, which is what keeps
// a graph's cost of the layout memory small.
static lv_obj_t *canvas(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb, void *owner) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, std::max(1, w), std::max(1, h));
  lv_obj_add_event_cb(obj, cb, LV_EVENT_DRAW_MAIN, owner);
  return obj;
}

// The owner of a canvas goes: the canvas, when the core has not deleted it yet, no longer asks it to draw.
static void drop_canvas(lv_obj_t *obj, lv_event_cb_t cb, void *owner) {
  if (obj && lv_obj_is_valid(obj)) lv_obj_remove_event_cb_with_user_data(obj, cb, owner);
}

// A filled rectangle at x, y of a canvas whose parent's corner is at ox, oy on the glass.
static void fill_rect(lv_layer_t *layer, int ox, int oy, int x, int y, int w, int h, lv_color_t color,
                      lv_opa_t opa = LV_OPA_COVER, int radius = 0) {
  if (w <= 0 || h <= 0) return;
  const lv_area_t box{ox + x, oy + y, ox + x + w - 1, oy + y + h - 1};
  const lv_area_t &clip = layer->_clip_area;
  if (box.x2 < clip.x1 || box.x1 > clip.x2 || box.y2 < clip.y1 || box.y1 > clip.y2) return;
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = color;
  dsc.bg_opa = opa;
  dsc.radius = radius;
  dsc.border_width = 0;
  lv_draw_rect(layer, &dsc, &box);
}

class Chart {
 public:
  ~Chart() { drop_canvas(plot_, paint_cb, this); }

  // The graph takes the rectangle x, y, w, h: its plot, the price labels left of it and the times under it.
  // `bars` is how many bars it may draw, `times` how many times it may write under the bars.
  // `line` draws a stepped line in place of the bars, with the bars as a faint fill under it when `fill`.
  void create(lv_obj_t *parent, int x, int y, int w, int h, int bars, int times, bool cents, bool line = false,
              bool fill = true, Grid grid = Grid::NONE) {
    cents_ = cents;
    grid_style_ = grid;
    line_ = line;
    fill_ = fill;
    bars_n_ = std::min(bars, CHART_SLOTS);
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
    thick_ = std::max(2, ui::px(big ? 3 : 2));
    dot_ = std::max(5, ui::px(big ? 9 : 7));
    value_.resize(bars_n_);
    base_.resize(bars_n_);
    // Everything but the words is drawn in one object: the lines across, the bars, the stepped line and its dots, the
    // marks of the cheapest hours and of the times. It covers the plot alone, not the words beside it: from a little
    // above it, where a mark over the highest bar and a dot on the line's top stand, down to the marks of the times.
    const int above = ui::px(big ? 6 : 3);
    plot_x_ = ax_;
    plot_y_ = ay_ - above;
    plot_ = canvas(parent, plot_x_, plot_y_, aw_, mark_y_ + mark_h_ - plot_y_, paint_cb, this);
    for (int i = 0; i < MAX_TICKS; i++) {
      price_[i] = ui::label(parent, Font::BODY, theme::SUBTLE);
      lv_obj_set_style_text_align(price_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < times_n_; i++) {
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
  // The feed-in price bar i stands on; 0 without one.
  float bar_feed(int i) const { return base_[i]; }
  // The first and the one past the last slot (of all the days) that bar i stands for.
  int bar_from(int i) const { return first_ + i * group_; }
  int bar_to(int i, int n) const { return std::min(n, first_ + (i + 1) * group_); }
  void select(int i) { sel_ = i; }
  // How many hours from now the graph shows (0: every slot of the list), or from which hour of the day 24 hours
  // (-1: none); `end()` is one past its last slot.
  void set_window(int ahead, int start) {
    ahead_ = ahead;
    start_ = start;
  }
  // The prices of the colours, when the tile sets them.
  void set_levels(const Levels &levels) { levels_ = levels; }
  // How many of the cheapest hours to come it marks (0: none), and whether in one row.
  void set_cheap(int hours, bool block) {
    cheap_want_ = hours;
    cheap_block_ = block;
  }
  // The cheapest hours it marked at its last update.
  const Cheap &cheap() const { return cheap_; }
  int end() const { return end_; }
  // Draw it again in the next pass: the look changed, so every colour is another.
  void repaint() {
    if (plot_) lv_obj_invalidate(plot_);
  }

  // The past is grey, the bar of now the accent, the bars to come green, amber or red by their place in the range of
  // the delivery price. With a feed-in price each bar stands on it instead of on zero: it runs from what a kWh brings
  // to what it costs. The graph is drawn again only when what it shows is another than before.
  void update(const Prices &p, const Day &d, uint32_t epoch) {
    layout(p, d, epoch);
    const uint32_t now = digest();
    if (now == digest_) return;
    digest_ = now;
    repaint();
  }

 private:
  // What the plot draws beside the bars' own prices: worked out in layout(), drawn in paint().
  struct Scene {
    float top, range, mn, mx;       // the plot's highest price and its span, the lowest and highest bar
    int16_t lines;                  // lines across
    int16_t grid_y[MAX_TICKS];
    int16_t gap, now_bar, known;    // between bars; the bar of now; the first slot that is a forecast
    int16_t has_feed;
    int16_t cheap_n;
    int16_t cheap_x[MAX_CHEAP], cheap_y[MAX_CHEAP], cheap_w[MAX_CHEAP], cheap_h;
    int16_t marks_n;
    int16_t mark_x[MAX_TIMES];
  };

  void layout(const Prices &p, const Day &d, uint32_t epoch) {
    Scene &sc = scene_;
    sc.lines = sc.cheap_n = sc.marks_n = 0;
    // The slots shown (window_of).
    int first = 0, stop = 0;
    const int at_now = d.slot(epoch);
    window_of(p, d, epoch, ahead_, start_, first, stop);
    first_ = first;
    end_ = stop;
    const int n = stop - first;
    const int now = at_now < 0 ? -1 : at_now - first;
    const int group = n > bars_n_ ? (n + bars_n_ - 1) / bars_n_ : 1;
    const int bars = n ? (n + group - 1) / group : 0;
    float *v = value_.data();
    float lo = 0, hi = 0, mn = INFINITY, mx = -INFINITY;
    for (int b = 0; b < bars; b++) {
      float sum = 0;
      int seen = 0;
      for (int s = b * group; s < std::min(n, (b + 1) * group); s++) {
        if (std::isnan(p.slot[first + s])) continue;
        sum += p.slot[first + s];
        seen++;
      }
      v[b] = seen ? sum / seen : NAN;
      base_[b] = 0;
      if (std::isnan(v[b])) continue;
      if (p.has_feed) {
        float feed = 0;
        int known = 0;
        for (int s = b * group; s < std::min(n, (b + 1) * group); s++) {
          if (std::isnan(p.feed[first + s])) continue;
          feed += p.feed[first + s];
          known++;
        }
        base_[b] = known ? feed / known : 0;
      }
      lo = std::min({lo, v[b], base_[b]});
      hi = std::max({hi, v[b], base_[b]});
      mn = std::min(mn, v[b]);
      mx = std::max(mx, v[b]);
    }
    mid_x_ = 0;
    group_ = group;
    bars_now_ = bars;
    if (sel_ >= bars) sel_ = -1;
    if (!bars) {
      for (int i = 0; i < MAX_TICKS; i++) lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
      for (int i = 0; i < times_n_; i++) lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
      cheap_ = Cheap{};
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
    sc.lines = static_cast<int16_t>(lines);
    for (int i = 0; i < MAX_TICKS; i++) {
      if (i >= lines) {
        lv_obj_add_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const float value = bottom + step * i;
      const int ty = ay_ + static_cast<int>(std::lround(ah_ * (top - value) / range));
      sc.grid_y[i] = static_cast<int16_t>(std::min(ty, ay_ + ah_ - 1));
      lv_obj_clear_flag(price_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(price_[i], label_w_ + 2, label_h_);
      lv_obj_set_pos(price_[i], x_, std::max(ay_ - label_h_ / 2, ty - label_h_ / 2));
      ui::set_text(price_[i], number(std::fabs(value) < step / 100 ? 0.0f : value, cents_ ? 0 : 2));
      ui::set_color(price_[i], theme::SUBTLE);
    }

    // The bars share the whole width of the plot, also when it does not divide by their number.
    const int slot = aw_ / bars;
    const int gap = line_ ? 0 : slot >= 6 ? 2 : (slot >= 3 ? 1 : 0);  // a fill is one area
    sc.top = top;
    sc.range = range;
    sc.mn = mn;
    sc.mx = mx;
    sc.gap = static_cast<int16_t>(gap);
    sc.now_bar = static_cast<int16_t>(now < 0 ? -1 : now / group);
    sc.known = static_cast<int16_t>(p.known());
    sc.has_feed = p.has_feed ? 1 : 0;

    // The cheapest hours to come: a short dark stroke over each hour's bars, so hours in a row make one line.
    cheap_ = cheapest(p, d, epoch, stop, cheap_want_, cheap_block_);
    const int mark_h = std::max(2, ui::px(ui::large() ? 4 : 3)), lift = ui::px(ui::large() ? 4 : 3);
    sc.cheap_h = static_cast<int16_t>(mark_h);
    for (int k = 0; k < cheap_.n; k++) {
      const int r = cheap_.start[k] - first;
      if (r < 0 || r >= n) continue;
      const int b0 = r / group, b1 = std::min(bars - 1, (r + cheap_.per - 1) / group);
      int top_y = ay_ + ah_;
      for (int b = b0; b <= b1; b++)
        if (!std::isnan(v[b])) top_y = std::min(top_y, y_of(std::max(v[b], base_[b])));
      const int x0 = ax_ + edge_x(b0), x1 = ax_ + edge_x(b1 + 1) - (cheap_block_ ? 0 : gap);
      sc.cheap_x[sc.cheap_n] = static_cast<int16_t>(x0);
      sc.cheap_w[sc.cheap_n] = static_cast<int16_t>(std::max(2, x1 - x0));
      sc.cheap_y[sc.cheap_n] = static_cast<int16_t>(std::max(ay_ - mark_h, top_y - lift - mark_h));
      sc.cheap_n++;
    }

    // The line between the days.
    if (p.tomorrow > 0 && p.today > first && p.today < stop) mid_x_ = ax_ + edge_x((p.today - first) / group);

    // The times under the bars: the clock hours that fit, a mark over each, as the history graph writes them. Each
    // stands where its slot begins, so a day of 23 or 25 hours has its hours where they are.
    for (int i = 0; i < times_n_; i++) lv_obj_add_flag(time_[i], LV_OBJ_FLAG_HIDDEN);
    if (!d.known()) return;
    const int hours = std::max(1, static_cast<int>((d.at(stop) - d.at(first) + 1800) / 3600));
    const int between = ui::px(ui::large() ? 10 : 6);
    const int sample = ui::text_width(tessera::clock_text(d.start + 8 * 3600), Font::BODY);
    const float per_hour = static_cast<float>(aw_) / hours;
    int every = 12;
    for (int h : {1, 2, 3, 4, 6, 12})
      if (hours / h <= times_n_ && (sample + between) <= h * per_hour) { every = h; break; }
    int last = ax_ - 1000, k = 0;
    for (int s = 0; s < n && k < times_n_; s++) {
      // Only a slot that begins on a whole hour, and of those the hours of the clock every `every`.
      const uint32_t at = d.at(first + s), from = first + s < d.today ? d.start : d.next;
      if ((at - from) % 3600 != 0) continue;
      const tessera::LocalTime t = tessera::local_time(at);
      if (t.minute != 0 || t.hour % every != 0) continue;
      const std::string words = tessera::clock_text(at);
      const int tw = ui::text_width(words, Font::BODY);
      const int cx = ax_ + static_cast<int>(static_cast<long long>(aw_) * s / n);
      const int left = std::max(ax_ - ui::px(ui::large() ? 8 : 4), cx - tw / 2);
      if (left <= last + between || left + tw > ax_ + aw_) continue;
      sc.mark_x[k] = static_cast<int16_t>(cx);
      lv_obj_clear_flag(time_[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(time_[k], tw + 2, label_h_);
      lv_obj_set_pos(time_[k], left, time_y_);
      ui::set_text(time_[k], words);
      ui::set_color(time_[k], theme::SUBTLE);
      last = left + tw;
      k++;
    }
    sc.marks_n = static_cast<int16_t>(k);
  }

  // Where bar i begins, from the plot's left edge, and where a price stands, in the parent's pixels.
  int edge_x(int i) const { return static_cast<int>(static_cast<long long>(aw_) * i / bars_now_); }
  int y_of(float value) const {
    return ay_ + static_cast<int>(std::lround(ah_ * (scene_.top - value) / scene_.range));
  }

  // A number that is another when what the plot draws is another.
  uint32_t digest() const {
    uint32_t h = 2166136261u;
    const auto mix = [&h](const void *data, size_t size) {
      const auto *bytes = static_cast<const uint8_t *>(data);
      for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 16777619u;
    };
    const int own[] = {bars_now_, group_, first_, sel_, mid_x_};
    mix(own, sizeof(own));
    if (bars_now_ <= 0) return h;
    const Scene &sc = scene_;
    const float prices[] = {sc.top, sc.range, sc.mn, sc.mx};
    mix(prices, sizeof(prices));
    const int16_t parts[] = {sc.lines, sc.gap, sc.now_bar, sc.known, sc.has_feed, sc.cheap_n, sc.cheap_h, sc.marks_n};
    mix(parts, sizeof(parts));
    mix(sc.grid_y, sc.lines * sizeof(int16_t));
    mix(sc.cheap_x, sc.cheap_n * sizeof(int16_t));
    mix(sc.cheap_y, sc.cheap_n * sizeof(int16_t));
    mix(sc.cheap_w, sc.cheap_n * sizeof(int16_t));
    mix(sc.mark_x, sc.marks_n * sizeof(int16_t));
    mix(value_.data(), bars_now_ * sizeof(float));
    mix(base_.data(), bars_now_ * sizeof(float));
    return h;
  }

  static void paint_cb(lv_event_t *e) {
    auto *chart = static_cast<Chart *>(lv_event_get_user_data(e));
    if (chart) chart->paint(lv_event_get_layer(e), static_cast<lv_obj_t *>(lv_event_get_current_target(e)));
  }

  // A bar's place in the colours: the past, now, and what is to come by its place in the range.
  int colour(int i) const {
    if (i < scene_.now_bar) return 0;
    if (i == scene_.now_bar) return 1;
    const Tone t = levels_.tone(value_[i], scene_.mn, scene_.mx);
    return t == Tone::GOOD ? 2 : t == Tone::BUSY ? 3 : 4;
  }

  // The plot, in the order its parts lie over each other: the lines across, the line between the days, the marks of
  // the cheapest hours, the bars, the stepped line with its dots, and the marks of the times.
  void paint(lv_layer_t *layer, lv_obj_t *obj) const {
    const int bars = bars_now_;
    if (bars <= 0) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int ox = a.x1 - plot_x_, oy = a.y1 - plot_y_;  // the parent's corner on the glass
    const Scene &sc = scene_;
    const float *v = value_.data();
    const auto rect = [&](int x, int y, int w, int h, lv_color_t color, lv_opa_t opa = LV_OPA_COVER, int radius = 0) {
      fill_rect(layer, ox, oy, x, y, w, h, color, opa, radius);
    };

    for (int i = 0; i < sc.lines && grid_style_ != Grid::NONE; i++) {
      if (grid_style_ == Grid::SOLID) {
        rect(ax_, sc.grid_y[i], aw_, 1, ui::color(theme::GRID));
        continue;
      }
      // A dotted line: dashes of a dot.
      lv_draw_line_dsc_t dsc;
      lv_draw_line_dsc_init(&dsc);
      dsc.color = ui::color(theme::TICK);
      dsc.width = 1;
      dsc.dash_width = std::max(1, ui::px(ui::large() ? 2 : 1));
      dsc.dash_gap = std::max(2, ui::px(ui::large() ? 4 : 3));
      dsc.p1.x = ox + ax_;
      dsc.p1.y = oy + sc.grid_y[i];
      dsc.p2.x = ox + ax_ + aw_;
      dsc.p2.y = oy + sc.grid_y[i];
      lv_draw_line(layer, &dsc);
    }
    if (mid_x_ > 0) rect(mid_x_, ay_, 1, ah_, ui::color(theme::TICK));
    for (int k = 0; k < sc.cheap_n; k++)
      rect(sc.cheap_x[k], sc.cheap_y[k], sc.cheap_w[k], sc.cheap_h, ui::color(theme::INK), LV_OPA_COVER, ui::px(1));

    // The bars, from the line of zero or from the feed-in price. Under a stepped line they are its fill: faint, the
    // past a little less so as its grey is light already. Carnot's forecast, after the known prices, is a guess: its
    // bars are fainter than the prices before them.
    const lv_color_t tones[] = {ui::color(theme::TRACK), ui::color(theme::ACCENT),
                                ui::tone_color(Tone::GOOD, theme::TRACK), ui::tone_color(Tone::BUSY, theme::TRACK),
                                ui::tone_color(Tone::ALERT, theme::TRACK)};
    for (int i = 0; i < bars && !(line_ && !fill_); i++) {
      if (std::isnan(v[i])) continue;
      const int high = y_of(std::max(v[i], base_[i])), low = y_of(std::min(v[i], base_[i]));
      const int len = std::max(2, low - high);
      const bool guess = first_ + i * group_ >= sc.known;
      const lv_opa_t opa = line_ ? (i < sc.now_bar ? LV_OPA_50 : guess ? LV_OPA_10 : LV_OPA_20)
                                 : (guess ? LV_OPA_40 : LV_OPA_COVER);
      rect(ax_ + edge_x(i), v[i] >= base_[i] ? low - len : high, std::max(1, edge_x(i + 1) - edge_x(i) - sc.gap), len,
           i == sel_ && !line_ ? ui::color(theme::INK) : tones[colour(i)], static_cast<lv_opa_t>(opa),
           line_ ? 0 : ui::px(1));
    }

    if (line_) {
      // The stepped line: each bar's price a flat step across its slot, and a rise or drop to the next, in the colour
      // of the bar it leaves. The feed-in price, when there is one, is a thin line under it. A gap in the prices
      // breaks the line.
      const lv_color_t past = ui::color(theme::SUBTLE);
      const auto steps = [&](int thick, bool feed) {
        const int half = thick / 2;
        for (int i = 0; i < bars; i++) {
          if (std::isnan(v[i])) continue;
          const lv_color_t color = feed ? ui::color(theme::MUTED) : colour(i) == 0 ? past : tones[colour(i)];
          const int y = y_of(feed ? base_[i] : v[i]);
          rect(ax_ + edge_x(i), y - half, edge_x(i + 1) - edge_x(i), thick, color);
          if (i + 1 >= bars || std::isnan(v[i + 1])) continue;
          const int next = y_of(feed ? base_[i + 1] : v[i + 1]);
          rect(ax_ + edge_x(i + 1) - half, std::min(y, next) - half, thick, std::abs(next - y) + thick, color);
        }
      };
      if (sc.has_feed) steps(std::max(1, thick_ / 2), true);
      steps(thick_, false);
      const auto middle = [&](int b) { return ax_ + (edge_x(b) + edge_x(b + 1)) / 2; };
      const auto dot = [&](int b, float value, lv_color_t color) {
        const int x = std::clamp(middle(b) - dot_ / 2, ax_, std::max(ax_, ax_ + aw_ - dot_));
        rect(x, y_of(value) - dot_ / 2, dot_, dot_, color, LV_OPA_COVER, LV_RADIUS_CIRCLE);
      };
      if (sc.now_bar >= 0 && sc.now_bar < bars && !std::isnan(v[sc.now_bar]) && sc.now_bar != sel_)
        dot(sc.now_bar, v[sc.now_bar], ui::color(theme::ACCENT));
      // A tapped slot: a line down through it, and a dot on each price.
      if (sel_ >= 0 && sel_ < bars && !std::isnan(v[sel_])) {
        rect(middle(sel_), ay_, 1, ah_, ui::color(theme::INK));
        dot(sel_, v[sel_], ui::color(theme::INK));
        if (sc.has_feed) dot(sel_, base_[sel_], ui::color(theme::MUTED));
      }
    }

    for (int k = 0; k < sc.marks_n; k++) rect(sc.mark_x[k], mark_y_, 1, mark_h_, ui::color(theme::TICK));
  }

  lv_obj_t *plot_{}, *price_[MAX_TICKS]{}, *time_[MAX_TIMES]{};
  Scene scene_{};
  uint32_t digest_ = 0;
  bool line_ = false, fill_ = true;
  Grid grid_style_ = Grid::NONE;
  bool cents_ = false;
  int x_ = 0, ax_ = 0, ay_ = 0, aw_ = 1, ah_ = 1, label_w_ = 0, label_h_ = 0, time_y_ = 0, mark_y_ = 0, mark_h_ = 0;
  int plot_x_ = 0, plot_y_ = 0, thick_ = 2, dot_ = 5;
  int bars_n_ = 0, times_n_ = 0, mid_x_ = 0;
  int group_ = 1, bars_now_ = 0, sel_ = -1;
  int first_ = 0, end_ = 0, ahead_ = 0;  // the window of slots the graph shows; ahead_ in hours, 0 for all
  int start_ = -1;                        // or 24 hours from this hour of the day
  Levels levels_;
  int cheap_want_ = 0;                    // the cheapest hours to mark, and whether in one row
  bool cheap_block_ = false;
  Cheap cheap_;
  tessera::List<float> value_, base_;  // each bar's delivery price, and the price it stands on
};

// What a card takes over from the tile that opened it: the tile's options and its last prices.
struct Shared : tessera::InPsram {
  bool cents = false;
  bool line = false, fill = true;  // the graph: a stepped line in place of bars, with a fill under it
  Grid grid = Grid::NONE;
  int decimals = 3;
  Tariff tariff;
  Prices prices;
};

// What the top bar shows: the prices of one tile on the screen, with its formula, unit and decimals. The first tile
// that has a sensor gives them, and keeps giving them while it lives; the prices stay when it goes (the CYD makes a
// new tile when its page comes back), until the next tile gives its own.
struct BarSource : tessera::InPsram {
  const void *tile = nullptr;  // the tile that gives them; nullptr: none now
  bool set = false;            // a tile gave prices
  bool cents = false, error = false;
  int decimals = 3;
  Prices prices;
};

class PriceTile : public tessera::Tile {
 public:
  PriceTile(const tessera::Plugin *plugin, Shared *shared, BarSource *bar) : plugin_(plugin), shared_(shared), bar_(bar) {}
  ~PriceTile() override {
    if (bar_->tile == this) bar_->tile = nullptr;
    drop_canvas(meter_, meter_cb, this);
  }

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    entity_ = c.entity;
    cents_ = strcmp(c.options["unit"] | "eur", "cents") == 0;
    decimals_ = std::min<int>(static_cast<int>(std::lround(c.options["fixed"] | 3.0f)), cents_ ? 1 : 4);  // 3 or 3.0
    tariff_ = tariff_of(c.options);
    line_ = strcmp(c.options["graph"] | "bars", "line") == 0;
    fill_ = c.options["fill"] | true;
    grid_ = grid_of(c.options["grid"] | "none");
    head_ = ui::line_height(Font::HEADLINE);
    const int gap = ui::px(6);
    const int axis = ui::line_height(Font::BODY) + ui::px(ui::large() ? 8 : 4);
    chart_on_ = c.columns >= 2 && c.rows >= 2 && height_ - head_ - gap >= axis + ui::px(36);
    value_ = ui::label(c.parent, Font::VALUE, theme::INK);
    unit_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    name_text_ = c.name && (c.options["show_name"] | true) ? c.name : "";
    name_ = ui::label(c.parent, Font::TITLE, theme::MUTED);
    feed_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(feed_, LV_TEXT_ALIGN_RIGHT, 0);
    if (chart_on_) {
      chart_.create(c.parent, 0, head_ + gap, width_, height_ - head_ - gap, TILE_BARS, 12, cents_, line_, fill_, grid_);
      chart_.set_window(tariff_.ahead, tariff_.start);
      chart_.set_levels(tariff_.levels);
      chart_.set_cheap(tariff_.cheap, tariff_.cheap_block);
      return;
    }
    // The meter's bar of cells (Tessera's sensor meter): narrow cells along the bottom of the tile, lit up to where
    // the price stands between the day's lowest and highest.
    cells_h_ = std::clamp(height_ * 20 / 100, ui::px(10), ui::px(26));
    const int cell = std::clamp(cells_h_ / 12, std::max(2, ui::px(3)), std::max(2, ui::px(5)));
    cell_gap_ = std::max(1, cell * 3 / 5);
    cells_n_ = std::clamp((width_ + cell_gap_) / (cell + cell_gap_), 10, MAX_CELLS);
    meter_ = canvas(c.parent, 0, height_ - cells_h_, width_, cells_h_, meter_cb, this);
  }

  void on_state(JsonObjectConst data) override {
    read_prices(data, cents_, tariff_, prices_);
    if (prices_.chosen && (bar_->tile == nullptr || bar_->tile == this)) {
      bar_->tile = this;
      bar_->set = true;
      bar_->cents = cents_;
      bar_->error = tariff_.error;
      bar_->decimals = decimals_;
      bar_->prices = prices_;
    }
  }

  void on_tick(uint32_t epoch) override {
    place_forecast(prices_, day_of(prices_, epoch), tariff_, cents_);
    range_of_window(prices_, day_of(prices_, epoch), epoch, tariff_);
    price_now(prices_, epoch);
    std::string big, small;
    if (!prices_.chosen) {
      big = plugin_->text("choose");
    } else if (tariff_.error) {
      big = plugin_->text("formula_error");
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
      // With a feed-in price, the bar's own after its time: "18:00–18:15 · Feed-in 0.250", where it fits beside
      // the price (at most as large as the headline face the row takes).
      if (prices_.has_feed) {
        const std::string with = name + " \u00b7 " +
                                 tessera::fill(plugin_->text("feed_in"), "value", number(chart_.bar_feed(sel_), decimals_));
        const int room = width_ - ui::text_width(small.empty() ? big : big + " " + small, Font::HEADLINE) - ui::px(8);
        if (ui::text_width(with, Font::BODY) <= room) name = with;
      }
    }
    // A message ("Choose a price sensor") that does not fit beside the name takes the whole row.
    if (!ok && ui::text_width(big, Font::BODY_LARGE) > width_ * 60 / 100) name.clear();
    if (chart_on_) {
      // The name at the left and the price with its unit at the right, over the graph.
      place_row(big, small, name, head_);
      ui::set_text(feed_, "");
      chart_.update(ok ? prices_ : NO_PRICES, day, epoch);
    } else {
      // The meter's short card: the name at the left and the price with its unit at the right, in one row, the
      // cells under them. The feed-in price, when there is one, has a line of its own under the price where the
      // tile has room for it.
      int room = height_ - cells_h_ - ui::px(6);
      const int fh = ui::line_height(Font::BODY);
      const bool feed = ok && prices_.has_feed && room - fh >= ui::line_height(Font::BODY_LARGE);
      if (feed) room -= fh;
      place_row(big, small, name, room);
      ui::set_text(feed_, feed ? feed_text(prices_.feed_current, width_) : "");
      lv_obj_set_size(feed_, std::max(1, width_ - 2), fh);
      lv_obj_set_pos(feed_, 0, room);
      ui::set_color(feed_, theme::MUTED);
      // The cells are lit up to where the price stands in the day's range, and drawn again only when that changes.
      const bool known = ok && prices_.today > 0 && prices_.hi > prices_.lo;
      const float share = known ? std::clamp((prices_.current - prices_.lo) / (prices_.hi - prices_.lo), 0.0f, 1.0f) : 0.0f;
      const int lit = known ? static_cast<int>(std::lround(share * cells_n_)) : -1;
      if (lit != lit_) {
        lit_ = lit;
        lv_obj_invalidate(meter_);
      }
    }
    last_epoch_ = epoch;
  }

  void on_theme() override {
    ui::set_color(value_, theme::INK);
    ui::set_color(unit_, theme::MUTED);
    on_tick(last_epoch_);
    chart_.repaint();
    if (meter_) lv_obj_invalidate(meter_);
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
    shared_->tariff = tariff_;
    shared_->line = line_;
    shared_->fill = fill_;
    shared_->grid = grid_;
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

  // "Feed-in 0.120 €/kWh"; without the unit when that does not fit in `room`.
  std::string feed_text(float value, int room) const {
    const std::string n = number(value, decimals_), unit = unit_text(prices_, cents_);
    const std::string both = tessera::fill(plugin_->text("feed_in"), "value", unit.empty() ? n : n + " " + unit);
    return ui::text_width(both, Font::BODY) <= room ? both : tessera::fill(plugin_->text("feed_in"), "value", n);
  }

  static void meter_cb(lv_event_t *e) {
    auto *tile = static_cast<PriceTile *>(lv_event_get_user_data(e));
    if (tile) tile->paint_meter(lv_event_get_layer(e), static_cast<lv_obj_t *>(lv_event_get_current_target(e)));
  }

  // The meter's cells: each wears the colour of its place in the range, as the graph's bars do; the cells past the
  // price are a dim tint of theirs. Without a range (lit_ < 0) they are all the track's grey.
  void paint_meter(lv_layer_t *layer, lv_obj_t *obj) const {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    for (int i = 0; i < cells_n_; i++) {
      const int from = static_cast<int>(static_cast<long>(width_) * i / cells_n_);
      const int to = static_cast<int>(static_cast<long>(width_) * (i + 1) / cells_n_) - cell_gap_;
      const float at = (i + 0.5f) / cells_n_;
      if (lit_ < 0) {
        fill_rect(layer, a.x1, a.y1, from, 0, std::max(1, to - from), cells_h_, ui::color(theme::TRACK));
        continue;
      }
      // The cell's place in the range as a price, for prices of the tile's own.
      const float price = prices_.lo + at * (prices_.hi - prices_.lo);
      fill_rect(layer, a.x1, a.y1, from, 0, std::max(1, to - from), cells_h_,
                ui::tone_color(tariff_.levels.tone(price, prices_.lo, prices_.hi), theme::TRACK),
                i < lit_ ? LV_OPA_COVER : LV_OPA_30);
    }
  }

  const tessera::Plugin *plugin_;
  Shared *shared_;
  BarSource *bar_;
  int sel_ = -1;
  uint32_t sel_until_ = 0;
  std::string entity_, name_text_;
  Prices prices_;
  Chart chart_;
  Tariff tariff_;
  bool cents_ = false, chart_on_ = false, line_ = false, fill_ = true;
  Grid grid_ = Grid::NONE;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, tile_ = -1;
  int cells_n_ = 0, cells_h_ = 0, cell_gap_ = 1;
  int lit_ = -2;  // the meter's lit cells; -1: no range, all grey; -2: not drawn yet
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *name_{}, *feed_{}, *meter_{};
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
    tariff_ = shared_->tariff;
    const int gap = ui::px(8);
    const int text_h = ui::line_height(Font::BODY_LARGE);
    row_h_ = std::max(ui::line_height(Font::VALUE), 2 * text_h);
    // The feed-in price of now, when the tariff has one: a line under the header's row, at the left under the price.
    head_ = tariff_.feed ? row_h_ + text_h : row_h_;
    feed_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(feed_, width_, text_h);
    lv_obj_set_pos(feed_, 0, row_h_);
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
    lv_obj_set_pos(avg_cap_, left_w_, (row_h_ - 2 * text_h) / 2);
    avg_ = ui::label(c.parent, Font::BODY_LARGE, theme::INK);
    lv_obj_set_size(avg_, right_x_ - left_w_, text_h);
    lv_obj_set_style_text_align(avg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(avg_, left_w_, (row_h_ - 2 * text_h) / 2 + text_h);
    for (int i = 0; i < 2; i++) {
      extreme_[i] = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
      lv_obj_set_size(extreme_[i], width_ - right_x_, text_h);
      lv_obj_set_style_text_align(extreme_[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(extreme_[i], right_x_, (row_h_ - 2 * text_h) / 2 + i * text_h);
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
    chart_.create(c.parent, edge, plot_y, width_ - 2 * edge, y + card_h - ui::px(big ? 10 : 6) - plot_y, CHART_SLOTS, MAX_TIMES,
                  cents_, shared_->line, shared_->fill, shared_->grid);
    chart_.set_window(tariff_.ahead, tariff_.start);
    chart_.set_levels(tariff_.levels);
    chart_.set_cheap(tariff_.cheap, tariff_.cheap_block);
    cheap_ = ui::label(c.parent, Font::BODY, theme::INK);
    lv_obj_set_style_text_align(cheap_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(cheap_, 1, days_h);
    lv_obj_set_pos(cheap_, edge, top);
    edge_ = edge;
    on_tick(0);
  }

  void on_state(JsonObjectConst data) override {
    read_prices(data, cents_, tariff_, prices_);
    on_tick(last_epoch_);
  }

  void on_tick(uint32_t epoch) override {
    last_epoch_ = epoch;
    place_forecast(prices_, day_of(prices_, epoch), tariff_, cents_);
    range_of_window(prices_, day_of(prices_, epoch), epoch, tariff_);
    price_now(prices_, epoch);
    const bool ok = prices_.chosen && prices_.has_current;
    const int gap = ui::px(8);
    // A tapped bar shows its own price and time slot for a few seconds, then the card is as it was.
    if (sel_ >= 0 && (!ok || prices_.today == 0 || epoch >= sel_until_)) { sel_ = -1; chart_.select(-1); }
    std::string big = ok                  ? number(prices_.current, decimals_)
                      : !prices_.chosen   ? plugin_->text("choose")
                      : tariff_.error     ? plugin_->text("formula_error")
                                          : plugin_->text("unavailable");
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
    lv_obj_set_pos(value_, 0, (row_h_ - vh) / 2);
    lv_obj_set_size(unit_, std::max(1, left_w_ - vw - gap), sh);
    lv_obj_set_pos(unit_, vw + gap, (row_h_ - vh) / 2 + vh - sh - ui::px(2));
    const bool graph = ok && prices_.today > 0;
    const Day day = day_of(prices_, epoch);
    // The feed-in price of now under the price; a tapped bar's own, with its time slot behind it, while it shows.
    std::string feed;
    if (ok && prices_.has_feed) {
      const bool tapped = graph && sel_ >= 0;
      feed = tessera::fill(plugin_->text("feed_in"), "value",
                           with_unit(tapped ? chart_.bar_feed(sel_) : prices_.feed_current, width_));
      if (tapped && day.known()) {
        const std::string with = feed + " \u00b7 " + tessera::clock_text(day.at(chart_.bar_from(sel_))) +
                                 (ui::covers("\u2013", Font::BODY_LARGE) ? "\u2013" : "-") +
                                 tessera::clock_text(day.at(chart_.bar_to(sel_, prices_.count())));
        if (ui::text_width(with, Font::BODY_LARGE) <= width_) feed = with;
      }
    }
    ui::set_text(feed_, feed);
    ui::set_color(feed_, theme::MUTED);
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
    chart_.update(graph ? prices_ : NO_PRICES, day, epoch);
    ui::set_fill(surface_, Tone::NORMAL, theme::CARD);
    lv_obj_set_style_border_color(surface_, ui::color(theme::LINE), 0);
    ui::set_text(today_, plugin_->text(first_day(graph)));
    ui::set_color(today_, theme::SUBTLE);
    const bool second = graph && prices_.tomorrow > 0 && chart_.midnight_x() > 0;
    ui::set_text(tomorrow_, second ? plugin_->text(second_day()) : "");
    ui::set_color(tomorrow_, theme::SUBTLE);
    if (second) {
      const int tx = chart_.midnight_x() + ui::px(ui::large() ? 8 : 4);
      lv_obj_set_pos(tomorrow_, tx, lv_obj_get_y(today_));
      lv_obj_set_width(tomorrow_, std::max(1, width_ - tx));
    }
    show_cheap(graph, second, day);
  }

  // The cheapest hours in the day row, at its right end: "Cheapest 02:00 · 03:00 · 13:00", or "Cheapest 02:00–05:00"
  // for hours in a row; nothing where it would run into the day labels.
  void show_cheap(bool graph, bool second, const Day &day) {
    const Cheap &c = chart_.cheap();
    std::string words;
    if (graph && day.known() && c.n > 0) {
      const std::string dash = ui::covers("\u2013", Font::BODY) ? "\u2013" : "-";
      std::string list;
      if (tariff_.cheap_block) {
        list = tessera::clock_text(day.at(c.start[0])) + dash + tessera::clock_text(day.at(c.start[c.n - 1] + c.per));
      } else {
        for (int k = 0; k < c.n; k++) list += (k ? " \u00b7 " : "") + tessera::clock_text(day.at(c.start[k]));
      }
      words = tessera::fill(plugin_->text("cheap"), "value", list);
    }
    const int y = lv_obj_get_y(today_);
    const int left = second ? lv_obj_get_x(tomorrow_) + ui::text_width(plugin_->text(second_day()), Font::BODY)
                            : edge_ + ui::text_width(plugin_->text(first_day(graph)), Font::BODY);
    const int room = width_ - edge_ - left - ui::px(8);
    const int w = words.empty() ? 0 : ui::text_width(words, Font::BODY);
    if (w == 0 || w > room) words.clear();
    ui::set_text(cheap_, words);
    ui::set_color(cheap_, theme::INK);
    lv_obj_set_size(cheap_, std::max(1, w + 2), ui::line_height(Font::BODY));
    lv_obj_set_pos(cheap_, width_ - edge_ - w - 2, y);
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
    ui::set_color(feed_, theme::MUTED);
    ui::set_color(avg_cap_, theme::MUTED);
    ui::set_color(avg_, theme::INK);
    ui::set_color(extreme_[0], theme::MUTED);
    ui::set_color(extreme_[1], theme::MUTED);
    on_tick(last_epoch_);
    chart_.repaint();
  }

 private:
  // The words over the two days of the graph: today and tomorrow, or yesterday and today before the start hour.
  const char *first_day(bool graph) const { return !graph ? "no_graph" : prices_.back ? "yesterday" : "today"; }
  const char *second_day() const { return prices_.back ? "today" : "tomorrow"; }

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
    // With a window of hours ahead, only the slots the graph shows.
    // Only known prices: the forecast is a guess, and its slots are not today's or tomorrow's.
    const int end = (tariff_.ahead > 0 || tariff_.start >= 0) && chart_.end() > 0 ? std::min(prices_.known(), chart_.end())
                                                                                   : prices_.known();
    for (int s = std::max(now, 0); s < end; s++) {
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
    const std::string when =
        slot >= prices_.today && !prices_.back ? std::string(plugin_->text("tomorrow")) + " " : std::string();
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
  Tariff tariff_;
  bool cents_ = false;
  int sel_ = -1;
  uint32_t sel_until_ = 0;
  int decimals_ = 3, width_ = 0, height_ = 0, head_ = 0, row_h_ = 0, left_w_ = 0, right_x_ = 0;
  uint32_t last_epoch_ = 0;
  lv_obj_t *value_{}, *unit_{}, *feed_{}, *avg_cap_{}, *avg_{}, *extreme_[2]{}, *today_{}, *tomorrow_{}, *surface_{};
  lv_obj_t *cheap_{};
  int edge_ = 0;
};

void ElectricityPrice::setup() {
  shared_ = new Shared();
  bar_ = new BarSource();
  // A tile per integration: the inspector lists only sensors that have the attributes of that one.
  add_tile("nordpool", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("entsoe", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("gespot", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("epex", [this]() { return new PriceTile(this, shared_, bar_); });
  add_tile("energidataservice", [this]() { return new PriceTile(this, shared_, bar_); });
  add_card("prices", [this]() { return new PriceCard(this, shared_); }, true);
  add_bar_item("price", [this]() { return bar(); });
}

// The price now in the top bar, as a tile shows it: the prices of the tile that gives them (BarSource), its formula,
// unit and decimals, at the slot of now. Nothing shows until a tile has a price; a formula with a typo says so.
tessera::BarItem ElectricityPrice::bar() {
  tessera::BarItem item;
  if (!bar_->set) return item;
  if (bar_->error) {
    item.shown = true;
    item.icon = 0xF0241;  // flash
    item.text = text("formula_error");
    return item;
  }
  Prices &p = bar_->prices;
  price_now(p, tessera::epoch());
  if (!p.has_current) return item;
  item.shown = true;
  item.icon = 0xF0241;
  item.text = number(p.current, bar_->decimals) + " " + unit_text(p, bar_->cents);
  return item;
}

}  // namespace esphome::electricity_price
