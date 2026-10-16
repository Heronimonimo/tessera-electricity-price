# Electricity price

The electricity price now as a tile, with the graph of today and tomorrow on bigger tiles and on a card of its own.
It reads the price sensor of **Nord Pool** (HACS `nordpool`) or **ENTSO-e** (`entsoe`) in Home Assistant.

## Set up

1. Add the plugin to a screen in Tessera. The screen builds once.
2. In Layout, place the **Electricity price** tile from the library's Plugins group. A 1x1 tile shows the price; from
   2x1 up it adds the graph.
3. In the inspector, choose your price sensor (for Nord Pool the sensor with the prices as its state, for ENTSO-e
   `Average electricity price` or the current price sensor).
4. Optionally show cents (`27.3 ct/kWh`) instead of euros, and set the number of decimals.
5. Tap the tile to open the full screen card: the price now, the day's lowest, average and highest, and the graph.

## Good to know

- The graph has a bar for every hour. A sensor with a price every quarter of an hour is averaged per hour. Past hours
  are grey, the hour now is the accent colour, and the hours to come are green, amber or red by their place between
  the day's lowest and highest price. Prices below zero hang under the zero line.
- Tomorrow's bars appear on their own when the sensor has them (Nord Pool and ENTSO-e publish them in the afternoon).
- The inspector lists every sensor, not only price sensors: the plugin reads both Nord Pool and ENTSO-e, and a list
  filter can only ask for the attributes of one.
- The unit comes from the sensor (`EUR/kWh`). When the sensor reports per MWh, use the cents toggle with care: it
  multiplies by 100.
