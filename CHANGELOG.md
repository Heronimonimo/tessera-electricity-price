# Changelog

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
