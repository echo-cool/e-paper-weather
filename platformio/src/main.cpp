/* Main program for esp32-weather-epd.
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
#include <Adafruit_BME280.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <Wire.h>

#include "_locale.h"
#include "api_response.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "icons/icons_196x196.h"
#include "location.h"
#include "ota.h"
#include "renderer.h"
#if defined(USE_HTTPS_WITH_CERT_VERIF) || defined(USE_HTTPS_WITH_CERT_VERIF)
#include <WiFiClientSecure.h>
#endif
#ifdef USE_HTTPS_WITH_CERT_VERIF
#include "cert.h"
#endif

// too large to allocate locally on stack
static owm_resp_onecall_t owm_onecall;
static owm_resp_air_pollution_t owm_air_pollution;

Preferences prefs;

/* Put esp32 into ultra low-power deep sleep (<11μA).
 * Aligns wake time to the minute. Sleep times defined in config.cpp.
 */
void beginDeepSleep(unsigned long &startTime, tm *timeInfo)
{
  if (!getLocalTime(timeInfo))
  {
    Serial.println(TXT_REFERENCING_OLDER_TIME_NOTICE);
  }

  uint64_t sleepDuration = 0;
  int extraHoursUntilWake = 0;
  int curHour = timeInfo->tm_hour;

  if (timeInfo->tm_min >= 58)
  { // if we are within 2 minutes of the next hour, then round up for the
    // purposes of bed time
    curHour = (curHour + 1) % 24;
    extraHoursUntilWake += 1;
  }

  if (BED_TIME < WAKE_TIME && curHour >= BED_TIME && curHour < WAKE_TIME)
  { // 0              B   v  W  24
    // |--------------zzzzZzz---|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour < WAKE_TIME)
  { // 0 v W               B    24
    // |zZz----------------zzzzz|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour >= BED_TIME)
  { // 0   W               B  v 24
    // |zzz----------------zzzZz|
    extraHoursUntilWake += WAKE_TIME - (curHour - 24);
  }
  else // This feature is disabled (BED_TIME == WAKE_TIME)
  {    // OR it is not past BED_TIME
    extraHoursUntilWake = 0;
  }

  if (extraHoursUntilWake == 0)
  { // align wake time to nearest multiple of SLEEP_DURATION
    sleepDuration = SLEEP_DURATION * 60ULL - ((timeInfo->tm_min % SLEEP_DURATION) * 60ULL + timeInfo->tm_sec);
  }
  else
  { // align wake time to the hour
    sleepDuration = extraHoursUntilWake * 3600ULL - (timeInfo->tm_min * 60ULL + timeInfo->tm_sec);
  }

  // if we are within 2 minutes of the next alignment.
  if (sleepDuration <= 120ULL)
  {
    sleepDuration += SLEEP_DURATION * 60ULL;
  }

  // add extra delay to compensate for esp32's with fast RTCs.
  sleepDuration += 10ULL;

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
  Serial.print(TXT_AWAKE_FOR);
  Serial.println(" " + String((millis() - startTime) / 1000.0, 3) + "s");
  Serial.print(TXT_ENTERING_DEEP_SLEEP_FOR);
  Serial.println(" " + String(sleepDuration) + "s");
  esp_deep_sleep_start();
} // end beginDeepSleep

#if !ALWAYS_ON
/* Program entry point.
 */
void setup()
{
  unsigned long startTime = millis();
  Serial.begin(115200);

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  disableBuiltinLED();

  // Open namespace for read/write to non-volatile storage
  prefs.begin(NVS_NAMESPACE, false);

#if BATTERY_MONITORING
  uint32_t batteryVoltage = readBatteryVoltage();
  Serial.print(TXT_BATTERY_VOLTAGE);
  Serial.println(": " + String(batteryVoltage) + "mv");

  // When the battery is low, the display should be updated to reflect that, but
  // only the first time we detect low voltage. The next time the display will
  // refresh is when voltage is no longer low. To keep track of that we will
  // make use of non-volatile storage.
  bool lowBat = prefs.getBool("lowBat", false);

  // low battery, deep sleep now
  if (batteryVoltage <= LOW_BATTERY_VOLTAGE)
  {
    if (lowBat == false)
    { // battery is now low for the first time
      prefs.putBool("lowBat", true);
      prefs.end();
      initDisplay();
      do
      {
        drawError(battery_alert_0deg_196x196, TXT_LOW_BATTERY);
      } while (display.nextPage());
      powerOffDisplay();
    }

    if (batteryVoltage <= CRIT_LOW_BATTERY_VOLTAGE)
    { // critically low battery
      // don't set esp_sleep_enable_timer_wakeup();
      // We won't wake up again until someone manually presses the RST button.
      Serial.println(TXT_CRIT_LOW_BATTERY_VOLTAGE);
      Serial.println(TXT_HIBERNATING_INDEFINITELY_NOTICE);
    }
    else if (batteryVoltage <= VERY_LOW_BATTERY_VOLTAGE)
    { // very low battery
      esp_sleep_enable_timer_wakeup(VERY_LOW_BATTERY_SLEEP_INTERVAL * 60ULL * 1000000ULL);
      Serial.println(TXT_VERY_LOW_BATTERY_VOLTAGE);
      Serial.print(TXT_ENTERING_DEEP_SLEEP_FOR);
      Serial.println(" " + String(VERY_LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    else
    { // low battery
      esp_sleep_enable_timer_wakeup(LOW_BATTERY_SLEEP_INTERVAL * 60ULL * 1000000ULL);
      Serial.println(TXT_LOW_BATTERY_VOLTAGE);
      Serial.print(TXT_ENTERING_DEEP_SLEEP_FOR);
      Serial.println(" " + String(LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    esp_deep_sleep_start();
  }
  // battery is no longer low, reset variable in non-volatile storage
  if (lowBat == true)
  {
    prefs.putBool("lowBat", false);
  }
#else
  uint32_t batteryVoltage = UINT32_MAX;
#endif

  // All data should have been loaded from NVS. Close filesystem.
  prefs.end();

  String statusStr = {};
  String tmpStr = {};
  tm timeInfo = {};

  // START WIFI
  int wifiRSSI = 0; // “Received Signal Strength Indicator"
  wl_status_t wifiStatus = startWiFi(wifiRSSI);
  if (wifiStatus != WL_CONNECTED)
  { // WiFi Connection Failed
    killWiFi();
    initDisplay();
    if (wifiStatus == WL_NO_SSID_AVAIL)
    {
      Serial.println(TXT_NETWORK_NOT_AVAILABLE);
      do
      {
        drawError(wifi_x_196x196, TXT_NETWORK_NOT_AVAILABLE);
      } while (display.nextPage());
    }
    else
    {
      Serial.println(TXT_WIFI_CONNECTION_FAILED);
      do
      {
        drawError(wifi_x_196x196, TXT_WIFI_CONNECTION_FAILED);
      } while (display.nextPage());
    }
    powerOffDisplay();
    beginDeepSleep(startTime, &timeInfo);
  }

  // TIME SYNCHRONIZATION
  configTzTime(TIMEZONE.c_str(), NTP_SERVER_1, NTP_SERVER_2);
  bool timeConfigured = waitForSNTPSync(&timeInfo);
  if (!timeConfigured)
  {
    Serial.println(TXT_TIME_SYNCHRONIZATION_FAILED);
    killWiFi();
    initDisplay();
    do
    {
      drawError(wi_time_4_196x196, TXT_TIME_SYNCHRONIZATION_FAILED);
    } while (display.nextPage());
    powerOffDisplay();
    beginDeepSleep(startTime, &timeInfo);
  }

  // MAKE API REQUESTS
#ifdef USE_HTTP
  WiFiClient client;
#elif defined(USE_HTTPS_NO_CERT_VERIF)
  WiFiClientSecure client;
  client.setInsecure();
#elif defined(USE_HTTPS_WITH_CERT_VERIF)
  WiFiClientSecure client;
  client.setCACert(cert_Sectigo_RSA_Domain_Validation_Secure_Server_CA);
#endif
  int rxStatus = getOpenMeteoForecast(client, owm_onecall);
  if (rxStatus != HTTP_CODE_OK)
  {
    killWiFi();
    statusStr = "Open-Meteo Forecast API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    initDisplay();
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
    powerOffDisplay();
    beginDeepSleep(startTime, &timeInfo);
  }
  rxStatus = getOpenMeteoAirQuality(client, owm_air_pollution);
  if (rxStatus != HTTP_CODE_OK)
  {
    killWiFi();
    statusStr = "Open-Meteo Air Quality API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    initDisplay();
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
    powerOffDisplay();
    beginDeepSleep(startTime, &timeInfo);
  }
  killWiFi(); // WiFi no longer needed
  float inTemp = NAN;
  float inHumidity = NAN;
  if (USE_BME280)
  {
    // GET INDOOR TEMPERATURE AND HUMIDITY, start BME280...
    pinMode(PIN_BME_PWR, OUTPUT);
    digitalWrite(PIN_BME_PWR, HIGH);

    Serial.print(String(TXT_READING_FROM) + " BME280... ");
    TwoWire I2C_bme = TwoWire(0);
    Adafruit_BME280 bme;

    I2C_bme.begin(PIN_BME_SDA, PIN_BME_SCL, 100000); // 100kHz
    if (bme.begin(BME_ADDRESS, &I2C_bme))
    {
      inTemp = bme.readTemperature();  // Celsius
      inHumidity = bme.readHumidity(); // %

      // check if BME readings are valid
      // note: readings are checked again before drawing to screen. If a reading
      //       is not a number (NAN) then an error occurred, a dash '-' will be
      //       displayed.
      if (std::isnan(inTemp) || std::isnan(inHumidity))
      {
        statusStr = "BME " + String(TXT_READ_FAILED);
        Serial.println(statusStr);
      }
      else
      {
        Serial.println(TXT_SUCCESS);
      }
    }
    else
    {
      statusStr = "BME " + String(TXT_NOT_FOUND); // check wiring
      Serial.println(statusStr);
    }
    digitalWrite(PIN_BME_PWR, LOW);
  }
  else if (USE_SHT)
  {
    // GET INDOOR TEMPERATURE AND HUMIDITY, start SHT31...
    pinMode(PIN_SHT_PWR, OUTPUT);
    digitalWrite(PIN_SHT_PWR, HIGH);

    Serial.print(String(TXT_READING_FROM) + " SHT31... ");
    Adafruit_SHT31 sht;
    if (sht.begin())
    {
      inTemp = sht.readTemperature();  // Celsius
      inHumidity = sht.readHumidity(); // %

      // check if SHT readings are valid
      // note: readings are checked again before drawing to screen. If a reading
      //       is not a number (NAN) then an error occurred, a dash '-' will be
      //       displayed.
      if (std::isnan(inTemp) || std::isnan(inHumidity))
      {
        statusStr = "SHT " + String(TXT_READ_FAILED);
        Serial.println(statusStr);
      }
      else
      {
        Serial.println(TXT_SUCCESS);
      }
    }
    else
    {
      statusStr = "SHT " + String(TXT_NOT_FOUND); // check wiring
      Serial.println(statusStr);
    }
    digitalWrite(PIN_SHT_PWR, LOW);
  }
  String refreshTimeStr;
  getRefreshTimeStr(refreshTimeStr, timeConfigured, &timeInfo);
  String dateStr;
  getDateStr(dateStr, &timeInfo);

  // RENDER FULL REFRESH
  initDisplay();
  do
  {
    drawGrid();
    drawCurrentConditions(owm_onecall.current, owm_onecall.daily[0],
                          owm_air_pollution, inTemp, inHumidity);
    drawForecast(owm_onecall.daily, timeInfo);
    drawLocationDate(CITY_STRING, dateStr);
    drawOutlookGraph(owm_onecall.hourly, owm_onecall.current, timeInfo);
#if DISPLAY_ALERTS
    drawAlerts(owm_onecall.alerts, CITY_STRING, dateStr);
#endif
    drawStatusBar(statusStr, refreshTimeStr, wifiRSSI, batteryVoltage);
  } while (display.nextPage());
  powerOffDisplay();

  // DEEP SLEEP
  beginDeepSleep(startTime, &timeInfo);
} // end setup

/* This will never run
 */
void loop()
{
} // end loop

#else // ALWAYS_ON

// ---------------------------------------------------------------------------
// Always-on flow (CrowPanel): stay awake, keep WiFi connected, refresh the
// display every SLEEP_DURATION minutes, and keep OTA available continuously.
// ---------------------------------------------------------------------------

static unsigned long lastUpdateMs = 0;
static bool firstUpdateDone = false;
static bool otaStarted = false;

/* Render a full-screen message (draws to the canvas and blits to the panel). */
static void showMessage(const uint8_t *bitmap, const String &ln1,
                        const String &ln2 = "")
{
  initDisplay();
  ssd1683BeginCanvas();
  drawError(bitmap, ln1, ln2);
  ssd1683CommitCanvas();
  powerOffDisplay();
}

/* Fetch weather data and render the display once. Assumes WiFi is connected.
 * Never sleeps and never drops WiFi (OTA must stay reachable). On an API error
 * it draws the error screen and returns so loop() can retry next interval.
 */
static void runWeatherUpdate()
{
  String statusStr = {};
  String tmpStr = {};
  tm timeInfo = {};

  // Keep the clock fresh (cheap, and recovers if NTP failed at boot).
  bool timeConfigured = waitForSNTPSync(&timeInfo);
  if (!timeConfigured)
  {
    Serial.println(TXT_TIME_SYNCHRONIZATION_FAILED);
  }

  // API requests
#ifdef USE_HTTP
  WiFiClient client;
#elif defined(USE_HTTPS_NO_CERT_VERIF)
  WiFiClientSecure client;
  client.setInsecure();
#elif defined(USE_HTTPS_WITH_CERT_VERIF)
  WiFiClientSecure client;
  client.setCACert(cert_Sectigo_RSA_Domain_Validation_Secure_Server_CA);
#endif

  int rxStatus = getOpenMeteoForecast(client, owm_onecall);
  if (rxStatus != HTTP_CODE_OK)
  {
    statusStr = "Open-Meteo Forecast API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    showMessage(wi_cloud_down_196x196, statusStr, tmpStr);
    return;
  }
  // Open-Meteo does not publish government alert products. For US locations,
  // supplement it with active point alerts from the National Weather Service.
  // Alert lookup failure is non-fatal so worldwide locations still render.
  getNWSAlerts(client, owm_onecall.alerts);
  rxStatus = getOpenMeteoAirQuality(client, owm_air_pollution);
  if (rxStatus != HTTP_CODE_OK)
  {
    statusStr = "Open-Meteo Air Quality API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    showMessage(wi_cloud_down_196x196, statusStr, tmpStr);
    return;
  }

  // No indoor sensor on this board.
  float inTemp = NAN;
  float inHumidity = NAN;

  String refreshTimeStr;
  getRefreshTimeStr(refreshTimeStr, timeConfigured, &timeInfo);
  String dateStr;
  getDateStr(dateStr, &timeInfo);
  int wifiRSSI = WiFi.RSSI();

  // RENDER FULL REFRESH (draw to the in-RAM canvas, then blit to the panel)
  initDisplay();
  ssd1683BeginCanvas();
  drawGrid();
  drawCurrentConditions(owm_onecall.current, owm_onecall.daily[0],
                        owm_air_pollution, inTemp, inHumidity);
  drawForecast(owm_onecall.daily, timeInfo);
  drawLocationDate(CITY_STRING, dateStr);
  drawOutlookGraph(owm_onecall.hourly, owm_onecall.current, timeInfo);
#if DISPLAY_ALERTS
  drawAlerts(owm_onecall.alerts, CITY_STRING, dateStr);
#endif
  drawStatusBar(statusStr, refreshTimeStr, wifiRSSI, UINT32_MAX);
  ssd1683CommitCanvas();
  powerOffDisplay();

  Serial.println(TXT_SUCCESS);
} // end runWeatherUpdate

/* Program entry point (always-on). */
void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("esp32-weather-epd " FW_VERSION " (always-on + OTA)");

  loadLocationSettings();

  // A reset can otherwise leave the previous e-paper image visible for many
  // seconds. Perform a real panel refresh to white before any network work.
  initDisplay();
  ssd1683ConditionPanel();
  powerOffDisplay();

#if !defined(BOARD_CROWPANEL_S3)
  disableBuiltinLED();
#endif

  // START WIFI (kept connected for the lifetime of the device)
  int wifiRSSI = 0;
  wl_status_t wifiStatus = startWiFi(wifiRSSI);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (wifiStatus != WL_CONNECTED)
  { // show an error now; loop() will keep retrying the connection
    Serial.println(TXT_WIFI_CONNECTION_FAILED);
    showMessage(wifi_x_196x196,
                wifiStatus == WL_NO_SSID_AVAIL ? TXT_NETWORK_NOT_AVAILABLE
                                               : TXT_WIFI_CONNECTION_FAILED);
  }

  // TIME SYNCHRONIZATION
  configTzTime(TIMEZONE.c_str(), NTP_SERVER_1, NTP_SERVER_2);

  if (WiFi.status() == WL_CONNECTED)
  {
#if OTA_ENABLED
    initOTA();
    otaStarted = true;
#endif
    runWeatherUpdate();
    firstUpdateDone = true;
    lastUpdateMs = millis();
  }

  // Dump a screenshot of whatever is now on screen over Serial so the UI can be
  // inspected without reading the panel. Also available at http://<ip>/.
  ssd1683SerialDumpScreenshot();
} // end setup

/* Main loop (always-on): service OTA and refresh on the configured interval. */
void loop()
{
  // On-demand screenshot: send 's' over the serial monitor to dump the current
  // screen as a base64 BMP.
  if (Serial.available() > 0)
  {
    int ch = Serial.read();
    if (ch == 's' || ch == 'S')
    {
      ssd1683SerialDumpScreenshot();
    }
  }

  // Recover WiFi if it dropped.
  if (WiFi.status() != WL_CONNECTED)
  {
    static unsigned long lastReconnectMs = 0;
    if (millis() - lastReconnectMs > 10000UL)
    {
      lastReconnectMs = millis();
      Serial.println("[wifi] reconnecting...");
      WiFi.reconnect();
    }
    delay(50);
    return;
  }

  // WiFi is up: make sure OTA is running (it needs an IP address to start).
#if OTA_ENABLED
  if (!otaStarted)
  {
    initOTA();
    otaStarted = true;
  }
  handleOTA();
#endif

  // An immediate refresh requested from the web control center jumps the
  // periodic schedule. Left queued while a firmware upload is running.
#if OTA_ENABLED
  if (!otaInProgress() && otaConsumePendingRefresh())
  {
    firstUpdateDone = false;
  }
#endif

  // Periodic display refresh (skipped while a firmware upload is in progress).
  const unsigned long intervalMs =
      static_cast<unsigned long>(SLEEP_DURATION) * 60UL * 1000UL;
  bool due = !firstUpdateDone || (millis() - lastUpdateMs >= intervalMs);
  if (due
#if OTA_ENABLED
      && !otaInProgress()
#endif
  )
  {
    runWeatherUpdate();
    firstUpdateDone = true;
    lastUpdateMs = millis();
  }

  delay(10);
} // end loop

#endif // ALWAYS_ON
