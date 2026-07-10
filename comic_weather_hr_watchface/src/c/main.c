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
#define SLEEP_REDUCED_REFRESH_MINUTES 240
#define OVERLAY_HIDE_MS 8000

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
  bool ShowFeelsLike;       // apparent temperature under the current temp
  bool StaleBadge;          // grey temp + age tag when the forecast is old
  uint8_t NightTheme;       // 0 = off, 1 = black gutters at night, 2 = grey
  uint8_t ShakeAction;      // 0 = off, 1 = details page, 2 = details + graph
  uint8_t SleepFetchMode;   // 0 = normal, 1 = reduced (4h), 2 = paused
  uint16_t MorningFetchMin; // daily fetch at this minute-of-day even when paused; 0 = off
  uint8_t SleepDetectMode;  // 0 = Pebble Health (on wrist), 1 = fixed 23:00-07:00
  bool RelaxHrSleep;        // drop frequent HR sampling back to automatic while asleep
} ClaySettings;

typedef struct WeatherState {
  bool valid;
  int temp_now;
  int feels_like;
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
  int wind;               // current wind speed, display units (km/h or mph)
  int wind_dir;           // current wind direction, degrees
  int humidity;           // current relative humidity, %
  int sunrise_min;        // minutes since local midnight, 0 = unknown
  int sunset_min;
  int day_min[2];         // days +2 and +3 for the shake overlay strip
  int day_max[2];
  int day_code[2];
  int8_t graph_temp[24];  // next 24h, display units
  uint8_t graph_rain[24]; // precipitation mm x10 (25.5mm cap)
  uint8_t graph_wind[24]; // wind speed, display units
  uint8_t graph_start_hour;
} WeatherState;

// The whole state must stay persistable as one blob.
_Static_assert(sizeof(WeatherState) <= PERSIST_DATA_MAX_LENGTH,
               "WeatherState exceeds persist limit; split the cache");

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
static bool s_is_sleeping = false;
static uint8_t s_overlay_page = 0;  // 0 = watchface, 1 = details, 2 = graph
static AppTimer *s_overlay_timer = NULL;

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
  s_settings.ShowFeelsLike = false;
  s_settings.StaleBadge = true;
  s_settings.NightTheme = 0;
  s_settings.ShakeAction = 2;
  s_settings.SleepFetchMode = 1;
  s_settings.MorningFetchMin = 0;
  s_settings.SleepDetectMode = 0;
  s_settings.RelaxHrSleep = false;
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
  if (s_settings.NightTheme > 2) s_settings.NightTheme = 0;
  if (s_settings.ShakeAction > 2) s_settings.ShakeAction = 2;
  if (s_settings.SleepFetchMode > 2) s_settings.SleepFetchMode = 1;
  if (s_settings.MorningFetchMin >= 24 * 60) s_settings.MorningFetchMin = 0;
  if (s_settings.SleepDetectMode > 1) s_settings.SleepDetectMode = 0;
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  prv_validate_settings();
}

static void prv_save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static bool prv_fetched_age_ok(int32_t fetched_at) {
  if (fetched_at <= 0) return false;
  int32_t age = (int32_t)time(NULL) - fetched_at;
  // Negative tolerance covers the watch clock stepping backwards (resync)
  // between a save and the next boot; ingest paths clamp to the watch clock.
  return age >= -300 && age <= (MAX_WEATHER_CACHE_AGE_HOURS * 60 * 60);
}

static bool prv_weather_cache_age_ok(void) {
  return s_weather.valid && prv_fetched_age_ok(s_weather.fetched_at);
}

static int32_t prv_weather_age_seconds(void) {
  if (!s_weather.valid || s_weather.fetched_at <= 0) return 0;
  int32_t age = (int32_t)time(NULL) - s_weather.fetched_at;
  return age > 0 ? age : 0;
}

static bool prv_weather_is_stale(void) {
  // Stale = at least one whole refresh window has been missed.
  return prv_weather_age_seconds() > 2 * (int32_t)s_settings.RefreshMinutes * 60;
}

static void prv_format_weather_age(char *buffer, size_t size) {
  int32_t minutes = prv_weather_age_seconds() / 60;
  if (minutes < 100) snprintf(buffer, size, "%ldM OLD", (long)minutes);
  else snprintf(buffer, size, "%ldH OLD", (long)(minutes / 60));
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
    // Same skew forgiveness as the receive path: a watch clock stepped
    // backwards must not make a good cache look future-stamped.
    int32_t now = (int32_t)time(NULL);
    if (s_weather.fetched_at > now) s_weather.fetched_at = now;
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
  bool frequent = s_settings.ShowHeartRate && s_settings.LiveHeartRate;
  // Optionally fall back to Pebble's automatic schedule while asleep; off by
  // default because frequent HR users may want sleep heart-rate data.
  if (frequent && s_settings.RelaxHrSleep && s_is_sleeping) frequent = false;
  health_service_set_heart_rate_sample_period(frequent ? 60 : 0);
}

static void prv_update_sleep_state(void) {
  bool sleeping;
  if (s_settings.SleepDetectMode == 1) {
    // Fixed window: for watches not worn during sleep, where Health cannot
    // detect it.
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    sleeping = (t->tm_hour >= 23 || t->tm_hour < 7);
  } else {
    HealthActivityMask activities = health_service_peek_current_activities();
    sleeping = (activities & (HealthActivitySleep | HealthActivityRestfulSleep)) != 0;
  }
  if (sleeping != s_is_sleeping) {
    s_is_sleeping = sleeping;
    prv_apply_hr_period();
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
  // (grey + age tag when at least one scheduled refresh has been missed)
  char temp[16];
  if (s_weather.valid) snprintf(temp, sizeof(temp), "%d°", s_weather.temp_now);
  else snprintf(temp, sizeof(temp), "--°");
  bool stale = s_settings.StaleBadge && s_weather.valid && prv_weather_is_stale();
  prv_draw_text_outline(ctx, temp, s_font_big, GRect(100, 18, 92, 30),
                        stale ? GColorLightGray : GColorWhite, GColorBlack, GTextAlignmentRight, 2);
  if (stale) {
    char age_text[12];
    prv_format_weather_age(age_text, sizeof(age_text));
    GSize age_sz = graphics_text_layout_get_content_size(age_text, s_font_small, GRect(0, 0, 80, 20),
                                                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(88, 22, age_sz.w + 9, 13), 2, GCornersAll);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, age_text, s_font_small, GRect(92, 21, age_sz.w + 2, 15),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  if (s_settings.ShowFeelsLike && s_weather.valid) {
    // apparent temperature, small tag under the bubble numerals
    // (no degree sign: Lilita's ° rasterises as a blob below ~20px)
    char feels[16];
    snprintf(feels, sizeof(feels), "FEELS %d", s_weather.feels_like);
    prv_draw_text_outline(ctx, feels, s_font_small, GRect(100, 44, 92, 16),
                          GColorWhite, GColorBlack, GTextAlignmentRight, 1);
  }

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



// ---------------------------------------------------------------- shake overlay

static void prv_draw_overlay_frame(GContext *ctx, GRect bounds, const char *title) {
  graphics_context_set_fill_color(ctx, COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_round_rect(ctx, grect_inset(bounds, GEdgeInsets(2)), 5);

  GSize sz = graphics_text_layout_get_content_size(title, s_font_small, GRect(0, 0, 160, 20),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
  int box_w = sz.w + 14;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect((bounds.size.w - box_w) / 2, 7, box_w, 15), 2, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title, s_font_small, GRect((bounds.size.w - box_w) / 2, 6, box_w, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static const char *prv_compass_label(int deg) {
  static const char *dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return dirs[((deg % 360 + 360 + 22) / 45) & 7];
}

static void prv_format_clock(char *buf, size_t size, int minutes) {
  int h = minutes / 60, m = minutes % 60;
  if (prv_use_24h()) {
    snprintf(buf, size, "%d:%02d", h, m);
  } else {
    int h12 = h % 12; if (h12 == 0) h12 = 12;
    snprintf(buf, size, "%d:%02d%s", h12, m, h < 12 ? "A" : "P");
  }
}

static void prv_draw_detail_row(GContext *ctx, int y, const char *label, const char *value) {
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, label, s_font_small, GRect(14, y + 2, 80, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, value, s_font_med, GRect(70, y, 116, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void prv_draw_overlay_details(GContext *ctx, GRect bounds) {
  prv_draw_overlay_frame(ctx, bounds, "WEATHER DETAILS");
  const char *wind_unit = s_settings.TemperatureUnit ? "MPH" : "KM/H";
  char value[32];

  snprintf(value, sizeof(value), "%d", s_weather.feels_like);
  prv_draw_detail_row(ctx, 28, "FEELS", value);
  snprintf(value, sizeof(value), "%d %s %s", s_weather.wind, wind_unit, prv_compass_label(s_weather.wind_dir));
  prv_draw_detail_row(ctx, 52, "WIND", value);
  snprintf(value, sizeof(value), "%d%%", s_weather.humidity);
  prv_draw_detail_row(ctx, 76, "HUMID", value);
  int rain_sum = 0;
  for (int i = 0; i < 24; i++) rain_sum += s_weather.graph_rain[i];
  snprintf(value, sizeof(value), "%d.%d MM", rain_sum / 10, rain_sum % 10);
  prv_draw_detail_row(ctx, 100, "RAIN 24H", value);
  if (s_weather.sunrise_min > 0 && s_weather.sunset_min > 0) {
    char rise[10], set[10];
    prv_format_clock(rise, sizeof(rise), s_weather.sunrise_min);
    prv_format_clock(set, sizeof(set), s_weather.sunset_min);
    snprintf(value, sizeof(value), "%s-%s", rise, set);
    prv_draw_detail_row(ctx, 124, "SUN", value);
  }

  // four-day strip: today, tomorrow, +2, +3
  static const char *wd[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  const char *labels[4] = {"TODAY", "TMRW", wd[(t->tm_wday + 2) % 7], wd[(t->tm_wday + 3) % 7]};
  int mins[4] = {s_weather.today_min, s_weather.tomorrow_min, s_weather.day_min[0], s_weather.day_min[1]};
  int maxs[4] = {s_weather.today_max, s_weather.tomorrow_max, s_weather.day_max[0], s_weather.day_max[1]};
  int codes[4] = {s_weather.today_code, s_weather.tomorrow_code, s_weather.day_code[0], s_weather.day_code[1]};

  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_line(ctx, GPoint(10, 152), GPoint(190, 152));
  for (int i = 0; i < 4; i++) {
    int cx = 25 + i * 50;
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, labels[i], s_font_small, GRect(cx - 24, 155, 48, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    prv_draw_bitmap_centered(ctx, prv_icon_for_code(codes[i]), GRect(cx - 11, 172, 22, 22));
    char mm[16];
    snprintf(mm, sizeof(mm), "%d/%d", mins[i], maxs[i]);
    graphics_draw_text(ctx, mm, s_font_small, GRect(cx - 24, 197, 48, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void prv_draw_overlay_graph(GContext *ctx, GRect bounds) {
  prv_draw_overlay_frame(ctx, bounds, "NEXT 24H");
  GRect chart = GRect(16, 44, 168, 88);

  int tmin = 127, tmax = -128, rmax = 20;  // rmax floor = 2.0mm so drizzle stays small
  int wmax = 1;
  for (int i = 0; i < 24; i++) {
    if (s_weather.graph_temp[i] < tmin) tmin = s_weather.graph_temp[i];
    if (s_weather.graph_temp[i] > tmax) tmax = s_weather.graph_temp[i];
    if (s_weather.graph_rain[i] > rmax) rmax = s_weather.graph_rain[i];
    if (s_weather.graph_wind[i] > wmax) wmax = s_weather.graph_wind[i];
  }
  if (tmax <= tmin) tmax = tmin + 1;

  // rain bars, blue, bottom-anchored
  graphics_context_set_fill_color(ctx, GColorFromHEX(0x0055FF));
  for (int i = 0; i < 24; i++) {
    int h = chart.size.h * s_weather.graph_rain[i] / rmax;
    if (h > chart.size.h) h = chart.size.h;
    if (h > 0) {
      graphics_fill_rect(ctx, GRect(chart.origin.x + i * 7, chart.origin.y + chart.size.h - h, 6, h),
                         0, GCornerNone);
    }
  }

  // temperature polyline, red
  graphics_context_set_stroke_color(ctx, GColorFromHEX(0xFF0000));
  graphics_context_set_stroke_width(ctx, 3);
  GPoint prev = GPointZero;
  for (int i = 0; i < 24; i++) {
    int x = chart.origin.x + i * 7 + 3;
    int y = chart.origin.y + chart.size.h - 5 -
            (chart.size.h - 10) * (s_weather.graph_temp[i] - tmin) / (tmax - tmin);
    GPoint p = GPoint(x, y);
    if (i > 0) graphics_draw_line(ctx, prev, p);
    prev = p;
  }

  // chart frame + scale labels
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_rect(ctx, grect_inset(chart, GEdgeInsets(-2)));
  char label[20];
  graphics_context_set_text_color(ctx, GColorBlack);
  snprintf(label, sizeof(label), "%d-%d", tmin, tmax);
  graphics_draw_text(ctx, label, s_font_small, GRect(16, 24, 90, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  bool any_rain = false;
  for (int i = 0; i < 24; i++) {
    if (s_weather.graph_rain[i] > 0) { any_rain = true; break; }
  }
  if (any_rain) {  // scale label only when there are bars to read against it
    snprintf(label, sizeof(label), "%d.%dMM", rmax / 10, rmax % 10);
    graphics_draw_text(ctx, label, s_font_small, GRect(94, 24, 90, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  }

  // hour ticks every 6h
  for (int i = 0; i < 24; i += 6) {
    snprintf(label, sizeof(label), "%02d", (s_weather.graph_start_hour + i) % 24);
    graphics_draw_text(ctx, label, s_font_small, GRect(chart.origin.x + i * 7 - 6, 136, 26, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  // wind, grey polyline strip
  GRect wchart = GRect(16, 178, 168, 36);
  graphics_context_set_stroke_color(ctx, GColorFromHEX(0x555555));
  graphics_context_set_stroke_width(ctx, 2);
  for (int i = 1; i < 24; i++) {
    GPoint a = GPoint(wchart.origin.x + (i - 1) * 7 + 3,
                      wchart.origin.y + wchart.size.h - 2 -
                      (wchart.size.h - 4) * s_weather.graph_wind[i - 1] / wmax);
    GPoint b = GPoint(wchart.origin.x + i * 7 + 3,
                      wchart.origin.y + wchart.size.h - 2 -
                      (wchart.size.h - 4) * s_weather.graph_wind[i] / wmax);
    graphics_draw_line(ctx, a, b);
  }
  snprintf(label, sizeof(label), "WIND MAX %d %s", wmax, s_settings.TemperatureUnit ? "MPH" : "KM/H");
  graphics_draw_text(ctx, label, s_font_small, GRect(16, 158, 168, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_overlay_page == 1 && s_weather.valid) {
    prv_draw_overlay_details(ctx, bounds);
    return;
  }
  if (s_overlay_page == 2 && s_weather.valid) {
    prv_draw_overlay_graph(ctx, bounds);
    return;
  }

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

  // While a timeline Quick View peek covers the bottom strip, the forecast
  // panels are underneath it — skip them (everything else stays visible).
  GRect unobstructed = layer_get_unobstructed_bounds(layer);
  if (unobstructed.size.h < bounds.size.h) return;

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
    prv_update_sleep_state();
    layer_mark_dirty(s_canvas_layer);
    prv_request_weather_if_due();
  }
}

static void prv_overlay_hide_timer(void *context) {
  (void)context;
  s_overlay_timer = NULL;
  s_overlay_page = 0;
  layer_mark_dirty(s_canvas_layer);
}

static void prv_accel_tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis;
  (void)direction;
  if (s_settings.ShakeAction == 0 || !s_weather.valid) return;
  uint8_t max_page = s_settings.ShakeAction;  // 1 = details only, 2 = + graph
  s_overlay_page = (s_overlay_page >= max_page) ? 0 : (uint8_t)(s_overlay_page + 1);
  if (s_overlay_timer) app_timer_cancel(s_overlay_timer);
  s_overlay_timer = s_overlay_page
      ? app_timer_register(OVERLAY_HIDE_MS, prv_overlay_hide_timer, NULL) : NULL;
  layer_mark_dirty(s_canvas_layer);
}

static void prv_unobstructed_did_change(void *context) {
  (void)context;
  layer_mark_dirty(s_canvas_layer);
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

static void prv_copy_tuple_bytes(uint8_t *dest, size_t len, Tuple *tuple) {
  if (tuple && tuple->type == TUPLE_BYTE_ARRAY && tuple->length == len) {
    memcpy(dest, tuple->value->data, len);
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
  t = dict_find(iterator, MESSAGE_KEY_WEATHER_FEELS);
  if (t) s_weather.feels_like = prv_tuple_int(t, s_weather.temp_now);
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
  s_weather.wind = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_WIND), s_weather.wind);
  s_weather.wind_dir = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_WIND_DIR), s_weather.wind_dir);
  s_weather.humidity = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_HUMIDITY), s_weather.humidity);
  s_weather.sunrise_min = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_SUNRISE), s_weather.sunrise_min);
  s_weather.sunset_min = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_SUNSET), s_weather.sunset_min);
  s_weather.day_min[0] = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_DAY2_MIN), s_weather.day_min[0]);
  s_weather.day_max[0] = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_DAY2_MAX), s_weather.day_max[0]);
  s_weather.day_code[0] = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_DAY2_CODE), s_weather.day_code[0]);
  s_weather.day_min[1] = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_DAY3_MIN), s_weather.day_min[1]);
  s_weather.day_max[1] = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_DAY3_MAX), s_weather.day_max[1]);
  s_weather.day_code[1] = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_DAY3_CODE), s_weather.day_code[1]);
  prv_copy_tuple_bytes((uint8_t *)s_weather.graph_temp, sizeof(s_weather.graph_temp),
                       dict_find(iterator, MESSAGE_KEY_GRAPH_TEMP));
  prv_copy_tuple_bytes(s_weather.graph_rain, sizeof(s_weather.graph_rain),
                       dict_find(iterator, MESSAGE_KEY_GRAPH_RAIN));
  prv_copy_tuple_bytes(s_weather.graph_wind, sizeof(s_weather.graph_wind),
                       dict_find(iterator, MESSAGE_KEY_GRAPH_WIND));
  s_weather.graph_start_hour = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_GRAPH_START_HOUR), s_weather.graph_start_hour);

  if (got_weather) {
    int32_t now = (int32_t)time(NULL);
    int32_t raw_fetched_at = prv_tuple_int(dict_find(iterator, MESSAGE_KEY_WEATHER_FETCHED_AT), now);
    // The phone stamps payloads with its own clock; clamp future timestamps
    // instead of rejecting them so clock skew cannot blank a fresh forecast.
    s_weather.fetched_at = (raw_fetched_at <= 0 || raw_fetched_at > now) ? now : raw_fetched_at;
    // A delivered payload is the best data available (the phone already
    // enforces its 12h staleness policy) — always display it. Persist only
    // when the RAW timestamp is plausible, so a phone clock jumped far into
    // the future cannot launder ancient data into a fresh boot cache.
    s_weather.valid = true;
    if (prv_fetched_age_ok(raw_fetched_at)) {
      prv_save_weather_cache();
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
  Tuple *feels_like = dict_find(iterator, MESSAGE_KEY_ShowFeelsLike);
  Tuple *stale_badge = dict_find(iterator, MESSAGE_KEY_StaleBadge);
  Tuple *night_theme = dict_find(iterator, MESSAGE_KEY_NightTheme);
  Tuple *shake_action = dict_find(iterator, MESSAGE_KEY_ShakeAction);
  Tuple *sleep_fetch = dict_find(iterator, MESSAGE_KEY_SleepFetchMode);
  Tuple *morning_fetch = dict_find(iterator, MESSAGE_KEY_MorningFetchMin);
  Tuple *sleep_detect = dict_find(iterator, MESSAGE_KEY_SleepDetectMode);
  Tuple *relax_hr = dict_find(iterator, MESSAGE_KEY_RelaxHrSleep);

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
  if (feels_like) { s_settings.ShowFeelsLike = prv_tuple_int(feels_like, s_settings.ShowFeelsLike) ? true : false; changed = true; }
  if (stale_badge) { s_settings.StaleBadge = prv_tuple_int(stale_badge, s_settings.StaleBadge) ? true : false; changed = true; }
  if (night_theme) { s_settings.NightTheme = prv_tuple_int(night_theme, s_settings.NightTheme); changed = true; }
  if (shake_action) { s_settings.ShakeAction = prv_tuple_int(shake_action, s_settings.ShakeAction); changed = true; }
  if (sleep_fetch) { s_settings.SleepFetchMode = prv_tuple_int(sleep_fetch, s_settings.SleepFetchMode); changed = true; }
  if (morning_fetch) { s_settings.MorningFetchMin = prv_tuple_int(morning_fetch, s_settings.MorningFetchMin); changed = true; }
  if (sleep_detect) { s_settings.SleepDetectMode = prv_tuple_int(sleep_detect, s_settings.SleepDetectMode); changed = true; }
  if (relax_hr) { s_settings.RelaxHrSleep = prv_tuple_int(relax_hr, s_settings.RelaxHrSleep) ? true : false; changed = true; }

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

static bool prv_in_morning_fetch_window(void) {
  if (s_settings.MorningFetchMin == 0) return false;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int now_min = t->tm_hour * 60 + t->tm_min;
  // A 15-minute window paired with the 15-minute failure retry gives the
  // daily fetch one or two attempts without ever looping.
  return now_min >= s_settings.MorningFetchMin && now_min < s_settings.MorningFetchMin + 15;
}

static bool prv_weather_due_for_refresh(void) {
#if DEMO_MODE
  return false;
#endif
  int32_t now = (int32_t)time(NULL);
  int32_t refresh_seconds = s_settings.RefreshMinutes * 60;
  bool morning_override = false;

  if (s_is_sleeping) {
    if (prv_in_morning_fetch_window() && prv_weather_age_seconds() > 60 * 60) {
      // The configured wake-up fetch runs even in paused mode.
      morning_override = true;
    } else if (s_settings.SleepFetchMode == 2) {
      return false;  // paused while sleeping
    } else if (s_settings.SleepFetchMode == 1) {
      if (refresh_seconds < SLEEP_REDUCED_REFRESH_MINUTES * 60) {
        refresh_seconds = SLEEP_REDUCED_REFRESH_MINUTES * 60;
      }
    }
  }

  int32_t retry_seconds = s_weather.valid ? refresh_seconds : (INITIAL_RETRY_MINUTES * 60);
  if (morning_override && retry_seconds > INITIAL_RETRY_MINUTES * 60) {
    retry_seconds = INITIAL_RETRY_MINUTES * 60;
  }

  if (s_last_weather_request_at > 0 && now - s_last_weather_request_at < retry_seconds) {
    return false;
  }
  if (morning_override) {
    return true;
  }
  if (!s_weather.valid) {
    return true;
  }
  if (s_weather.fetched_at <= 0) {
    return true;
  }
  return now - s_weather.fetched_at >= refresh_seconds;
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

static bool prv_is_night(void) {
  if (!s_settings.NightTheme || !s_weather.valid) return false;
  if (s_weather.sunrise_min <= 0 || s_weather.sunset_min <= 0) return false;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int now_min = t->tm_hour * 60 + t->tm_min;
  return now_min < s_weather.sunrise_min || now_min >= s_weather.sunset_min;
}

static uint32_t prv_base_resource_for_state(void) {
  int divider = s_settings.DividerColor;
  if (prv_is_night()) {
    divider = (s_settings.NightTheme == 2) ? DIVIDER_GREY : DIVIDER_BLACK;
  }
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
  s_weather.feels_like = DEMO_TEMP_NOW - 2;
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
  // Overlay data derived from the scenario values so store screenshots can
  // show the shake pages without extra demo.h fields.
  s_weather.today_min = temps[0] - 2;
  s_weather.today_max = temps[3] + 2;
  s_weather.today_code = DEMO_CODE_NOW;
  s_weather.wind = 14;
  s_weather.wind_dir = 315;
  s_weather.humidity = 55;
  s_weather.sunrise_min = 5 * 60 + 8;
  s_weather.sunset_min = 21 * 60 + 14;
  for (int d = 0; d < 2; d++) {
    s_weather.day_min[d] = DEMO_TMRW_MIN - 1 - d;
    s_weather.day_max[d] = DEMO_TMRW_MAX - 2 * d;
    s_weather.day_code[d] = d ? 61 : DEMO_TMRW_CODE;
  }
  for (int h = 0; h < 24; h++) {
    s_weather.graph_temp[h] = (int8_t)(DEMO_TEMP_NOW - 4 + (h < 12 ? h : 24 - h) * 2 / 3);
    s_weather.graph_rain[h] = (h >= 14 && h <= 19) ? (uint8_t)((h - 13) * 8) : 0;
    s_weather.graph_wind[h] = (uint8_t)(8 + (h % 6) * 3);
  }
  s_weather.graph_start_hour = 8;
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
  accel_tap_service_subscribe(prv_accel_tap_handler);
  unobstructed_area_service_subscribe((UnobstructedAreaHandlers) {
    .did_change = prv_unobstructed_did_change,
  }, NULL);

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
  accel_tap_service_unsubscribe();
  unobstructed_area_service_unsubscribe();
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
