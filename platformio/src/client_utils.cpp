/* Client side utilities for esp32-weather-epd.
 * Copyright (C) 2022-2024  Luke Marzen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// built-in C++ libraries
#include <cstring>
#include <vector>

// arduino/esp32 libraries
#include <Arduino.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>

// additional libraries
#include <Adafruit_BusIO_Register.h>
#include <ArduinoJson.h>

// header files
#include "_locale.h"
#include "api_response.h"
#include "aqi.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "renderer.h"
#ifndef USE_HTTP
#include <WiFiClientSecure.h>
#endif

#ifdef USE_HTTP
static const uint16_t OWM_PORT = 80;
#else
static const uint16_t OWM_PORT = 443;
#endif

/* Power-on and connect WiFi.
 * Takes int parameter to store WiFi RSSI, or “Received Signal Strength
 * Indicator"
 *
 * Returns WiFi status.
 */
wl_status_t startWiFi(int &wifiRSSI)
{
  WiFi.mode(WIFI_STA);
  Serial.printf("%s '%s'", TXT_CONNECTING_TO, WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // timeout if WiFi does not connect in WIFI_TIMEOUT ms from now
  unsigned long timeout = millis() + WIFI_TIMEOUT;
  wl_status_t connection_status = WiFi.status();

  while ((connection_status != WL_CONNECTED) && (millis() < timeout))
  {
    Serial.print(".");
    delay(50);
    connection_status = WiFi.status();
  }
  Serial.println();

  if (connection_status == WL_CONNECTED)
  {
    wifiRSSI = WiFi.RSSI(); // get WiFi signal strength now, because the WiFi
                            // will be turned off to save power!
    Serial.println("IP: " + WiFi.localIP().toString());
  }
  else
  {
    Serial.printf("%s '%s' (status=%d)\n", TXT_COULD_NOT_CONNECT_TO, WIFI_SSID,
                  static_cast<int>(connection_status));
    // Diagnostic: the ESP32 is 2.4GHz-only and cannot see 5GHz SSIDs or APs on
    // channels its region disallows. List what is actually visible so the right
    // SSID/channel can be identified.
    int n = WiFi.scanNetworks();
    Serial.printf("[wifi] visible 2.4GHz networks: %d\n", n);
    for (int i = 0; i < n; ++i)
    {
      Serial.printf("  %2d) ch%-3d %4ddBm  %s\n", i, WiFi.channel(i),
                    WiFi.RSSI(i), WiFi.SSID(i).c_str());
    }
    WiFi.scanDelete();
  }
  return connection_status;
} // startWiFi

/* Disconnect and power-off WiFi.
 */
void killWiFi()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
} // killWiFi

/* Prints the local time to serial monitor.
 *
 * Returns true if getting local time was a success, otherwise false.
 */
bool printLocalTime(tm *timeInfo)
{
  int attempts = 0;
  while (!getLocalTime(timeInfo) && attempts++ < 3)
  {
    Serial.println(TXT_FAILED_TO_GET_TIME);
    return false;
  }
  Serial.println(timeInfo, "%A, %B %d, %Y %H:%M:%S");
  return true;
} // printLocalTime

/* Waits for NTP server time sync, adjusted for the time zone specified in
 * config.cpp.
 *
 * Returns true if time was set successfully, otherwise false.
 *
 * Note: Must be connected to WiFi to get time from NTP server.
 */
bool waitForSNTPSync(tm *timeInfo)
{
  // Wait for SNTP synchronization to complete
  unsigned long timeout = millis() + NTP_TIMEOUT;
  if ((sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) && (millis() < timeout))
  {
    Serial.print(TXT_WAITING_FOR_SNTP);
    delay(100); // ms
    while ((sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) && (millis() < timeout))
    {
      Serial.print(".");
      delay(100); // ms
    }
    Serial.println();
  }
  return printLocalTime(timeInfo);
} // waitForSNTPSync

/* Perform an HTTP GET request to OpenWeatherMap's "One Call" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_onecall.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
int getOWMonecall(WiFiClient &client, owm_resp_onecall_t &r)
#else
int getOWMonecall(WiFiClientSecure &client, owm_resp_onecall_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};
  String uri = "/data/" + OWM_ONECALL_VERSION + "/onecall?lat=" + LAT + "&lon=" + LON + "&lang=" + OWM_LANG + "&units=standard&exclude=minutely";
#if !DISPLAY_ALERTS
  // exclude alerts
  uri += ",alerts";
#endif

  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT + uri + "&appid={API key}";

  uri += "&appid=" + OWM_APIKEY;

  Serial.print(TXT_ATTEMPTING_HTTP_REQ);
  Serial.println(": " + sanitizedUri);
  Serial.println(": " + uri);

  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    wl_status_t connection_status = WiFi.status();
    if (connection_status != WL_CONNECTED)
    {
      // -512 offset distinguishes these errors from httpClient errors
      return -512 - static_cast<int>(connection_status);
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT); // default 5000ms
    http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);        // default 5000ms
    http.begin(client, OWM_ENDPOINT, OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeOneCall(http.getStream(), r);
      if (jsonErr)
      {
        // -256 offset distinguishes these errors from httpClient errors
        httpResponse = -256 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " " + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getOWMonecall

/* Perform an HTTP GET request to OpenWeatherMap's "Air Pollution" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_air_pollution.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
int getOWMairpollution(WiFiClient &client, owm_resp_air_pollution_t &r)
#else
int getOWMairpollution(WiFiClientSecure &client, owm_resp_air_pollution_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};

  // set start and end to appropriate values so that the last 24 hours of air
  // pollution history is returned. Unix, UTC.
  time_t now;
  int64_t end = time(&now);
  // minus 1 is important here, otherwise we could get an extra hour of history
  int64_t start = end - ((3600 * OWM_NUM_AIR_POLLUTION) - 1);
  char endStr[22];
  char startStr[22];
  sprintf(endStr, "%lld", end);
  sprintf(startStr, "%lld", start);
  String uri = "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON + "&start=" + startStr + "&end=" + endStr + "&appid=" + OWM_APIKEY;
  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT +
                        "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON + "&start=" + startStr + "&end=" + endStr + "&appid={API key}";

  Serial.print(TXT_ATTEMPTING_HTTP_REQ);
  Serial.println(": " + sanitizedUri);
  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    wl_status_t connection_status = WiFi.status();
    if (connection_status != WL_CONNECTED)
    {
      // -512 offset distinguishes these errors from httpClient errors
      return -512 - static_cast<int>(connection_status);
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT); // default 5000ms
    http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);        // default 5000ms
    http.begin(client, OWM_ENDPOINT, OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeAirQuality(http.getStream(), r);
      if (jsonErr)
      {
        // -256 offset to distinguishes these errors from httpClient errors
        httpResponse = -256 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " " + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getOWMairpollution

static void setWmoWeather(int code, bool day, owm_weather_t &weather)
{
  int id = 800; const char *main = "Clear"; const char *description = "clear sky"; const char *icon = "01";
  if (code == 1) { id = 801; main = "Clouds"; description = "mainly clear"; icon = "02"; }
  else if (code == 2) { id = 802; main = "Clouds"; description = "partly cloudy"; icon = "03"; }
  else if (code == 3) { id = 804; main = "Clouds"; description = "overcast clouds"; icon = "04"; }
  else if (code == 45 || code == 48) { id = 741; main = "Fog"; description = "fog"; icon = "50"; }
  else if (code >= 51 && code <= 55) { id = 300 + (code - 51) / 2; main = "Drizzle"; description = "drizzle"; icon = "09"; }
  else if (code == 56 || code == 57 || code == 66 || code == 67) { id = 511; main = "Rain"; description = "freezing rain"; icon = "13"; }
  else if (code >= 61 && code <= 65) { id = 500 + (code - 61) / 2; main = "Rain"; description = "rain"; icon = "10"; }
  else if (code >= 71 && code <= 77) { id = 600 + std::min(2, (code - 71) / 2); main = "Snow"; description = "snow"; icon = "13"; }
  else if (code >= 80 && code <= 82) { id = 520 + code - 80; main = "Rain"; description = "rain showers"; icon = "09"; }
  else if (code == 85 || code == 86) { id = code == 85 ? 620 : 622; main = "Snow"; description = "snow showers"; icon = "13"; }
  else if (code >= 95) { id = code == 95 ? 211 : 202; main = "Thunderstorm"; description = code == 95 ? "thunderstorm" : "thunderstorm with hail"; icon = "11"; }
  weather.id = id; weather.main = main; weather.description = description;
  weather.icon = String(icon) + (day ? "d" : "n");
}

template <typename Client>
static int openMeteoGet(Client &client, const char *host, const String &uri,
                        JsonDocument &doc)
{
  HTTPClient http;
  // ArduinoJson parses the response body stream directly. HTTP/1.0 makes
  // HTTPClient request a non-chunked response so chunk-size lines are not
  // mistaken for JSON input.
  http.useHTTP10(true);
  http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  Serial.println(String(TXT_ATTEMPTING_HTTP_REQ) + ": https://" + host + uri);
  if (!http.begin(client, host, 443, uri)) return -1;
  int code = http.GET();
  if (code == HTTP_CODE_OK)
  {
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (err) code = -256 - static_cast<int>(err.code());
  }
  http.end();
  client.stop();
  Serial.println("  " + String(code) + " " + getHttpResponsePhrase(code));
  return code;
}

template <typename Client>
static int fetchOpenMeteoForecast(Client &client, owm_resp_onecall_t &r)
{
  String uri = "/v1/forecast?latitude=" + LAT + "&longitude=" + LON +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
    "&hourly=temperature_2m,precipitation,precipitation_probability,uv_index"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,precipitation_sum,precipitation_probability_max"
    "&past_hours=6&forecast_hours=24&timeformat=unixtime&timezone=auto&wind_speed_unit=ms";
  JsonDocument doc;
  int code = openMeteoGet(client, "api.open-meteo.com", uri, doc);
  if (code != HTTP_CODE_OK) return code;

  r = {};
  r.lat = doc["latitude"] | LAT.toFloat(); r.lon = doc["longitude"] | LON.toFloat();
  r.timezone = String(doc["timezone"] | ""); r.timezone_offset = doc["utc_offset_seconds"] | 0;
  JsonObject current = doc["current"];
  r.current.dt = current["time"] | 0LL;
  r.current.temp = (current["temperature_2m"] | 0.0f) + 273.15f;
  r.current.feels_like = (current["apparent_temperature"] | 0.0f) + 273.15f;
  r.current.humidity = current["relative_humidity_2m"] | 0;
  r.current.pressure = current["pressure_msl"] | 0;
  r.current.clouds = current["cloud_cover"] | 0;
  r.current.wind_speed = current["wind_speed_10m"] | 0.0f;
  r.current.wind_gust = current["wind_gusts_10m"] | 0.0f;
  r.current.wind_deg = current["wind_direction_10m"] | 0;
  setWmoWeather(current["weather_code"] | 0, (current["is_day"] | 1) != 0, r.current.weather);

  JsonObject hourly = doc["hourly"];
  size_t hourlyCount = std::min<size_t>(OWM_NUM_HOURLY, hourly["time"].size());
  for (size_t i = 0; i < hourlyCount; ++i)
  {
    r.hourly[i].dt = hourly["time"][i] | 0LL;
    r.hourly[i].temp = (hourly["temperature_2m"][i] | 0.0f) + 273.15f;
    r.hourly[i].rain_1h = hourly["precipitation"][i] | 0.0f;
    r.hourly[i].pop = (hourly["precipitation_probability"][i] | 0.0f) / 100.0f;
    r.hourly[i].uvi = hourly["uv_index"][i] | 0.0f;
  }
  if (hourlyCount > 6) r.current.uvi = r.hourly[6].uvi;

  JsonObject daily = doc["daily"];
  size_t dailyCount = std::min<size_t>(OWM_NUM_DAILY, daily["time"].size());
  for (size_t i = 0; i < dailyCount; ++i)
  {
    r.daily[i].dt = daily["time"][i] | 0LL;
    r.daily[i].sunrise = daily["sunrise"][i] | 0LL; r.daily[i].sunset = daily["sunset"][i] | 0LL;
    r.daily[i].temp.max = (daily["temperature_2m_max"][i] | 0.0f) + 273.15f;
    r.daily[i].temp.min = (daily["temperature_2m_min"][i] | 0.0f) + 273.15f;
    r.daily[i].uvi = daily["uv_index_max"][i] | 0.0f;
    r.daily[i].rain = daily["precipitation_sum"][i] | 0.0f;
    r.daily[i].pop = (daily["precipitation_probability_max"][i] | 0.0f) / 100.0f;
    setWmoWeather(daily["weather_code"][i] | 0, true, r.daily[i].weather);
  }
  if (dailyCount) { r.current.sunrise = r.daily[0].sunrise; r.current.sunset = r.daily[0].sunset; }
  return code;
}

template <typename Client>
static int fetchOpenMeteoAirQuality(Client &client, owm_resp_air_pollution_t &r)
{
  String uri = "/v1/air-quality?latitude=" + LAT + "&longitude=" + LON +
    "&hourly=pm10,pm2_5,carbon_monoxide,nitrogen_dioxide,sulphur_dioxide,ozone"
    "&past_hours=23&forecast_hours=1&timeformat=unixtime&timezone=auto";
  JsonDocument doc;
  int code = openMeteoGet(client, "air-quality-api.open-meteo.com", uri, doc);
  if (code != HTTP_CODE_OK) return code;
  r = {}; r.coord.lat = LAT.toFloat(); r.coord.lon = LON.toFloat();
  JsonObject h = doc["hourly"];
  size_t count = std::min<size_t>(OWM_NUM_AIR_POLLUTION, h["time"].size());
  for (size_t i = 0; i < count; ++i)
  {
    r.dt[i] = h["time"][i] | 0LL;
    r.components.pm10[i] = h["pm10"][i] | 0.0f; r.components.pm2_5[i] = h["pm2_5"][i] | 0.0f;
    r.components.co[i] = h["carbon_monoxide"][i] | 0.0f; r.components.no2[i] = h["nitrogen_dioxide"][i] | 0.0f;
    r.components.so2[i] = h["sulphur_dioxide"][i] | 0.0f; r.components.o3[i] = h["ozone"][i] | 0.0f;
  }
  return code;
}

template <typename Client>
static int fetchNWSAlerts(Client &client, std::vector<owm_alerts_t> &alerts)
{
  alerts.clear();
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  String uri = "/alerts/active?point=" + LAT + "," + LON;
  if (!http.begin(client, "api.weather.gov", 443, uri)) return -1;
  // api.weather.gov requires an identifying User-Agent and recommends GeoJSON.
  http.addHeader("User-Agent", "crowpanel-weather/1.0 (ESP32 e-paper display)");
  http.addHeader("Accept", "application/geo+json");
  Serial.println(String(TXT_ATTEMPTING_HTTP_REQ) + ": https://api.weather.gov" + uri);
  int code = http.GET();
  if (code == HTTP_CODE_OK)
  {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (err) code = -256 - static_cast<int>(err.code());
    else
    {
      JsonArray features = doc["features"];
      const size_t count = std::min<size_t>(3, features.size());
      for (size_t i = 0; i < count; ++i)
      {
        JsonObject p = features[i]["properties"];
        owm_alerts_t alert = {};
        alert.sender_name = String(p["senderName"] | "National Weather Service");
        alert.event = String(p["event"] | "Weather Alert");
        String headline = String(p["headline"] | "");
        String instruction = String(p["instruction"] | "");
        String detail = String(p["description"] | "");
        alert.description = headline;
        if (instruction.length()) alert.description += (alert.description.length() ? ". " : "") + instruction;
        if (!alert.description.length()) alert.description = detail;
        alert.description.replace("\n", " "); alert.description.replace("\r", " ");
        alert.tags = String(p["severity"] | "Unknown");
        alerts.push_back(alert);
      }
    }
  }
  http.end(); client.stop();
  Serial.printf("[alerts] NWS status=%d active=%u\n", code, static_cast<unsigned>(alerts.size()));
  return code;
}

#ifdef USE_HTTP
int getOpenMeteoForecast(WiFiClient &client, owm_resp_onecall_t &r) { return fetchOpenMeteoForecast(client, r); }
int getOpenMeteoAirQuality(WiFiClient &client, owm_resp_air_pollution_t &r) { return fetchOpenMeteoAirQuality(client, r); }
int getNWSAlerts(WiFiClient &client, std::vector<owm_alerts_t> &alerts) { return fetchNWSAlerts(client, alerts); }
#else
int getOpenMeteoForecast(WiFiClientSecure &client, owm_resp_onecall_t &r) { return fetchOpenMeteoForecast(client, r); }
int getOpenMeteoAirQuality(WiFiClientSecure &client, owm_resp_air_pollution_t &r) { return fetchOpenMeteoAirQuality(client, r); }
int getNWSAlerts(WiFiClientSecure &client, std::vector<owm_alerts_t> &alerts) { return fetchNWSAlerts(client, alerts); }
#endif

/* Prints debug information about heap usage.
 */
void printHeapUsage()
{
  Serial.println("[debug] Heap Size       : " + String(ESP.getHeapSize()) + " B");
  Serial.println("[debug] Available Heap  : " + String(ESP.getFreeHeap()) + " B");
  Serial.println("[debug] Min Free Heap   : " + String(ESP.getMinFreeHeap()) + " B");
  Serial.println("[debug] Max Allocatable : " + String(ESP.getMaxAllocHeap()) + " B");
  return;
}
