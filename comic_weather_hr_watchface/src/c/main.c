#include <pebble.h>
#include "demo.h"

#define SETTINGS_KEY 42
#define WEATHER_CACHE_KEY 43
#define MIN_REFRESH_MINUTES 15
#define MAX_REFRESH_MINUTES 180
#define DEFAULT_REFRESH_MINUTES 60
#define INITIAL_RETRY_MINUTES 15
#define MAX_WEATHER_CACHE_AGE_HOURS 12
#define DEFAULT_HOT_THRESHOLD_C 30

// All colours sit on the Pebble 64-colour grid (channels in {0,85,170,255})
// so they match the generated art exactly.
#define COLOR_PAPER GColorFromHEX(0xFFFFFF)
#define COLOR_GOLD GColorFromHEX(0xFFAA00)
#define COLOR_GOLD_SH GColorFromHEX(0xAA5500)

typedef struct ClaySettings {
  uint8_t TimeFormat;       // 0 = system, 1 = 24h, 2 = 12h
  uint8_t TemperatureUnit;  // 0 = Celsius, 1 = Fahrenheit
  uint8_t ForecastMode;     // 0 = full, 1 = 4h only, 2 = daily only, 3 = current only
  uint8_t RefreshMinutes;   // 15, 30, 60, 120, 180
  bool ShowHeartRate;
  bool LiveHeartRate;
  uint8_t DividerColor;     // 0 = white, 1 = grey, 2 = black
  uint8_t HotThresholdC;    // tomorrow panel "hot" threshold, always stored in Celsius
  bool ShowStatusChips;     // steps / HR / battery strip at the top
  bool RainFill;            // fill hourly icons blue by rain probability
} ClaySettings;

typedef struct WeatherState {
  bool valid;
  int temp_now;
  int code_now;
  int hour_temp[4];
  int hour_code[4];
  int hour_pop[4];
  char hour_label[4][4];
  int today_min;
  int today_max;
  int today_code;
  int today_pop;
  int tomorrow_min;
  int tomorrow_max;
  int tomorrow_code;
  int tomorrow_pop;
  char updated[8];
  int32_t fetched_at;
} WeatherState;

typedef enum WeatherKind {
  WEATHER_CLEAR,
  WEATHER_CLOUD,
  WEATHER_RAIN,
  WEATHER_SNOW,
  WEATHER_STORM,
  WEATHER_FOG
} WeatherKind;

typedef enum TomorrowTheme {
  TOMORROW_NICE,
  TOMORROW_HOT,
  TOMORROW_RAIN,
  TOMORROW_SNOW,
  TOMORROW_FOG,
  TOMORROW_CLOUD
} TomorrowTheme;

typedef enum DividerColor {
  DIVIDER_WHITE,
  DIVIDER_GREY,
  DIVIDER_BLACK
} DividerColor;

static Window *s_main_window;
static Layer *s_canvas_layer;
static ClaySettings s_settings;
static WeatherState s_weather;
static int s_battery_level = 0;
static bool s_bluetooth_connected = true;
static int s_heart_rate = -1;
static int s_step_count = 0;
static int32_t s_last_weather_request_at = 0;

static GFont s_font_time;   // Lilita 40
static GFont s_font_big;    // Lilita 26
static GFont s_font_med;    // Lilita 16
static GFont s_font_small;  // Lilita 14

static GBitmap *s_img_base_layout;
static uint32_t s_loaded_base_resource_id = 0;
// Only the current condition's scene stays in RAM. Full-screen base-layout
// swaps need ~2x the bitmap size of free heap for the PNG decode, so the
// scene is loaded on demand and freed around every base swap.
static GBitmap *s_img_scene;
static uint32_t s_loaded_scene_resource_id = 0;
static GBitmap *s_img_icon_clear;
static GBitmap *s_img_icon_cloud;
static GBitmap *s_img_icon_rain;
static GBitmap *s_img_icon_snow;
static GBitmap *s_img_icon_storm;
static GBitmap *s_img_icon_fog;
static GBitmap *s_img_chips_overlay;
static GBitmap *s_img_icon_clear_blue;
static GBitmap *s_img_icon_cloud_blue;
static GBitmap *s_img_icon_rain_blue;
static GBitmap *s_img_icon_snow_blue;
static GBitmap *s_img_icon_storm_blue;
static GBitmap *s_img_icon_fog_blue;

static int prv_tuple_int(Tuple *tuple, int fallback) {
  if (!tuple) return fallback;
  switch (tuple->type) {
    case TUPLE_INT: return (int)tuple->value->int32;
    case TUPLE_UINT: return (int)tuple->value->uint32;
    case TUPLE_CSTRING: return atoi(tuple->value->cstring);
    default: return fallback;
  }
}

static void prv_default_settings(void) {
  s_settings.TimeFormat = 0;
  s_settings.TemperatureUnit = 0;
  s_settings.ForecastMode = 0;
  s_settings.RefreshMinutes = DEFAULT_REFRESH_MINUTES;
  s_settings.ShowHeartRate = true;
  s_settings.LiveHeartRate = false;
  s_settings.DividerColor = DIVIDER_WHITE;
  s_settings.HotThresholdC = DEFAULT_HOT_THRESHOLD_C;
  s_settings.ShowStatusChips = true;
  s_settings.RainFill = true;
}

static void prv_validate_settings(void) {
  if (s_settings.RefreshMinutes != 15 && s_settings.RefreshMinutes != 30 &&
      s_settings.RefreshMinutes != 60 && s_settings.RefreshMinutes != 120 &&
      s_settings.RefreshMinutes != 180) {
    s_settings.RefreshMinutes = DEFAULT_REFRESH_MINUTES;
  }
  if (s_settings.ForecastMode > 3) {
    s_settings.ForecastMode = 0;
  }
  if (s_settings.TimeFormat > 3) {
    s_settings.TimeFormat = 0;
  }
  if (s_settings.TemperatureUnit > 1) {
    s_settings.TemperatureUnit = 0;
  }
  if (s_settings.DividerColor > DIVIDER_BLACK) {
    s_settings.DividerColor = DIVIDER_WHITE;
  }
  if (s_settings.HotThresholdC < 20 || s_settings.HotThresholdC > 45) {
    s_settings.HotThresholdC = DEFAULT_HOT_THRESHOLD_C;
  }
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  prv_validate_settings();
}

static void prv_save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static bool prv_weather_cache_age_ok(void) {
  if (!s_weather.valid || s_weather.fetched_at <= 0) return false;
  int32_t now = (int32_t)time(NULL);
  int32_t age = now - s_weather.fetched_at;
  return age >= 0 && age <= (MAX_WEATHER_CACHE_AGE_HOURS * 60 * 60);
}

static void prv_normalize_weather_strings(void) {
  s_weather.updated[sizeof(s_weather.updated) - 1] = '\0';
  for (int i = 0; i < 4; i++) {
    s_weather.hour_label[i][sizeof(s_weather.hour_label[i]) - 1] = '\0';
  }
}

static void prv_load_weather_cache(void) {
  WeatherState cached;
  if (persist_read_data(WEATHER_CACHE_KEY, &cached, sizeof(cached)) == (int)sizeof(cached)) {
    s_weather = cached;
    prv_normalize_weather_strings();
    if (!prv_weather_cache_age_ok()) {
      s_weather.valid = false;
    }
  }
}

static void prv_save_weather_cache(void) {
  if (s_weather.valid) {
    persist_write_data(WEATHER_CACHE_KEY, &s_weather, sizeof(s_weather));
  }
}

static bool prv_use_24h(void) {
  if (s_settings.TimeFormat == 1) return true;
  if (s_settings.TimeFormat >= 2) return false;  // 2 = 12h, 3 = 12h + AM/PM
  return clock_is_24h_style();
}

static void prv_format_time(char *buffer, size_t size) {
#if DEMO_MODE
  snprintf(buffer, size, "%s", DEMO_TIME);
  return;
#endif
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  if (prv_use_24h()) {
    strftime(buffer, size, "%H:%M", tick_time);
  } else {
    strftime(buffer, size, "%I:%M", tick_time);
    if (buffer[0] == '0') memmove(buffer, buffer + 1, strlen(buffer));
  }
}

static void prv_format_calendar_parts(char *dow, size_t dow_size, char *month, size_t month_size, char *day, size_t day_size) {
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  strftime(dow, dow_size, "%a", tick_time);
  strftime(month, month_size, "%b", tick_time);
  strftime(day, day_size, "%d", tick_time);
  if (day[0] == '0') memmove(day, day + 1, strlen(day));
  for (size_t i = 0; dow[i]; i++) if (dow[i] >= 'a' && dow[i] <= 'z') dow[i] -= 32;
  for (size_t i = 0; month[i]; i++) if (month[i] >= 'a' && month[i] <= 'z') month[i] -= 32;
}

static const char *prv_condition_label(int code) {
  if (code == 0) return "CLEAR";
  if (code <= 3) return "CLOUDY";
  if (code <= 48) return "FOG";
  if (code <= 57) return "DRIZZLE";
  if (code <= 67) return "RAIN";
  if (code <= 77) return "SNOW";
  if (code <= 82) return "SHOWERS";
  if (code <= 86) return "SNOW";
  if (code <= 99) return "STORM";
  return "WEATHER";
}


static bool prv_is_rain(int code) {
  return (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
}

static bool prv_is_snow(int code) {
  return (code >= 71 && code <= 77) || (code >= 85 && code <= 86);
}

static WeatherKind prv_weather_kind(int code) {
  if (code == 0) return WEATHER_CLEAR;
  if (code >= 95) return WEATHER_STORM;
  if (prv_is_snow(code)) return WEATHER_SNOW;
  if (prv_is_rain(code)) return WEATHER_RAIN;
  if (code >= 45 && code <= 48) return WEATHER_FOG;
  return WEATHER_CLOUD;
}

static int prv_hot_threshold_display_units(void) {
  if (s_settings.TemperatureUnit == 1) {
    return (s_settings.HotThresholdC * 9 + 2) / 5 + 32;
  }
  return s_settings.HotThresholdC;
}

static TomorrowTheme prv_tomorrow_theme(void) {
  if (!s_weather.valid) return TOMORROW_NICE;

  int code = s_weather.tomorrow_code;
  if (prv_is_snow(code)) return TOMORROW_SNOW;
  if (code >= 45 && code <= 48) return TOMORROW_FOG;
  if (prv_is_rain(code)) return TOMORROW_RAIN;
  if (s_weather.tomorrow_max >= prv_hot_threshold_display_units()) return TOMORROW_HOT;
  if (prv_weather_kind(code) == WEATHER_CLEAR) return TOMORROW_NICE;
  return TOMORROW_CLOUD;
}

static uint32_t prv_scene_resource_for_code(int code) {
  switch (prv_weather_kind(code)) {
    case WEATHER_CLEAR: return RESOURCE_ID_IMAGE_SCENE_CLEAR;
    case WEATHER_CLOUD: return RESOURCE_ID_IMAGE_SCENE_CLOUD;
    case WEATHER_RAIN: return RESOURCE_ID_IMAGE_SCENE_RAIN;
    case WEATHER_SNOW: return RESOURCE_ID_IMAGE_SCENE_SNOW;
    case WEATHER_STORM: return RESOURCE_ID_IMAGE_SCENE_STORM;
    case WEATHER_FOG: return RESOURCE_ID_IMAGE_SCENE_FOG;
  }
  return RESOURCE_ID_IMAGE_SCENE_CLOUD;
}

static GBitmap *prv_scene_for_code(int code) {
  uint32_t resource_id = prv_scene_resource_for_code(code);
  if (s_loaded_scene_resource_id == resource_id && s_img_scene) return s_img_scene;
  gbitmap_destroy(s_img_scene);
  s_img_scene = gbitmap_create_with_resource(resource_id);
  s_loaded_scene_resource_id = s_img_scene ? resource_id : 0;
  return s_img_scene;
}

static GBitmap *prv_icon_for_code(int code) {
  switch (prv_weather_kind(code)) {
    case WEATHER_CLEAR: return s_img_icon_clear;
    case WEATHER_CLOUD: return s_img_icon_cloud;
    case WEATHER_RAIN: return s_img_icon_rain;
    case WEATHER_SNOW: return s_img_icon_snow;
    case WEATHER_STORM: return s_img_icon_storm;
    case WEATHER_FOG: return s_img_icon_fog;
  }
  return s_img_icon_cloud;
}

static GBitmap *prv_blue_icon_for_code(int code) {
  switch (prv_weather_kind(code)) {
    case WEATHER_CLEAR: return s_img_icon_clear_blue;
    case WEATHER_CLOUD: return s_img_icon_cloud_blue;
    case WEATHER_RAIN: return s_img_icon_rain_blue;
    case WEATHER_SNOW: return s_img_icon_snow_blue;
    case WEATHER_STORM: return s_img_icon_storm_blue;
    case WEATHER_FOG: return s_img_icon_fog_blue;
  }
  return s_img_icon_cloud_blue;
}

static void prv_apply_hr_period(void) {
  if (s_settings.ShowHeartRate && s_settings.LiveHeartRate) {
    health_service_set_heart_rate_sample_period(60);
  } else {
    health_service_set_heart_rate_sample_period(0);
  }
}

static void prv_update_heart_rate(void) {
#if DEMO_MODE
  s_heart_rate = DEMO_HR;
  return;
#endif
  if (!s_settings.ShowHeartRate) {
    s_heart_rate = -1;
    return;
  }
  time_t now = time(NULL);
  HealthServiceAccessibilityMask access = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
  if (access & HealthServiceAccessibilityMaskAvailable) {
    HealthValue value = health_service_peek_current_value(HealthMetricHeartRateBPM);
    s_heart_rate = value > 0 ? (int)value : -1;
  } else {
    s_heart_rate = -1;
  }
}

static void prv_update_step_count(void) {
#if DEMO_MODE
  s_step_count = DEMO_STEPS;
  return;
#endif
  HealthServiceAccessibilityMask access = health_service_metric_accessible(HealthMetricStepCount, time_start_of_today(), time(NULL));
  if (access & HealthServiceAccessibilityMaskAvailable) {
    s_step_count = (int)health_service_sum_today(HealthMetricStepCount);
  } else {
    s_step_count = 0;
  }
}

static void prv_format_compact_number(int value, char *buffer, size_t size) {
  if (value >= 100000) {
    snprintf(buffer, size, "%dk", value / 1000);
  } else {
    snprintf(buffer, size, "%d", value);
  }
}

static void prv_draw_bitmap_centered(GContext *ctx, GBitmap *bitmap, GRect rect) {
  if (!bitmap) return;
  GRect b = gbitmap_get_bounds(bitmap);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);  // icons are RGBA PNGs
  graphics_draw_bitmap_in_rect(ctx, bitmap, GRect(rect.origin.x + (rect.size.w - b.size.w) / 2,
                                                  rect.origin.y + (rect.size.h - b.size.h) / 2,
                                                  b.size.w, b.size.h));
  graphics_context_set_compositing_mode(ctx, GCompOpAssign);
}


static void prv_draw_text_outline(GContext *ctx, const char *text, GFont font, GRect box,
                                  GColor fill, GColor outline, GTextAlignment align, int r) {
  graphics_context_set_text_color(ctx, outline);
  for (int ox = -r; ox <= r; ox++) {
    for (int oy = -r; oy <= r; oy++) {
      if (ox == 0 && oy == 0) continue;
      graphics_draw_text(ctx, text, font, GRect(box.origin.x + ox, box.origin.y + oy, box.size.w, box.size.h),
                         GTextOverflowModeTrailingEllipsis, align, NULL);
    }
  }
  graphics_context_set_text_color(ctx, fill);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis, align, NULL);
}


static void prv_draw_comic_text(GContext *ctx, const char *text, GFont font, GRect box,
                                GColor fill, GColor shadow, GTextAlignment align, int r) {
  graphics_context_set_text_color(ctx, shadow);
  graphics_draw_text(ctx, text, font, GRect(box.origin.x + 3, box.origin.y + 3, box.size.w, box.size.h),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
  prv_draw_text_outline(ctx, text, font, box, fill, GColorBlack, align, r);
}


// Comic caption chips along the top of the page. The wobbly chip boxes and the
// sneaker / heart / battery glyphs are baked into base_layout.png; only the
// numbers (and the battery level fill) are drawn here. Lilita 14 caps sit 3px
// below the GRect top, so boxes use y = cap_top - 3.
static void prv_draw_status_chips(GContext *ctx) {
  if (!s_settings.ShowStatusChips) return;
  graphics_context_set_text_color(ctx, GColorBlack);

  char steps[12];
  prv_format_compact_number(s_step_count, steps, sizeof(steps));
  graphics_draw_text(ctx, steps, s_font_small, GRect(23, 2, 40, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char hr[12];
  if (s_settings.ShowHeartRate && s_heart_rate > 0) snprintf(hr, sizeof(hr), "%d", s_heart_rate);
  else snprintf(hr, sizeof(hr), "--");
  graphics_draw_text(ctx, hr, s_font_small, GRect(88, 1, 26, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // battery level fill inside the baked battery outline (126,8)-(144,17)
  int fill_w = (14 * s_battery_level) / 100;
  GColor fill = s_battery_level < 20 ? GColorRed : (s_battery_level < 45 ? GColorOrange : GColorGreen);
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, GRect(128, 10, fill_w, 5), 0, GCornerNone);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", s_battery_level);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, pct, s_font_small, GRect(150, 3, 48, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}


static void prv_draw_calendar(GContext *ctx) {
  char dow[8], month[8], day[4];
  prv_format_calendar_parts(dow, sizeof(dow), month, sizeof(month), day, sizeof(day));
  // Tear-off pad art: red band y29-44, white body y44-80 (baked into base_layout).
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, month, s_font_small, GRect(9, 27, 62, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, day, s_font_big, GRect(9, 40, 62, 30),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, dow, s_font_small, GRect(9, 64, 62, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}


static void prv_draw_weather_scene(GContext *ctx) {
  // Scene PNG carries its own slanted ink frame; the gutter corners are
  // transparent so the action band underneath shows through (GCompOpSet).
  GBitmap *scene = prv_scene_for_code(s_weather.valid ? s_weather.code_now : 0);
  if (scene) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, scene, GRect(84, 20, 116, 77));
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }

  // current temperature, white bubble numerals over the sky
  char temp[16];
  if (s_weather.valid) snprintf(temp, sizeof(temp), "%d°", s_weather.temp_now);
  else snprintf(temp, sizeof(temp), "--°");
  prv_draw_text_outline(ctx, temp, s_font_big, GRect(100, 18, 92, 30),
                        GColorWhite, GColorBlack, GTextAlignmentRight, 2);

  // condition caption box, bottom-left of the scene panel
  const char *cond = s_weather.valid ? prv_condition_label(s_weather.code_now) : "SYNCING";
  GSize sz = graphics_text_layout_get_content_size(cond, s_font_small, GRect(0, 0, 110, 20),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(89, 74, sz.w + 9, 13), 2, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, cond, s_font_small, GRect(93, 73, sz.w + 2, 15),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}


static void prv_draw_center_panel(GContext *ctx) {
  // time in the starburst (burst art centred on (100,129))
  char time_buf[8];
  prv_format_time(time_buf, sizeof(time_buf));
  prv_draw_comic_text(ctx, time_buf, s_font_time, GRect(0, 102, 200, 52),
                      COLOR_GOLD, COLOR_GOLD_SH, GTextAlignmentCenter, 2);

  if (s_settings.TimeFormat == 3) {
    // small AM/PM tag in the top-right corner of the action band
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    prv_draw_text_outline(ctx, t->tm_hour < 12 ? "AM" : "PM", s_font_small,
                          GRect(160, 103, 36, 16), GColorWhite, GColorBlack, GTextAlignmentRight, 1);
  }

  if (!s_bluetooth_connected) {
    // little "!" bubble on the action band when the phone is gone
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(189, 112), 7);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, GPoint(189, 112), 7);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, "!", s_font_small, GRect(184, 104, 11, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}


static void prv_draw_today_panel(GContext *ctx) {
  if (!s_weather.valid) return;

  int cx[4] = {16, 47, 77, 108};
  for (int i = 0; i < 4; i++) {
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, s_weather.hour_label[i], s_font_small, GRect(cx[i] - 15, 176, 30, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    prv_draw_bitmap_centered(ctx, prv_icon_for_code(s_weather.hour_code[i]), GRect(cx[i] - 11, 192, 22, 22));
    if (s_settings.RainFill) {
      // rain-chance "water level": overlay the bottom pop% of the blue icon
      int pop = s_weather.hour_pop[i];
      if (pop > 100) pop = 100;
      int rows = (22 * pop) / 100;
      GBitmap *blue = rows > 0 ? prv_blue_icon_for_code(s_weather.hour_code[i]) : NULL;
      GBitmap *sub = blue ? gbitmap_create_as_sub_bitmap(blue, GRect(0, 22 - rows, 22, rows)) : NULL;
      if (sub) {
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, sub, GRect(cx[i] - 11, 192 + 22 - rows, 22, rows));
        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
        gbitmap_destroy(sub);
      }
    }
    // no degree sign: Lilita's ° rasterises as a blob below ~20px
    char temp[8];
    snprintf(temp, sizeof(temp), "%d", s_weather.hour_temp[i]);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, temp, s_font_med, GRect(cx[i] - 15, 209, 30, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}


static void prv_ensure_base_layout(void);

static void prv_draw_tomorrow_panel(GContext *ctx) {
  if (!s_weather.valid) return;
  prv_draw_bitmap_centered(ctx, prv_icon_for_code(s_weather.tomorrow_code), GRect(154, 180, 22, 22));
  char temps[20];
  snprintf(temps, sizeof(temps), "%d/%d", s_weather.tomorrow_min, s_weather.tomorrow_max);
  // White with ink outline stays readable on every tomorrow theme (dark rain
  // blues and fog greys included), unlike plain black on the old orange panel.
  prv_draw_text_outline(ctx, temps, s_font_med, GRect(128, 204, 72, 20),
                        GColorWhite, GColorBlack, GTextAlignmentCenter, 1);
}



static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  prv_ensure_base_layout();
  if (s_img_base_layout) {
    graphics_draw_bitmap_in_rect(ctx, s_img_base_layout, bounds);
  } else {
    graphics_context_set_fill_color(ctx, COLOR_PAPER);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  // chip boxes are an RGBA overlay (not baked) so they can be hidden
  if (s_settings.ShowStatusChips && s_img_chips_overlay) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_img_chips_overlay, GRect(0, 0, 200, 26));
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }

  prv_draw_weather_scene(ctx);
  prv_draw_status_chips(ctx);
  prv_draw_calendar(ctx);
  prv_draw_center_panel(ctx);

  if (s_settings.ForecastMode == 0) {
    prv_draw_today_panel(ctx);
    prv_draw_tomorrow_panel(ctx);
  } else if (s_settings.ForecastMode == 1) {
    prv_draw_today_panel(ctx);
  } else if (s_settings.ForecastMode == 2) {
    prv_draw_tomorrow_panel(ctx);
  }
}


static void prv_request_weather(void);
static void prv_request_weather_if_due(void);
static void prv_request_weather_timer(void *context);

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  (void)tick_time;
  if (units_changed & MINUTE_UNIT) {
    layer_mark_dirty(s_canvas_layer);
    prv_request_weather_if_due();
  }
}

static void prv_battery_handler(BatteryChargeState charge_state) {
#if DEMO_MODE
  s_battery_level = DEMO_BATT;
#else
  s_battery_level = charge_state.charge_percent;
#endif
  layer_mark_dirty(s_canvas_layer);
}

static void prv_bluetooth_handler(bool connected) {
  s_bluetooth_connected = connected;
  layer_mark_dirty(s_canvas_layer);
}

static void prv_health_handler(HealthEventType type, void *context) {
  if (type == HealthEventHeartRateUpdate || type == HealthEventSignificantUpdate || type == HealthEventMovementUpdate) {
    prv_update_heart_rate();
    prv_update_step_count();
    layer_mark_dirty(s_canvas_layer);
  }
}

static void prv_store_weather_string(char *target, size_t target_size, Tuple *tuple, const char *fallback) {
  if (tuple && tuple->type == TUPLE_CSTRING) snprintf(target, target_size, "%s", tuple->value->cstring);
  else snprintf(target, target_size, "%s", fallback);
}

static void prv_inbox_received_callback(DictionaryIterator *iterator, void *context) {
  bool got_weather = false;
  Tuple *error = dict_find(iterator, MESSAGE_KEY_WEATHER_ERROR);
  if (error && prv_tuple_int(error, 0)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Weather fetch failed on phone");
    // Rewind the request throttle so the next retry happens after the short
    // interval instead of waiting out a full (up to 3h) refresh window.
    int32_t refresh_seconds = s_settings.RefreshMinutes * 60;
    int32_t retry_seconds = INITIAL_RETRY_MINUTES * 60;
    if (refresh_seconds > retry_seconds) {
      s_last_weather_request_at = (int32_t)time(NULL) - (refresh_seconds - retry_seconds);
    }
  }

  Tuple *t = dict_find(iterator, MESSAGE_KEY_WEATHER_TEMP_NOW);
  if (t) {
    s_weather.temp_now = prv_tuple_int(t, s_weather.temp_now);
    got_weather = true;
  }
  t = dict_find(iterator, MESSAGE_KEY_WEATHER_CODE_NOW);
  if (t) s_weather.code_now = prv_tuple_int(t, s_weather.code_now);
  prv_store_weather_string(s_weather.updated, sizeof(s_weather.updated), dict_find(iterator, MESSAGE_KEY_WEATHER_UPDATED), "--:--");

  Tuple *tuples_temp[4] = {
    dict_find(iterator, MESSAGE_KEY_HOUR0_TEMP), dict_find(iterator, MESSAGE_KEY_HOUR1_TEMP),
    dict_find(iterator, MESSAGE_KEY_HOUR2_TEMP), dict_find(iterator, MESSAGE_KEY_HOUR3_TEMP)
  };
  Tuple *tuples_code[4] = {
    dict_find(iterator, MESSAGE_KEY_HOUR0_CODE), dict_find(iterator, MESSAGE_KEY_HOUR1_CODE),
    dict_find(iterator, MESSAGE_KEY_HOUR2_CODE), dict_find(iterator, MESSAGE_KEY_HOUR3_CODE)
  };
  Tuple *tuples_pop[4] = {
    dict_find(iterator, MESSAGE_KEY_HOUR0_POP), dict_find(iterator, MESSAGE_KEY_HOUR1_POP),
    dict_find(iterator, MESSAGE_KEY_HOUR2_POP), dict_find(iterator, MESSAGE_KEY_HOUR3_POP)
  };
  Tuple *tuples_label[4] = {
    dict_find(iterator, MESSAGE_KEY_HOUR0_TIME), dict_find(iterator, MESSAGE_KEY_HOUR1_TIME),
    dict_find(iterator, MESSAGE_KEY_HOUR2_TIME), dict_find(iterator, MESSAGE_KEY_HOUR3_TIME)
  };

  for (int i = 0; i < 4; i++) {
    s_weather.hour_temp[i] = prv_tuple_int(tuples_temp[i], s_weather.hour_temp[i]);
    s_weather.hour_code[i] = prv_tuple_int(tuples_code[i], s_weather.hour_code[i]);
    s_weather.hour_pop[i] = prv_tuple_int(tuples_pop[i], s_weather.hour_pop[i]);
    prv_store_weather_string(s_weather.hour_label[i], sizeof(s_weather.hour_label[i]), tuples_label[i], "--");
  }

  s_weather.today_min = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TODAY_MIN), s_weather.today_min);
  s_weather.today_max = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TODAY_MAX), s_weather.today_max);
  s_weather.today_code = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TODAY_CODE), s_weather.today_code);
  s_weather.today_pop = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TODAY_POP), s_weather.today_pop);
  s_weather.tomorrow_min = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TOMORROW_MIN), s_weather.tomorrow_min);
  s_weather.tomorrow_max = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TOMORROW_MAX), s_weather.tomorrow_max);
  s_weather.tomorrow_code = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TOMORROW_CODE), s_weather.tomorrow_code);
  s_weather.tomorrow_pop = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_TOMORROW_POP), s_weather.tomorrow_pop);

  if (got_weather) {
    s_weather.fetched_at = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_FETCHED_AT), (int)time(NULL));
    s_weather.valid = true;
    if (prv_weather_cache_age_ok()) {
      prv_save_weather_cache();
    } else {
      s_weather.valid = false;
    }
  }

  bool changed = false;
  Tuple *time_format = dict_find(iterator, MESSAGE_KEY_TimeFormat);
  Tuple *temp_unit = dict_find(iterator, MESSAGE_KEY_TemperatureUnit);
  Tuple *forecast_mode = dict_find(iterator, MESSAGE_KEY_ForecastMode);
  Tuple *show_hr = dict_find(iterator, MESSAGE_KEY_ShowHeartRate);
  Tuple *live_hr = dict_find(iterator, MESSAGE_KEY_LiveHeartRate);
  Tuple *refresh = dict_find(iterator, MESSAGE_KEY_RefreshMinutes);
  Tuple *divider = dict_find(iterator, MESSAGE_KEY_DividerColor);
  Tuple *hot_threshold = dict_find(iterator, MESSAGE_KEY_HotThresholdC);
  Tuple *show_chips = dict_find(iterator, MESSAGE_KEY_ShowStatusChips);
  Tuple *rain_fill = dict_find(iterator, MESSAGE_KEY_RainFill);

  if (time_format) { s_settings.TimeFormat = prv_tuple_int(time_format, s_settings.TimeFormat); changed = true; }
  if (temp_unit) {
    int old = s_settings.TemperatureUnit;
    s_settings.TemperatureUnit = prv_tuple_int(temp_unit, s_settings.TemperatureUnit) ? 1 : 0;
    changed = true;
    if (old != s_settings.TemperatureUnit) {
      s_weather.valid = false;
      // Drop the persisted forecast too; a restart before the next successful
      // fetch must not resurrect temperatures in the old unit.
      persist_delete(WEATHER_CACHE_KEY);
      s_last_weather_request_at = 0;
      app_timer_register(500, prv_request_weather_timer, NULL);
    }
  }
  if (forecast_mode) {
    int mode = prv_tuple_int(forecast_mode, s_settings.ForecastMode);
    if (mode < 0 || mode > 3) mode = 0;
    s_settings.ForecastMode = mode;
    changed = true;
  }
  if (show_hr) { s_settings.ShowHeartRate = prv_tuple_int(show_hr, s_settings.ShowHeartRate) ? true : false; changed = true; }
  if (live_hr) { s_settings.LiveHeartRate = prv_tuple_int(live_hr, s_settings.LiveHeartRate) ? true : false; changed = true; }
  if (refresh) {
    int minutes = prv_tuple_int(refresh, s_settings.RefreshMinutes);
    if (minutes != 15 && minutes != 30 && minutes != 60 && minutes != 120 && minutes != 180) {
      minutes = DEFAULT_REFRESH_MINUTES;
    }
    s_settings.RefreshMinutes = minutes;
    changed = true;
  }
  if (divider) {
    int color = prv_tuple_int(divider, s_settings.DividerColor);
    if (color < DIVIDER_WHITE || color > DIVIDER_BLACK) color = DIVIDER_WHITE;
    s_settings.DividerColor = color;
    changed = true;
  }
  if (hot_threshold) {
    int threshold = prv_tuple_int(hot_threshold, s_settings.HotThresholdC);
    if (threshold < 20 || threshold > 45) threshold = DEFAULT_HOT_THRESHOLD_C;
    s_settings.HotThresholdC = threshold;
    changed = true;
  }
  if (show_chips) { s_settings.ShowStatusChips = prv_tuple_int(show_chips, s_settings.ShowStatusChips) ? true : false; changed = true; }
  if (rain_fill) { s_settings.RainFill = prv_tuple_int(rain_fill, s_settings.RainFill) ? true : false; changed = true; }

  if (changed) {
    prv_validate_settings();
    prv_save_settings();
    prv_apply_hr_period();
    prv_update_heart_rate();
    prv_update_step_count();
  }

  layer_mark_dirty(s_canvas_layer);
}

static void prv_inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Inbox dropped: %d", reason);
}

static void prv_outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox failed: %d", reason);
}

static void prv_request_weather(void) {
#if DEMO_MODE
  return;  // keep the fake forecast; never fetch real weather
#endif
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK || !iter) return;
  s_last_weather_request_at = (int32_t)time(NULL);
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
  dict_write_uint8(iter, MESSAGE_KEY_TemperatureUnit, s_settings.TemperatureUnit);
  dict_write_uint8(iter, MESSAGE_KEY_RefreshMinutes, s_settings.RefreshMinutes);
  app_message_outbox_send();
}

static bool prv_weather_due_for_refresh(void) {
#if DEMO_MODE
  return false;
#endif
  int32_t now = (int32_t)time(NULL);
  int32_t retry_seconds = s_weather.valid ? (s_settings.RefreshMinutes * 60) : (INITIAL_RETRY_MINUTES * 60);

  if (s_last_weather_request_at > 0 && now - s_last_weather_request_at < retry_seconds) {
    return false;
  }
  if (!s_weather.valid) {
    return true;
  }
  if (s_weather.fetched_at <= 0) {
    return true;
  }
  return now - s_weather.fetched_at >= s_settings.RefreshMinutes * 60;
}

static void prv_request_weather_if_due(void) {
  if (prv_weather_due_for_refresh()) {
    prv_request_weather();
  }
}

static void prv_request_weather_timer(void *context) {
  (void)context;
  prv_request_weather_if_due();
}

static uint32_t prv_base_resource_for_state(void) {
  int divider = s_settings.DividerColor;
  if (divider < DIVIDER_WHITE || divider > DIVIDER_BLACK) divider = DIVIDER_WHITE;

  static const uint32_t resources[3][6] = {
    {
      RESOURCE_ID_IMAGE_BASE_LAYOUT,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_WHITE_HOT,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_WHITE_RAIN,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_WHITE_SNOW,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_WHITE_FOG,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_WHITE_CLOUD,
    },
    {
      RESOURCE_ID_IMAGE_BASE_LAYOUT_GREY_NICE,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_GREY_HOT,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_GREY_RAIN,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_GREY_SNOW,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_GREY_FOG,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_GREY_CLOUD,
    },
    {
      RESOURCE_ID_IMAGE_BASE_LAYOUT_BLACK_NICE,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_BLACK_HOT,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_BLACK_RAIN,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_BLACK_SNOW,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_BLACK_FOG,
      RESOURCE_ID_IMAGE_BASE_LAYOUT_BLACK_CLOUD,
    }
  };

  return resources[divider][prv_tomorrow_theme()];
}

static void prv_ensure_base_layout(void) {
  uint32_t resource_id = prv_base_resource_for_state();
  if (s_loaded_base_resource_id == resource_id && s_img_base_layout) return;

  // Free the two biggest bitmaps before decoding: the PNG decode of a
  // full-screen layout needs roughly twice the bitmap size in free heap,
  // which is only available with the old layout and the scene released.
  // The scene reloads on demand during the same frame.
  gbitmap_destroy(s_img_base_layout);
  s_img_base_layout = NULL;
  s_loaded_base_resource_id = 0;
  gbitmap_destroy(s_img_scene);
  s_img_scene = NULL;
  s_loaded_scene_resource_id = 0;

  GBitmap *next = gbitmap_create_with_resource(resource_id);
  if (!next && resource_id != RESOURCE_ID_IMAGE_BASE_LAYOUT) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Base layout %lu failed to load, using default", (unsigned long)resource_id);
    resource_id = RESOURCE_ID_IMAGE_BASE_LAYOUT;
    next = gbitmap_create_with_resource(resource_id);
  }
  if (!next) return;

  s_img_base_layout = next;
  s_loaded_base_resource_id = resource_id;
}

static void prv_load_bitmaps(void) {
  prv_ensure_base_layout();
  s_img_icon_clear = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_CLEAR_SMALL);
  s_img_icon_cloud = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_CLOUD_SMALL);
  s_img_icon_rain = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_RAIN_SMALL);
  s_img_icon_snow = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_SNOW_SMALL);
  s_img_icon_storm = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_STORM_SMALL);
  s_img_icon_fog = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_FOG_SMALL);
  s_img_chips_overlay = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_CHIPS_OVERLAY);
  s_img_icon_clear_blue = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_CLEAR_SMALL_BLUE);
  s_img_icon_cloud_blue = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_CLOUD_SMALL_BLUE);
  s_img_icon_rain_blue = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_RAIN_SMALL_BLUE);
  s_img_icon_snow_blue = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_SNOW_SMALL_BLUE);
  s_img_icon_storm_blue = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_STORM_SMALL_BLUE);
  s_img_icon_fog_blue = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ICON_FOG_SMALL_BLUE);
}

static void prv_destroy_bitmaps(void) {
  gbitmap_destroy(s_img_base_layout);
  gbitmap_destroy(s_img_scene);
  gbitmap_destroy(s_img_icon_clear);
  gbitmap_destroy(s_img_icon_cloud);
  gbitmap_destroy(s_img_icon_rain);
  gbitmap_destroy(s_img_icon_snow);
  gbitmap_destroy(s_img_icon_storm);
  gbitmap_destroy(s_img_icon_fog);
  gbitmap_destroy(s_img_chips_overlay);
  gbitmap_destroy(s_img_icon_clear_blue);
  gbitmap_destroy(s_img_icon_cloud_blue);
  gbitmap_destroy(s_img_icon_rain_blue);
  gbitmap_destroy(s_img_icon_snow_blue);
  gbitmap_destroy(s_img_icon_storm_blue);
  gbitmap_destroy(s_img_icon_fog_blue);
}

static void prv_main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);
}

static void prv_main_window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
}

static void prv_init_weather_defaults(void) {
  memset(&s_weather, 0, sizeof(s_weather));
  s_weather.valid = false;
  snprintf(s_weather.updated, sizeof(s_weather.updated), "--:--");
  for (int i = 0; i < 4; i++) snprintf(s_weather.hour_label[i], sizeof(s_weather.hour_label[i]), "--");
}

#if DEMO_MODE
static void prv_apply_demo(void) {
  const char *labels[4] = DEMO_HOURS;
  const int temps[4] = DEMO_HOUR_TEMPS;
  const int codes[4] = DEMO_HOUR_CODES;
  const int pops[4] = DEMO_HOUR_POPS;
  s_weather.valid = true;
  s_weather.temp_now = DEMO_TEMP_NOW;
  s_weather.code_now = DEMO_CODE_NOW;
  for (int i = 0; i < 4; i++) {
    s_weather.hour_temp[i] = temps[i];
    s_weather.hour_code[i] = codes[i];
    s_weather.hour_pop[i] = pops[i];
    snprintf(s_weather.hour_label[i], sizeof(s_weather.hour_label[i]), "%s", labels[i]);
  }
  s_weather.tomorrow_min = DEMO_TMRW_MIN;
  s_weather.tomorrow_max = DEMO_TMRW_MAX;
  s_weather.tomorrow_code = DEMO_TMRW_CODE;
  s_weather.fetched_at = (int32_t)time(NULL);
  s_step_count = DEMO_STEPS;
  s_heart_rate = DEMO_HR;
  s_battery_level = DEMO_BATT;
}
#endif

static void prv_init(void) {
  prv_load_settings();
  prv_init_weather_defaults();
  prv_load_weather_cache();
#if DEMO_MODE
  prv_apply_demo();
#endif
  // Weather state (cache or demo) must be final before the first bitmap load
  // so the initial base layout already matches the tomorrow theme.
  prv_load_bitmaps();

  s_font_time = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_COMIC_44));
  s_font_big = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_COMIC_26));
  s_font_med = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_COMIC_16));
  s_font_small = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_COMIC_14));

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = prv_main_window_load,
    .unload = prv_main_window_unload
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  battery_state_service_subscribe(prv_battery_handler);
  bluetooth_connection_service_subscribe(prv_bluetooth_handler);
  health_service_events_subscribe(prv_health_handler, NULL);

  prv_battery_handler(battery_state_service_peek());
  s_bluetooth_connected = bluetooth_connection_service_peek();
  prv_apply_hr_period();
  prv_update_heart_rate();
  prv_update_step_count();

  app_message_register_inbox_received(prv_inbox_received_callback);
  app_message_register_inbox_dropped(prv_inbox_dropped_callback);
  app_message_register_outbox_failed(prv_outbox_failed_callback);
  app_message_open(1024, 128);

  app_timer_register(1500, prv_request_weather_timer, NULL);
}

static void prv_deinit(void) {
  health_service_set_heart_rate_sample_period(0);
  health_service_events_unsubscribe();
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
  prv_destroy_bitmaps();
  fonts_unload_custom_font(s_font_time);
  fonts_unload_custom_font(s_font_big);
  fonts_unload_custom_font(s_font_med);
  fonts_unload_custom_font(s_font_small);
  window_destroy(s_main_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
