# Electricity price

The electricity price now as a tile, with the graph of today and tomorrow on bigger tiles and on a card of its own.
It reads the price sensor of **Nord Pool** (HACS `nordpool`), **ENTSO-e** (`entsoe`) or **GE-Spot** (`ge_spot`) in Home Assistant. For GE-Spot, use its price sensor in a currency per kWh (not the cents display).

## Set up

1. Add the plugin to a screen in Tessera. The screen builds once.
2. In Layout, place the **Electricity price (Nord Pool)** or **Electricity price (ENTSO-e)** tile from the library's
   Plugins group. A tile smaller than 2x2 shows the price with the meter bar; from 2x2 up it adds the graph.
3. In the inspector, choose your price sensor. The list holds only the sensors that fit the tile you placed.
4. Choose the unit, **EUR/kWh** (`0.273`) or **ct/kWh** (`27.3`), and the number of decimals.
5. Tap a bar of the graph to see its price and time slot for a few seconds, on a bigger tile and on the full screen card. On a tile, tap that bar again, or beside the bars, to open the card: the price now, the lowest and highest still to come, the day's average, and the graph.

## Good to know

- The graph has a bar for every quarter of an hour when the sensor has quarter prices, else for every hour. A small
  tile with more than 96 slots joins neighbouring quarters into one bar; the card shows them all. Past bars are grey,
  the bar now is the accent colour, and the bars to come are green, amber or red by their place between the lowest and
  highest price. Prices below zero hang under the zero line.
- Tomorrow's bars appear on their own when the sensor has them (Nord Pool and ENTSO-e publish them in the afternoon).
- The unit comes from the sensor (`EUR/kWh`). When the sensor reports per MWh, choosing ct/kWh
  multiplies the sensor's value by 100.
