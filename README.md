# Electricity price

The electricity price now as a tile, with the graph of today and tomorrow on bigger tiles and on a card of its own.
It reads the price sensor of **Nord Pool** (HACS `nordpool`), **ENTSO-e** (`entsoe`) or **GE-Spot** (`ge_spot`) in Home Assistant. For GE-Spot, use its price sensor in a currency per kWh (not the cents display).

It runs on every board Tessera supports (`boards: any`), from the CYD to the 10.1-inch screens.

## Set up

1. In Tessera, open **Plugins** and choose **Electricity price**, or choose **Add with a link** and paste
   `https://github.com/Heronimonimo/tessera-electricity-price`. Tick your screens; each builds once.
2. In Layout, place the **Electricity price (Nord Pool)**, **Electricity price (ENTSO-e)** or **Electricity price
   (GE-Spot)** tile from the library's Plugins group, at any size from 1x1 up to the whole page. A tile smaller than
   2x2 shows the price with the meter bar; from 2x2 up it adds the graph.
3. In the inspector, choose your price sensor. The list holds only the sensors that fit the tile you placed.
4. Choose the unit, **EUR/kWh** (`0.273`) or **ct/kWh** (`27.3`), and the number of decimals (in ct/kWh at most one).
5. Optionally set your tariff (below).
6. Tap a bar of the graph to see its price and time slot for a few seconds, on a bigger tile and on the full screen card. On a tile, tap that bar again, or beside the bars, to open the card: the price now, the lowest and highest still to come, the day's average, and the graph.

## Your tariff

The sensor gives the market price p. In the inspector you can type what you really pay, and what you get back, as a
formula in p:

| Field | Example | Means |
|---|---|---|
| **Delivery price** | `(p+0.12)*1.21` | Energy tax and surcharge on top, then 21% VAT |
| | `0.25` | A fixed price |
| **Feed-in price** | `p-0.02` | The market price minus your supplier's 2 cents |
| | `0,07` | A fixed feed-in price |

A formula has p, numbers (`0.12` or `0,12`), `+ - * /` and brackets. The numbers are in the sensor's currency per
kWh (0.02, not 2), also when the tile shows cents. An empty delivery price is the sensor's own price; an empty feed-in
price is none. A formula with a typo shows "Check the formula" on the screen in place of the price; the editor
cannot check it for you.

With a feed-in price each bar runs from the feed-in price to the delivery price, so you see the gap between the two
over the day, also when feed-in is the higher one. The tile and the card show the feed-in price of now under the
price. The editor's preview shows the sensor's own price: it cannot work out the formula.

## Bars or a stepped line

In the inspector, **Graph** chooses **Bars** or **Stepped line**. The stepped line follows the price slot by slot in
the same colours as the bars, with a dot for now. With a feed-in price it is a thin grey line under the delivery
price. **Fill under the line** adds a faint fill down to zero, or to the feed-in price. The card shows the graph of
the tile you open it from. **Grid lines** draws solid or dotted lines across the graph at its price labels.

## The price in the top bar

Place **Electricity price** on a page's top bar in the editor, under "From plugins". It shows the price now exactly
as your price tile does: the same sensor, formula, unit and decimals. With more than one price tile on the screen, the
first one with a sensor gives it. The top bar needs a price tile on the screen, on any page.

## Good to know

- The graph has a bar for every quarter of an hour when the sensor has quarter prices, else for every hour. A tile
  with more than 100 slots (quarters of today and tomorrow) joins neighbouring quarters into one bar; the card shows
  them all. Past bars are grey, the bar now is the accent colour, and the bars to come are green, amber or red by their
  place between the lowest and highest price. Prices below zero hang under the zero line.
- Tomorrow's bars appear on their own as soon as the sensor has them (the day-ahead prices come in the afternoon).
- The price of now is the current time slot of today's list, as the bar of now shows it, not the sensor's state.
  For ENTSO-e, choose its average-price sensor for a tile: it is the one with the lists (the tile and the top bar
  still show the price of now).
- The unit comes from the sensor (`EUR/kWh` shows as `€/kWh`). A sensor that reports per MWh is shown per kWh.
- On the days the clock changes, the graph has 23 or 25 hours, each where it is on the clock.
