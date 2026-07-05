var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

function xhrRequest(url, type, callback, errorCallback) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      callback(this.responseText);
    } else if (errorCallback) {
      errorCallback('HTTP ' + xhr.status);
    }
  };
  xhr.onerror = function () {
    if (errorCallback) {
      errorCallback('network error');
    }
  };
  xhr.open(type, url);
  xhr.send();
}

function pad2(n) {
  return (n < 10 ? '0' : '') + n;
}

function hourLabel(isoTime) {
  var d = new Date(isoTime);
  if (isNaN(d.getTime())) {
    // Open-Meteo returns local time without timezone, e.g. 2026-06-28T14:00.
    // This fallback is enough for a compact label.
    var m = /T(\d\d):/.exec(isoTime || '');
    return m ? m[1] : '--';
  }
  return pad2(d.getHours());
}

function findCurrentHourIndex(hourlyTimes) {
  var now = new Date();
  var best = 0;
  for (var i = 0; i < hourlyTimes.length; i++) {
    var d = new Date(hourlyTimes[i]);
    if (!isNaN(d.getTime()) && d.getTime() >= now.getTime() - (30 * 60 * 1000)) {
      return i;
    }
    best = i;
  }
  return best;
}

function sendWeather(json) {
  var hourly = json.hourly || {};
  var daily = json.daily || {};
  var current = json.current || {};
  var times = hourly.time || [];
  var start = findCurrentHourIndex(times);
  var payload = {};

  payload.WEATHER_TEMP_NOW = Math.round(current.temperature_2m || 0);
  payload.WEATHER_CODE_NOW = Number(current.weather_code || 0);

  var now = new Date();
  payload.WEATHER_UPDATED = pad2(now.getHours()) + ':' + pad2(now.getMinutes());

  for (var i = 0; i < 4; i++) {
    var idx = Math.min(start + i, Math.max(0, times.length - 1));
    payload['HOUR' + i + '_TEMP'] = Math.round((hourly.temperature_2m || [])[idx] || 0);
    payload['HOUR' + i + '_CODE'] = Number((hourly.weather_code || [])[idx] || 0);
    payload['HOUR' + i + '_POP'] = Number((hourly.precipitation_probability || [])[idx] || 0);
    payload['HOUR' + i + '_TIME'] = hourLabel(times[idx] || '');
  }

  payload.TODAY_MIN = Math.round((daily.temperature_2m_min || [])[0] || 0);
  payload.TODAY_MAX = Math.round((daily.temperature_2m_max || [])[0] || 0);
  payload.TODAY_CODE = Number((daily.weather_code || [])[0] || 0);
  payload.TODAY_POP = Number((daily.precipitation_probability_max || [])[0] || 0);
  payload.TOMORROW_MIN = Math.round((daily.temperature_2m_min || [])[1] || payload.TODAY_MIN);
  payload.TOMORROW_MAX = Math.round((daily.temperature_2m_max || [])[1] || payload.TODAY_MAX);
  payload.TOMORROW_CODE = Number((daily.weather_code || [])[1] || payload.TODAY_CODE);
  payload.TOMORROW_POP = Number((daily.precipitation_probability_max || [])[1] || payload.TODAY_POP);

  Pebble.sendAppMessage(payload,
    function () { console.log('Weather forecast sent'); },
    function (e) { console.log('Failed to send weather forecast: ' + JSON.stringify(e)); }
  );
}

function getWeather(useFahrenheit) {
  navigator.geolocation.getCurrentPosition(
    function locationSuccess(pos) {
      var unit = useFahrenheit ? 'fahrenheit' : 'celsius';
      var url = 'https://api.open-meteo.com/v1/forecast?' +
        'latitude=' + encodeURIComponent(pos.coords.latitude) +
        '&longitude=' + encodeURIComponent(pos.coords.longitude) +
        '&current=temperature_2m,weather_code' +
        '&hourly=temperature_2m,weather_code,precipitation_probability' +
        '&daily=temperature_2m_min,temperature_2m_max,weather_code,precipitation_probability_max' +
        '&forecast_days=2' +
        '&timezone=auto' +
        '&temperature_unit=' + unit;

      xhrRequest(url, 'GET', function (responseText) {
        try {
          sendWeather(JSON.parse(responseText));
        } catch (e) {
          console.log('Failed to parse weather JSON: ' + e.message);
        }
      }, function (err) {
        console.log('Weather request failed: ' + err);
      });
    },
    function locationError(err) {
      console.log('Location error: ' + JSON.stringify(err));
    },
    { timeout: 15000, maximumAge: 10 * 60 * 1000 }
  );
}

Pebble.addEventListener('ready', function () {
  console.log('Comic Weather HR PKJS ready');
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.REQUEST_WEATHER) {
    var useFahrenheit = Number(e.payload.TemperatureUnit || 0) === 1;
    getWeather(useFahrenheit);
  }
});
