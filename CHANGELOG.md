# Changelog

## 0.12.0
- EPEX Spot: a fourth tile, **Electricity price (EPEX Spot)**, for the sensor of the HACS `epex_spot` integration. Its
  one list of yesterday, today and tomorrow is cut into today and tomorrow by the start of its slots. From
  Nekromonger's [tessera-strompreis](https://github.com/Nekromonger/tessera-strompreis).
- **Next 24 hours only**, on every tile: the graph shows the 24 hours from the slot of now instead of today and
  tomorrow, up to midnight while tomorrow's prices are not in yet. The meter's range and the card's average, lowest
  and highest are of those 24 hours too. The card follows the tile.
- German, from Nekromonger's translation.

## 0.11.0
- The top bar shows the price as your tiles do: the sensor, formula, unit and decimals of a price tile on the screen,
  and the price of the current time slot. The first tile with a sensor gives it. Its own sensor and its two settings
  (Price sensor for the top bar, Top bar: price formula, Top bar: unit) are gone, and so is the permission to read
  that sensor. A screen without a price tile shows no price in the top bar.

## 0.10.0
- The price now in the top bar: place **Electricity price** on a page's top bar in the editor, under "From plugins".
  It reads the price sensor chosen when the plugin is added (Price sensor for the top bar), through a formula of its
  own and in EUR/kWh or ct/kWh: two settings in the plugin's details on the screen's Plugins tab, which change
  without a build. A formula with a typo says "Check the formula".
- The price of now comes from today's list at the current time slot, the same as the bar of now. The ENTSO-e tile
  showed the day's average as the price of now: ENTSO-e keeps its lists only on its average-price sensor, whose
  state is that average (issue #2). Without a list or a set clock, the sensor's state stays the price.

## 0.9.0
- Graph: bars or a stepped line, chosen per tile in the editor. The stepped line follows the price slot by slot in the
  colours of the bars, with a dot for now; a feed-in price is a thin grey line under it. A tapped slot gets a line
  down through it and a dot on each price.
- Fill under the line (on by default): a faint fill down to zero, or to the feed-in price.
- Grid lines (none by default): solid or dotted lines across the graph at the price labels it shows. The lines
  that were there before were a pixel wide by mistake, so none showed.
- The card shows the graph of the tile you open it from.

## 0.8.1
- A tapped bar shows its feed-in price too: after its time on a tile, where it fits, and on the card in the feed-in
  line, with its time slot.

## 0.8.0
- The tariff is a formula you type, one for delivery and one for feed-in: `(p+0.12)*1.21`, `p-0.02`, or a number
  alone for a fixed price. p is the sensor's price per kWh; + - * /, brackets and a decimal point or comma work.
  A formula with a typo shows "Check the formula" in place of the price.
- The a, b and c options of 0.7.0 are gone: type your tariff again as a formula.

## 0.7.0
- Your own tariff on top of the market price: the delivery price is (p + a) × b + c, with a for the energy tax and the
  supplier's surcharge, b for the VAT and c added after it. b 0 and c your price make a fixed price.
- An optional feed-in price, (p + a) × b + c with its own a, b and c. With it each bar of the graph runs from the
  feed-in price up to the delivery price; the colours stay those of the delivery price.
- The feed-in price of now under the price on the meter tile, where it has room, and under the price on the card.

## 0.6.6
- A sensor that reports per MWh shows its price per kWh, in EUR/kWh and in ct/kWh alike. Before, ct/kWh multiplied a
  price per MWh by 100.
- On the days the clock changes (23 or 25 hours), the bar of now, the times under the graph and the time slots of a
  tapped bar and of the highest and lowest price are right. Before, they were an hour off after the change.
- When tomorrow's prices come in another resolution than today's (hours beside quarters), the graph shows them in
  today's, without gaps.
- "Choose a price sensor" and "No price" fit on a small tile and on the card, in every language: without a price
  the message takes the whole row, in a smaller font where needed. Before, it was cut off.

## 0.6.5
- The tiles also take the whole page (`full: true`): a tile as big as the grid, such as 5x5 on a 5x5 grid, was not offered before.
- The editor shows the price with its unit.

## 0.6.4
- The tiles come in every size, from 1x1 up to 9x9 (before, at most 4x3).

## 0.6.3
- Graph: the bars share the whole width of the plot. Before, each bar had a whole number of pixels, so on a screen where 192 quarters did not divide the width the bars took only part of it.

## 0.6.2
- Card: the highest and the lowest price only count from now on, tomorrow included once it is known. A time slot of tomorrow says "Tomorrow" in front.

## 0.6.1
- The card is as wide as the glass, like Tessera's own graph.

## 0.6.0
- Card: the unit behind the average, the highest and the lowest. A tapped bar shows its price under its time slot in the middle, and the price of now stays at the left.

## 0.5.4
- Store picture (store.png) made on the bench.
- Fixes found by the first real build: the GE-Spot tile is registered, no `strlcpy`, the Tomorrow label stays inside the card.
- The name keeps the tile-label font, the price takes what is left.

## 0.5.3
- The graph shows from 2 wide and 2 high; smaller tiles show the price with the meter bar.

## 0.5.2
- Tile with a graph: the name at the left and the price of now at the right, like the small tile. A tapped bar shows its time in place of the name.

## 0.5.1
- Small tile like the meter bar tile: the name at the left, the price with its unit at the right.

## 0.5.0
- GE-Spot as a source: a third tile reads `today_interval_prices` and `tomorrow_interval_prices` (the `value` of each interval).

## 0.4.1
- Tapping a bar works on the full screen card too (needs API 0.12). High, low and average use the font of the history card.

## 0.4.0
- Tap a bar of the graph on a tile to see its price and time slot for a few seconds. Tap it again, or beside the bars, to open the card.

## 0.3.1
- Card header like the history card: price at the left, average in the middle, highest and lowest at the right with their time slot.

## 0.3.0
- Choose the unit: EUR/kWh or ct/kWh.
- Time axis and price axis in the look of Tessera's history graph; the small tile is the meter's bar of cells.

## 0.2.0
- The graph shows quarter-hour prices when the sensor has them.
- One tile per integration (Nord Pool, ENTSO-e), so the inspector lists only compatible sensors.

## 0.1.0
- First version: a tile with the electricity price now, a graph of today and tomorrow on bigger tiles and on a card.
  Works with the Nord Pool and ENTSO-e sensors.
