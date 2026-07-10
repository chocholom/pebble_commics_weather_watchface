# Comic Weather HR

A hand-drawn comic-book weather watchface for the **Pebble Time 2 (Emery, 200×228)**,
styled after classic action-comic pages: full-bleed panels cut on tilted "horizon"
lines, radial "action rays", wobbly ink outlines, cel-shaded art, and a big
starburst holding the time. Visual direction inspired by the
[Comic Drop](https://apps.repebble.com/comic-drop_d6c48195f8d14244bbc934d7) watchface.

## Layout (a comic page)

- **Caption chips** (top) — three tilted comic chips: sneaker + step count,
  heart + heart rate, battery + %.
- **Calendar panel** — spiral-bound tear-off pad with a curled corner on a purple
  ray burst: month band, big day numeral, weekday.
- **Weather panel** — hand-drawn scene per condition (clear / cloud / rain / snow /
  storm / fog) with the current temperature in outlined bubble numerals and a black
  condition caption (`RAIN`).
- **Action band** — red ray burst with a huge irregular white starburst; the time
  is drawn in gold Lilita numerals with ink outline + shadow. A small `!` bubble
  appears here when Bluetooth is disconnected.
- **TODAY** — four hourly cells (hour, icon, temperature) on a cream ben-day panel
  with a tilted red caption.
- **TOMORROW** — condition icon and min/max on an orange ray burst with a blue caption.

## How it renders

`resources/images/base_layout.png` is the default full-screen static background
drawn first (all the ink art, rays, captions). `tools/preview.py` also
generates base-layout variants for grey/black gutters and forecast-coloured
TOMORROW panels. The status chips (steps / heart rate / battery) are a separate
RGBA overlay (`chips_overlay.png`) so the whole strip can be hidden from
settings. `src/c/main.c` loads only the currently needed base bitmap,
then draws the dynamic content (time, date, weather, forecast, chips numbers)
on top. The weather scenes are blitted per condition; the small forecast icons
are RGBA PNGs composited with `GCompOpSet`.

Hourly rain probability is shown by "flooding" each hourly icon from the bottom
with blue (`icon_*_small_blue.png` cropped via sub-bitmap to the POP percentage);
it can be turned off in settings. Time can be shown as 24h, 12h, or 12h with a
small AM/PM tag on the action band.

## Shake to reveal, night theme, stale badge, Quick View

A wrist flick cycles two temporary overlay pages (configurable, auto-hide
after 8 s): a details card (feels-like, wind, humidity, 24h rain total,
sunrise/sunset, four-day strip) and a 24h chart (temperature line, rain bars,
wind strip) fed by three 24-byte arrays in the AppMessage payload.

The optional night theme switches the gutters to black or grey between sunset
and sunrise (times come from Open-Meteo's daily block). When a forecast is
older than twice the refresh interval, the current temperature turns grey and
an age tag appears (configurable). During a timeline Quick View peek the
bottom forecast panels are skipped; everything else stays visible.

## Sleep-aware battery saving

While the wearer sleeps (Pebble Health detection, or fixed 23:00-07:00 hours
for watches not worn at night), weather fetches can run on a reduced 4-hour
schedule or pause entirely — with an optional fixed morning fetch time so the
forecast is fresh on wake-up. Frequent heart-rate sampling can optionally drop
back to Pebble's automatic schedule while asleep (off by default, since
frequent-HR users may want sleep data).

## Weather refresh, caching, and battery

The watch requests weather at startup only when its persisted forecast is stale
or missing, then refreshes by forecast age rather than by wall-clock minute
modulo. The configurable refresh choices are 15, 30, 60, 120, and 180 minutes;
new installs default to 60 minutes. If no forecast is available yet, failed
initial fetches are retried at most every 15 minutes.

Successful forecasts are cached in two places:

- on the watch, via Pebble persistent storage, for instant display after app
  restart and to skip fresh startup fetches;
- on the phone JS side, via `localStorage`, so repeated requests inside the
  selected refresh window can avoid both geolocation and network.

When live location or Open-Meteo fails, the phone JS tries the last known
coordinates and can fall back to cached weather up to 12 hours old. Cached
payloads keep their original fetch timestamp, so stale fallback data does not
reset the refresh age.

For lowest battery use, set weather refresh to 2 or 3 hours and leave
`LiveHeartRate` off. With `LiveHeartRate` off, the watchface only reads Pebble's
normal heart-rate samples instead of requesting a 60-second sampling period.

`tools/preview.py` generates **both** the PNG assets and `preview/mock_preview.png`,
which simulates the on-device render (Pebble 64-colour palette, top-aligned text),
so you can verify the look before flashing.

## Fonts

All lettering uses **Lilita One** (`resources/fonts/LilitaOne-Regular.ttf`) at
44/26/16/14 px. Small temperatures skip the `°` glyph — it rasterises as a blob
below ~20 px on the 1-bit font renderer. Note that Pebble's rasteriser places
glyph caps a few px lower than PIL predicts (~5.5 px below the GRect top at
14 px); the C coordinates are calibrated against emulator screenshots.

## Regenerate assets + preview

```sh
python3 tools/preview.py
```

Outputs `resources/images/*.png`, `preview/mock_preview.png`, and `preview/mock_preview_3x.png`.

## Build the watchface

```sh
pebble package install @rebble/clay
pebble build
pebble install --emulator emery   # or --phone <ip> for a real watch
```
