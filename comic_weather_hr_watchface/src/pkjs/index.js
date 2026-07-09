var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);
var MAX_STALE_CACHE_MS = 12 * 60 * 60 * 1000;

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

function nowSeconds() {
  return Math.floor(Date.now() / 1000);
}

function clampRefreshMinutes(value) {
  var n = Number(value || 60);
  if (n !== 15 && n !== 30 && n !== 60 && n !== 120 && n !== 180) {
    return 60;
  }
  return n;
}

function storageGet(key) {
  try {
    if (typeof localStorage === 'undefined') return null;
    var value = localStorage.getItem(key);
    return value ? JSON.parse(value) : null;
  } catch (e) {
    console.log('Storage read failed for ' + key + ': ' + e.message);
    return null;
  }
}

function storageSet(key, value) {
  try {
    if (typeof localStorage === 'undefined') return;
    localStorage.setItem(key, JSON.stringify(value));
  } catch (e) {
    console.log('Storage write failed for ' + key + ': ' + e.message);
  }
}

function weatherCacheKey(useFahrenheit) {
  return 'comic-weather-cache-v2-' + (useFahrenheit ? 'f' : 'c');
}

function locationCacheKey() {
  return 'comic-weather-location-v1';
}

function readWeatherCache(useFahrenheit) {
  var cached = storageGet(weatherCacheKey(useFahrenheit));
  if (!cached || !cached.payload || !cached.fetchedAt) return null;
  if (!cached.payload.WEATHER_FETCHED_AT) {
    cached.payload.WEATHER_FETCHED_AT = cached.fetchedAt;
  }
  return cached;
}

function writeWeatherCache(useFahrenheit, payload) {
  storageSet(weatherCacheKey(useFahrenheit), {
    fetchedAt: payload.WEATHER_FETCHED_AT || nowSeconds(),
    payload: payload
  });
}

function readLastLocation() {
  var cached = storageGet(locationCacheKey());
  if (!cached || !cached.coords || !cached.storedAt) return null;
  // Last known coordinates are only a fallback for temporary geolocation
  // failures. Keep the window finite so travel does not pin weather forever.
  if (Date.now() - cached.storedAt > 24 * 60 * 60 * 1000) return null;
  return cached.coords;
}

function writeLastLocation(coords) {
  storageSet(locationCacheKey(), {
    storedAt: Date.now(),
    coords: {
      latitude: coords.latitude,
      longitude: coords.longitude
    }
  });
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

function sendPayload(payload, label) {
  Pebble.sendAppMessage(payload,
    function () { console.log(label + ' sent'); },
    function (e) { console.log('Failed to send ' + label + ': ' + JSON.stringify(e)); }
  );
}

function sendCachedWeather(useFahrenheit, maxAgeMs, allowStale, reason) {
  var cached = readWeatherCache(useFahrenheit);
  if (!cached) return false;

  var ageMs = Date.now() - cached.fetchedAt * 1000;
  if (ageMs <= maxAgeMs || (allowStale && ageMs <= MAX_STALE_CACHE_MS)) {
    sendPayload(cached.payload, reason || 'cached weather');
    return true;
  }
  return false;
}

function sendWeatherError(reason) {
  console.log('Weather unavailable: ' + reason);
  sendPayload({ WEATHER_ERROR: 1 }, 'weather error');
}

function sendWeather(json, useFahrenheit) {
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
  payload.WEATHER_FETCHED_AT = nowSeconds();

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

  writeWeatherCache(useFahrenheit, payload);
  sendPayload(payload, 'weather forecast');
}

function fetchWeatherAt(coords, useFahrenheit, errorCallback) {
  var unit = useFahrenheit ? 'fahrenheit' : 'celsius';
  var url = 'https://api.open-meteo.com/v1/forecast?' +
    'latitude=' + encodeURIComponent(coords.latitude) +
    '&longitude=' + encodeURIComponent(coords.longitude) +
    '&current=temperature_2m,weather_code' +
    '&hourly=temperature_2m,weather_code,precipitation_probability' +
    '&daily=temperature_2m_min,temperature_2m_max,weather_code,precipitation_probability_max' +
    '&forecast_days=2' +
    '&timezone=auto' +
    '&temperature_unit=' + unit;

  xhrRequest(url, 'GET', function (responseText) {
    try {
      sendWeather(JSON.parse(responseText), useFahrenheit);
    } catch (e) {
      errorCallback('parse error: ' + e.message);
    }
  }, errorCallback);
}

function getWeather(useFahrenheit, refreshMinutes) {
  var refresh = clampRefreshMinutes(refreshMinutes);
  var cacheMaxAge = refresh * 60 * 1000;

  // Fresh cached data avoids both geolocation and network on watchface restarts
  // or repeated requests inside the selected refresh window.
  if (sendCachedWeather(useFahrenheit, cacheMaxAge, false, 'fresh cached weather')) {
    return;
  }

  function fallback(reason) {
    if (!sendCachedWeather(useFahrenheit, cacheMaxAge, true, 'stale cached weather')) {
      sendWeatherError(reason);
    }
  }

  navigator.geolocation.getCurrentPosition(
    function locationSuccess(pos) {
      writeLastLocation(pos.coords);
      fetchWeatherAt(pos.coords, useFahrenheit, function (err) {
        fallback('weather request failed: ' + err);
      });
    },
    function locationError(err) {
      var lastLocation = readLastLocation();
      if (lastLocation) {
        console.log('Location failed; retrying with last known coordinates');
        fetchWeatherAt(lastLocation, useFahrenheit, function (fetchErr) {
          fallback('last-location weather request failed: ' + fetchErr);
        });
      } else {
        fallback('location error: ' + JSON.stringify(err));
      }
    },
    { timeout: 20000, maximumAge: cacheMaxAge }
  );
}

Pebble.addEventListener('ready', function () {
  console.log('Comic Weather HR PKJS ready');
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.REQUEST_WEATHER) {
    var useFahrenheit = Number(e.payload.TemperatureUnit || 0) === 1;
    getWeather(useFahrenheit, e.payload.RefreshMinutes);
  }
});
