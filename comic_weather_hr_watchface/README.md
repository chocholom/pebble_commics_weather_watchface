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

`resources/images/base_layout.png` is a full-screen static background drawn first
(all the ink art, rays, chips, captions). `src/c/main.c` draws the dynamic content
(time, date, weather, forecast, chips numbers) on top. The weather scenes are
blitted per condition; the small forecast icons are RGBA PNGs composited with
`GCompOpSet`.

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
