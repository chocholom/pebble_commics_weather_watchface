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
        "defaultValue": 30,
        "label": "Weather refresh",
        "options": [
          { "label": "Every 15 minutes", "value": 15 },
          { "label": "Every 30 minutes", "value": 30 },
          { "label": "Every 60 minutes", "value": 60 }
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
          { "label": "12 hour", "value": 2 }
        ]
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
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
