module.exports = [
  {
    "type": "heading",
    "defaultValue": "Comic Weather HR"
  },
  {
    "type": "text",
    "defaultValue": "Hand-drawn comic-style watchface with weather forecast and heart rate."
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Forecast"
      },
      {
        "type": "select",
        "messageKey": "ForecastMode",
        "defaultValue": 0,
        "label": "Forecast layout",
        "options": [
          { "label": "Full: 4h + today + tomorrow", "value": 0 },
          { "label": "Next 4 hours only", "value": 1 },
          { "label": "Today + tomorrow only", "value": 2 },
          { "label": "Current weather only", "value": 3 }
        ]
      },
      {
        "type": "select",
        "messageKey": "TemperatureUnit",
        "defaultValue": 0,
        "label": "Temperature units",
        "options": [
          { "label": "Celsius", "value": 0 },
          { "label": "Fahrenheit", "value": 1 }
        ]
      },
      {
        "type": "select",
        "messageKey": "RefreshMinutes",
        "label": "Weather refresh",
        "defaultValue": 60,
        "options": [
          { "label": "Every 15 minutes", "value": 15 },
          { "label": "Every 30 minutes", "value": 30 },
          { "label": "Every 60 minutes", "value": 60 },
          { "label": "Every 2 hours", "value": 120 },
          { "label": "Every 3 hours", "value": 180 }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "ShowFeelsLike",
        "label": "Show feels-like temperature",
        "defaultValue": false,
        "description": "Adds a small FEELS tag with the apparent temperature under the current temperature."
      },
      {
        "type": "toggle",
        "messageKey": "RainFill",
        "label": "Blue rain-chance fill on hourly icons",
        "defaultValue": true,
        "description": "Fills each hourly icon with blue from the bottom, proportional to the rain probability."
      },
      {
        "type": "toggle",
        "messageKey": "StaleBadge",
        "label": "Warn when the forecast is stale",
        "defaultValue": true,
        "description": "Greys the current temperature and shows its age once a scheduled refresh has been missed."
      },
      {
        "type": "select",
        "messageKey": "ShakeAction",
        "label": "Shake to reveal",
        "defaultValue": 2,
        "description": "Flick your wrist to show extra weather pages; they hide again after a few seconds.",
        "options": [
          { "label": "Off", "value": 0 },
          { "label": "Details page", "value": 1 },
          { "label": "Details + 24h graph", "value": 2 }
        ]
      },
      {
        "type": "select",
        "messageKey": "HotThresholdC",
        "defaultValue": 30,
        "label": "Hot tomorrow color",
        "description": "Threshold is always set in Celsius, even when temperatures are displayed in Fahrenheit.",
        "options": [
          { "label": "25 C / 77 F", "value": 25 },
          { "label": "28 C / 82 F", "value": 28 },
          { "label": "30 C / 86 F", "value": 30 },
          { "label": "32 C / 90 F", "value": 32 },
          { "label": "35 C / 95 F", "value": 35 }
        ]
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Display"
      },
      {
        "type": "select",
        "messageKey": "TimeFormat",
        "defaultValue": 0,
        "label": "Time format",
        "options": [
          { "label": "System default", "value": 0 },
          { "label": "24 hour", "value": 1 },
          { "label": "12 hour", "value": 2 },
          { "label": "12 hour + AM/PM", "value": 3 }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "ShowStatusChips",
        "label": "Show steps / heart rate / battery bar",
        "defaultValue": true
      },
      {
        "type": "toggle",
        "messageKey": "ShowHeartRate",
        "label": "Show heart rate bubble",
        "defaultValue": true
      },
      {
        "type": "toggle",
        "messageKey": "LiveHeartRate",
        "label": "Ask for more frequent HR samples",
        "defaultValue": false,
        "description": "Battery-friendly mode uses Pebble's automatic HR schedule. Frequent mode requests a 60s sample period."
      },
      {
        "type": "select",
        "messageKey": "DividerColor",
        "defaultValue": 0,
        "label": "Panel divider color",
        "options": [
          { "label": "White", "value": 0 },
          { "label": "Grey", "value": 1 },
          { "label": "Black", "value": 2 }
        ]
      },
      {
        "type": "select",
        "messageKey": "NightTheme",
        "defaultValue": 0,
        "label": "Night theme",
        "description": "Automatically switches the divider color between sunset and sunrise.",
        "options": [
          { "label": "Off", "value": 0 },
          { "label": "Black at night", "value": 1 },
          { "label": "Grey at night", "value": 2 }
        ]
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Battery saving"
      },
      {
        "type": "select",
        "messageKey": "SleepFetchMode",
        "defaultValue": 1,
        "label": "Weather updates while you sleep",
        "description": "Fewer overnight fetches save watch and phone battery. Data refreshes normally once you are awake.",
        "options": [
          { "label": "Normal schedule", "value": 0 },
          { "label": "Reduced (every 4 hours)", "value": 1 },
          { "label": "Paused", "value": 2 }
        ]
      },
      {
        "type": "select",
        "messageKey": "MorningFetchMin",
        "defaultValue": 0,
        "label": "Daily morning fetch",
        "description": "Fetches once at this time even when sleeping updates are reduced or paused, so the forecast is fresh when you wake up.",
        "options": [
          { "label": "Off", "value": 0 },
          { "label": "5:00", "value": 300 },
          { "label": "5:30", "value": 330 },
          { "label": "6:00", "value": 360 },
          { "label": "6:30", "value": 390 },
          { "label": "7:00", "value": 420 },
          { "label": "7:30", "value": 450 },
          { "label": "8:00", "value": 480 }
        ]
      },
      {
        "type": "select",
        "messageKey": "SleepDetectMode",
        "defaultValue": 0,
        "label": "How sleep is detected",
        "description": "Sleep detection uses Pebble Health and only works while the watch is on your wrist. Choose fixed hours if you take the watch off for the night.",
        "options": [
          { "label": "On the wrist (Pebble Health)", "value": 0 },
          { "label": "Fixed hours (23:00-07:00)", "value": 1 }
        ]
      },
      {
        "type": "toggle",
        "messageKey": "RelaxHrSleep",
        "label": "Relax frequent HR sampling while asleep",
        "defaultValue": false,
        "description": "Only applies when frequent HR samples are enabled above. Leave off if you want frequent heart-rate data during sleep."
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
