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

The sensor gives the market price p. The tile can show what you really pay, and what you get back:

- **Delivery**: (p + **a**) × **b** + **c**. Put the energy tax and your supplier's surcharge in a, the VAT in b
  (1.21 for 21%), and anything that comes after the VAT in c. A fixed price: b 0 and c your price.
- **Feed-in** (switch on **Feed-in price**): (p + a) × b + c with its own a, b and c. A supplier that deducts
  2 cents is a = -0.02, b = 1.

a and c are in the sensor's currency per kWh (0.02, not 2), also when the tile shows cents. With a feed-in price
each bar runs from the feed-in price up to the delivery price, so you see the gap between the two over the day.
The tile and the card show the feed-in price of now under the price. The editor's preview shows the sensor's own
price: it cannot work out the formula.

## Good to know

- The graph has a bar for every quarter of an hour when the sensor has quarter prices, else for every hour. A tile
  with more than 100 slots (quarters of today and tomorrow) joins neighbouring quarters into one bar; the card shows
  them all. Past bars are grey, the bar now is the accent colour, and the bars to come are green, amber or red by their
  place between the lowest and highest price. Prices below zero hang under the zero line.
- Tomorrow's bars appear on their own as soon as the sensor has them (the day-ahead prices come in the afternoon).
- The unit comes from the sensor (`EUR/kWh` shows as `€/kWh`). A sensor that reports per MWh is shown per kWh.
- On the days the clock changes, the graph has 23 or 25 hours, each where it is on the clock.
