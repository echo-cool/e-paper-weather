/* Configuration options for esp32-weather-epd.
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

#include <Arduino.h>
#include "config.h"

// PINS
// The configuration below is intended for use with the project's official
// wiring diagrams using the FireBeetle 2 ESP32-E microcontroller board.
//
// Note: LED_BUILTIN pin will be disabled to reduce power draw.  Refer to your
//       board's pinout to ensure you avoid using a pin with this shared
//       functionality.
//
#if defined(BOARD_CROWPANEL_S3)
// Elecrow CrowPanel 4.2" E-Paper (ESP32-S3). The e-paper is hard-wired to these
// GPIOs on the board; do not change them. There is no battery-voltage ADC pin
// and no on-board environment sensor.
const uint8_t PIN_BAT_ADC = 1; // unused (BATTERY_MONITORING disabled)
// Pins for the on-board SSD1683 e-paper
const uint8_t PIN_EPD_BUSY = 48;
const uint8_t PIN_EPD_CS = 45;
const uint8_t PIN_EPD_RST = 47;
const uint8_t PIN_EPD_DC = 46;
const uint8_t PIN_EPD_SCK = 12;
const uint8_t PIN_EPD_MISO = 13; // not used by the panel; SPI.begin() gets -1
const uint8_t PIN_EPD_MOSI = 11;
const uint8_t PIN_EPD_PWR = 7;   // display power-enable (HIGH = on)
// Newer CrowPanels (green circular sticker on the rear) require both rails.
// Driving GPIO41 is harmless on the original revision.
const uint8_t PIN_EPD_PWR_AUX = 41;
// I2C pins (no sensor on-board; defaults for the optional GPIO-header sensor)
const uint8_t PIN_BME_SDA = 8;
const uint8_t PIN_BME_SCL = 9;
const uint8_t PIN_BME_PWR = 3;
const uint8_t BME_ADDRESS = 0x76; // If sensor does not work, try 0x77

const uint8_t PIN_SHT_SDA = 8;
const uint8_t PIN_SHT_SCL = 9;
const uint8_t PIN_SHT_PWR = 3;
const uint8_t SHT_ADDRESS = 0x44; // If sensor does not work, try 0x45

const bool USE_BME280 = false;
const bool USE_SHT = false;
#else
// ADC pin used to measure battery voltage
const uint8_t PIN_BAT_ADC = 39; // A0 for micro-usb firebeetle
// Pins for E-Paper Driver Board
const uint8_t PIN_EPD_BUSY = 17; // 5 for micro-usb firebeetle
const uint8_t PIN_EPD_CS = 5;
const uint8_t PIN_EPD_RST = 16;
const uint8_t PIN_EPD_DC = 19;
const uint8_t PIN_EPD_SCK = 18;
const uint8_t PIN_EPD_MISO = 19; // 19 Master-In Slave-Out not used, as no data from display
const uint8_t PIN_EPD_MOSI = 23;
const uint8_t PIN_EPD_PWR = 26; // Irrelevant if directly connected to 3.3V
// I2C Pins used for BME280
const uint8_t PIN_BME_SDA = 21;
const uint8_t PIN_BME_SCL = 22;
const uint8_t PIN_BME_PWR = 27;   // Irrelevant if directly connected to 3.3V
const uint8_t BME_ADDRESS = 0x76; // If sensor does not work, try 0x77

const uint8_t PIN_SHT_SDA = 21;
const uint8_t PIN_SHT_SCL = 22;
const uint8_t PIN_SHT_PWR = 27;   // Irrelevant if directly connected to 3.3V
const uint8_t SHT_ADDRESS = 0x44; // If sensor does not work, try 0x45

const bool USE_BME280 = false;
const bool USE_SHT = true;
#endif

// WIFI
// Credentials live in an untracked secrets.h (copy secrets.h.example). If it is
// absent (e.g. CI), these fall back to placeholders so the project still builds.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SECRET_WIFI_SSID
#define SECRET_WIFI_SSID "your-wifi-ssid"
#endif
#ifndef SECRET_WIFI_PASSWORD
#define SECRET_WIFI_PASSWORD "your-wifi-password"
#endif
const char *WIFI_SSID = SECRET_WIFI_SSID;
const char *WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const unsigned long WIFI_TIMEOUT = 10000; // ms, WiFi connection timeout.

// OTA (over-the-air firmware update)
// mDNS hostname (reachable as <hostname>.local) and optional password used by
// ArduinoOTA / espota. Leave OTA_PASSWORD as "" for no password.
const char *OTA_HOSTNAME = "crowpanel-weather";
const char *OTA_PASSWORD = "";

// HTTP
// The following errors are likely the result of insuffient http client tcp
// timeout:
//   -1   Connection Refused
//   -11  Read Timeout
//   -258 Deserialization Incomplete Input
const unsigned HTTP_CLIENT_TCP_TIMEOUT = 10000; // ms

// WEATHER DATA SOURCE
// Weather and air quality come from Open-Meteo (https://open-meteo.com) and
// weather alerts from the US National Weather Service. Both are free and
// require NO API key, so there is nothing to configure here.

// LOCATION
// Initial latitude/longitude. These can be changed at runtime from the web UI
// (city or postal code, geocoded via Open-Meteo) and are persisted to NVS.
// lat=40.7128&lon=-74.0060 new york

String LAT = "37.3541";
String LON = "-121.9552";
String CITY_STRING = "Santa Clara, CA 95051";
String LOCATION_QUERY = "Santa Clara 95051";


// const String LAT = "37.3673";
// const String LON = "-121.9827";
// // City name that will be shown in the top-right corner of the display.
// const String CITY_STRING = "Santa Clara";

// TIME
// For list of time zones see
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
String TIMEZONE = "PST8PDT,M3.2.0,M11.1.0";
// Time format used when displaying sunrise/set times. (Max 11 characters)
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
// const char *TIME_FORMAT = "%l:%M%P"; // 12-hour ex: 1:23am  11:00pm
const char *TIME_FORMAT = "%H:%M"; // 24-hour ex: 01:23   23:00
// Time format used when displaying axis labels. (Max 11 characters)
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
// const char *HOUR_FORMAT = "%l%P"; // 12-hour ex: 1am  11pm
const char *HOUR_FORMAT = "%H"; // 24-hour ex: 01   23
// Date format used when displaying date in top-right corner.
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
const char *DATE_FORMAT = "%a, %B %e"; // ex: Sat, January 1
// Date/Time format used when displaying the last refresh time along the bottom
// of the screen.
// For more information about formatting see
// https://man7.org/linux/man-pages/man3/strftime.3.html
const char *REFRESH_TIME_FORMAT = "%x %H:%M";
// NTP_SERVER_1 is the primary time server, while NTP_SERVER_2 is a fallback.
// pool.ntp.org will find the closest available NTP server to you.
const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";
// If you encounter the 'Failed To Fetch The Time' error, try increasing
// NTP_TIMEOUT or select closer/lower latency time servers.
const unsigned long NTP_TIMEOUT = 20000; // ms
// Sleep duration in minutes. (aka how often esp32 will wake for an update)
// Aligned to the nearest minute boundary and must evenly divide 60.
// For example, if set to 30 (minutes) the display will update at 00 or 30
// minutes past the hour. (range: [2-60])
const long SLEEP_DURATION = 30;
// If BED_TIME == WAKE_TIME, then this battery saving feature will be disabled.
// (range: [0-23])
const int BED_TIME = 00;  // Last update at 00:00 (midnight) until WAKE_TIME.
const int WAKE_TIME = 06; // Hour of first update after BED_TIME, 06:00.

// HOURLY OUTLOOK GRAPH
// Number of hours to display on the outlook graph. (range: [8-48])
const int HOURLY_GRAPH_MAX = 24;

// BATTERY
// To protect the battery upon LOW_BATTERY_VOLTAGE, the display will cease to
// update until battery is charged again. The ESP32 will deep-sleep (consuming
// < 11μA), waking briefly check the voltage at the corresponding interval (in
// minutes). Once the battery voltage has fallen to CRIT_LOW_BATTERY_VOLTAGE,
// the esp32 will hibernate and a manual press of the reset (RST) button to
// begin operating again.
const uint32_t MAX_BATTERY_VOLTAGE = 4200;                 // (millivolts)
const uint32_t WARN_BATTERY_VOLTAGE = 3400;                // (millivolts)
const uint32_t LOW_BATTERY_VOLTAGE = 3200;                 // (millivolts)
const uint32_t VERY_LOW_BATTERY_VOLTAGE = 3100;            // (millivolts)
const uint32_t CRIT_LOW_BATTERY_VOLTAGE = 3000;            // (millivolts)
const unsigned long LOW_BATTERY_SLEEP_INTERVAL = 30;       // (minutes)
const unsigned long VERY_LOW_BATTERY_SLEEP_INTERVAL = 120; // (minutes)

// See config.h for the below options
// E-PAPER PANEL
// LOCALE
// UNITS
// WIND ICON PRECISION
// FONTS
// ALERTS
// BATTERY MONITORING
