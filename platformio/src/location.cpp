#include "location.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include "config.h"

static String urlEncode(const String &value)
{
  String out;
  const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); ++i)
  {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else if (c == ' ') out += "%20";
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

static String posixTimezone(const String &iana)
{
  if (iana == "America/Los_Angeles") return "PST8PDT,M3.2.0,M11.1.0";
  if (iana == "America/Denver") return "MST7MDT,M3.2.0,M11.1.0";
  if (iana == "America/Chicago") return "CST6CDT,M3.2.0,M11.1.0";
  if (iana == "America/New_York") return "EST5EDT,M3.2.0,M11.1.0";
  if (iana == "America/Phoenix") return "MST7";
  if (iana == "Europe/London") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (iana == "Europe/Paris" || iana == "Europe/Berlin") return "CET-1CEST,M3.5.0,M10.5.0/3";
  return "UTC0";
}

void loadLocationSettings()
{
  Preferences prefs;
  if (!prefs.begin("weather_epd", true)) return;
  LOCATION_QUERY = prefs.getString("locQuery", LOCATION_QUERY);
  CITY_STRING = prefs.getString("locName", CITY_STRING);
  LAT = prefs.getString("locLat", LAT);
  LON = prefs.getString("locLon", LON);
  TIMEZONE = prefs.getString("locTz", TIMEZONE);
  prefs.end();
  Serial.printf("[location] %s (%s, %s)\n", CITY_STRING.c_str(), LAT.c_str(), LON.c_str());
}

bool setLocationFromQuery(const String &rawQuery, String &message)
{
  String query = rawQuery;
  query.trim();
  if (query.length() < 2) { message = "Enter a city or postal code."; return false; }

  // Open-Meteo accepts either a place name or a postal code, but not always a
  // combined phrase such as "Santa Clara 95051". Prefer a trailing numeric
  // postal code when one is present while retaining the friendly full query.
  String searchTerm = query;
  int lastSpace = query.lastIndexOf(' ');
  if (lastSpace >= 0)
  {
    String suffix = query.substring(lastSpace + 1);
    bool numeric = suffix.length() >= 3;
    for (size_t i = 0; i < suffix.length(); ++i) numeric = numeric && isDigit(suffix[i]);
    if (numeric) searchTerm = suffix;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
               urlEncode(searchTerm) + "&count=1&language=en&format=json";
  http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  if (!http.begin(client, url)) { message = "Could not start geocoding request."; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { message = "Geocoding failed (HTTP " + String(code) + ")."; http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err || !doc["results"][0]) { message = "No matching city or postal code was found."; http.end(); return false; }
  JsonObject result = doc["results"][0];
  String name = result["name"] | query;
  String admin = result["admin1"] | "";
  String country = result["country_code"] | "";
  String display = name;
  if (admin.length() && admin != name) display += ", " + admin;
  if (country.length()) display += " " + country;
  if (searchTerm != query) display = query;
  String lat = String(result["latitude"].as<double>(), 5);
  String lon = String(result["longitude"].as<double>(), 5);
  String tz = posixTimezone(String(result["timezone"] | "UTC"));
  http.end();

  Preferences prefs;
  if (!prefs.begin("weather_epd", false)) { message = "Could not save location."; return false; }
  prefs.putString("locQuery", query);
  prefs.putString("locName", display);
  prefs.putString("locLat", lat);
  prefs.putString("locLon", lon);
  prefs.putString("locTz", tz);
  prefs.end();
  LOCATION_QUERY = query; CITY_STRING = display; LAT = lat; LON = lon; TIMEZONE = tz;
  message = "Location set to " + display + ". Rebooting...";
  return true;
}
