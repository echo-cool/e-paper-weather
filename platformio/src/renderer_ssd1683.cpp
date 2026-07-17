/* Dense 400x300 layout for esp32-weather-epd on the SSD1683 panel
 * (Elecrow CrowPanel 4.2" ESP32-S3 E-Paper).
 *
 * This file implements the same drawing entry points as renderer.cpp, but laid
 * out for the small 400x300 black/white panel. renderer.cpp compiles its
 * 800x480 versions of these functions only when DISP_SSD1683 is NOT defined, so
 * exactly one implementation of each is linked.
 *
 * Rendering goes into an in-RAM 1-bit canvas (fb) rather than straight to the
 * panel. The canvas is then blitted to the e-paper (ssd1683CommitCanvas), and
 * can be served as a screenshot (BMP over HTTP / base64 over Serial) since the
 * e-paper itself cannot be read back. On the canvas a set bit (1) = black ink;
 * the shared text helpers draw through the global g_gfx which points at fb.
 *
 * Layout (400 wide x 300 tall, rotation 0):
 *   y  0.. 18  header: city (left) + date (right)
 *   y 20..188  current conditions (left half) | stats column (right half)
 *   y190..287  5-day forecast row (5 columns)
 *   y288..300  status bar: last refresh (left) + WiFi (right)
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#ifdef DISP_SSD1683

#include <cmath>
#include <cstring>
#include <vector>
#include <Arduino.h>
#include <time.h>

#include "_locale.h"     // TXT_* strings, AQI_SCALE, <aqi.h>
#include "_strftime.h"
#include "api_response.h"
#include "conversions.h"
#include "display_utils.h"
#include "renderer.h"

// fonts
#include FONT_HEADER

#ifndef ACCENT_COLOR
#define ACCENT_COLOR GxEPD_BLACK
#endif

// In-RAM framebuffer. 1 bit per pixel; a set bit is black ink. 400x300/8 = 15KB.
GFXcanvas1 fb(DISP_WIDTH, DISP_HEIGHT);

// Canvas pixel values (GFXcanvas1: any non-zero color sets the bit).
static const uint16_t INK = 1; // black
static const uint16_t BG = 0;  // white

// ---- Layout constants -------------------------------------------------------
static const int HEADER_H      = 18;  // header separator y
static const int MAIN_BOTTOM   = 151; // compact summary; larger lower chart
static const int STATUS_TOP    = 287; // separator above status bar
static const int COL_DIVIDER_X = 198; // vertical divider between current & stats

// ---- Small helpers ----------------------------------------------------------

// Nearest-neighbour downscale for monochrome weather icons. The source assets
// are 96x96; 64x64 leaves enough vertical room for the expanded chart.
static void fbInvertedBitmap96to64(int16_t x, int16_t y,
                                   const uint8_t *bitmap)
{
  for (int16_t dy = 0; dy < 64; ++dy)
  {
    int16_t sy = dy * 96 / 64;
    for (int16_t dx = 0; dx < 64; ++dx)
    {
      int16_t sx = dx * 96 / 64;
      uint8_t b = bitmap[sy * 12 + (sx / 8)];
      if (!(b & (0x80 >> (sx & 7)))) fb.drawPixel(x + dx, y + dy, INK);
    }
  }
}

/* Round a temperature (Kelvin from OWM) to the configured unit and append the
 * degree symbol.
 */
static String fmtTemp(float kelvin)
{
#ifdef UNITS_TEMP_KELVIN
  return String(static_cast<int>(std::round(kelvin)));
#endif
#ifdef UNITS_TEMP_CELSIUS
  return String(static_cast<int>(std::round(kelvin_to_celsius(kelvin)))) + "\260";
#endif
#ifdef UNITS_TEMP_FAHRENHEIT
  return String(static_cast<int>(std::round(kelvin_to_fahrenheit(kelvin)))) + "\260";
#endif
}

/* Format a unix timestamp using the configured TIME_FORMAT (e.g. sunrise). */
static String fmtTime(int64_t unixTime)
{
  char buf[12] = {};
  time_t ts = static_cast<time_t>(unixTime);
  tm *ti = localtime(&ts);
  _strftime(buf, sizeof(buf), TIME_FORMAT, ti);
  return String(buf);
}

/* Draw a single "Label ................ Value" row in the stats column. */
static void drawStatRow(int y, const String &label, const String &value,
                        const GFXfont *valueFont)
{
  fb.setFont(&FONT_7pt8b);
  drawString(COL_DIVIDER_X + 8, y, label, LEFT, INK);
  fb.setFont(valueFont);
  drawString(DISP_WIDTH - 6, y, value, RIGHT, INK);
}

// ---- Frame / dividers -------------------------------------------------------

void drawGrid()
{
  fb.drawFastHLine(0, HEADER_H, DISP_WIDTH, INK);
  fb.drawFastHLine(0, MAIN_BOTTOM, DISP_WIDTH, INK);
  fb.drawFastHLine(0, STATUS_TOP, DISP_WIDTH, INK);
  fb.drawFastVLine(COL_DIVIDER_X, HEADER_H + 3, MAIN_BOTTOM - HEADER_H - 6, INK);
}

// ---- Header (city + date) ---------------------------------------------------

void drawLocationDate(const String &city, const String &date)
{
  fb.setFont(&FONT_9pt8b);
  drawString(4, 14, city, LEFT, INK);
  drawString(DISP_WIDTH - 4, 14, date, RIGHT, INK);
}

// ---- Current conditions (left) + stats (right) ------------------------------

void drawCurrentConditions(const owm_current_t &current,
                           const owm_daily_t &today,
                           const owm_resp_air_pollution_t &owm_air_pollution,
                           float inTemp, float inHumidity)
{
  (void)inTemp;
  (void)inHumidity; // no indoor sensor on this board

  // --- Left: hero icon + big temperature ---
  fbInvertedBitmap96to64(5, 24, getCurrentConditionsBitmap96(current, today));

  // big current temperature, to the right of the icon
  fb.setFont(&FONT_26pt8b);
  drawString(139, 62, fmtTemp(current.temp), CENTER, INK);

  // feels like
  fb.setFont(&FONT_7pt8b);
  drawString(139, 83, String(TXT_FEELS_LIKE) + ' ' + fmtTemp(current.feels_like),
             CENTER, INK);

  // weather description (title-cased), centered under the icon (up to 2 lines)
  String desc = current.weather.description;
  toTitleCase(desc);
  fb.setFont(&FONT_7pt8b);
  drawMultiLnString(98, 109, desc, CENTER, 190, 2, 13, INK);

  // daily high / low
  fb.setFont(&FONT_8pt8b);
  drawString(98, 146, "H:" + fmtTemp(today.temp.max)
                          + "  L:" + fmtTemp(today.temp.min), CENTER, INK);

  // --- Right: stats column ---
  int y = 31;
  const int step = 18;

  // humidity
  drawStatRow(y, TXT_HUMIDITY, String(current.humidity) + "%", &FONT_8pt8b);
  y += step;

  // wind (compass direction + speed)
  {
    int windSpeed;
#ifdef UNITS_SPEED_METERSPERSECOND
    windSpeed = static_cast<int>(std::round(current.wind_speed));
#elif defined(UNITS_SPEED_FEETPERSECOND)
    windSpeed = static_cast<int>(std::round(meterspersecond_to_feetpersecond(current.wind_speed)));
#elif defined(UNITS_SPEED_MILESPERHOUR)
    windSpeed = static_cast<int>(std::round(meterspersecond_to_milesperhour(current.wind_speed)));
#elif defined(UNITS_SPEED_KNOTS)
    windSpeed = static_cast<int>(std::round(meterspersecond_to_knots(current.wind_speed)));
#elif defined(UNITS_SPEED_BEAUFORT)
    windSpeed = meterspersecond_to_beaufort(current.wind_speed);
#else // UNITS_SPEED_KILOMETERSPERHOUR
    windSpeed = static_cast<int>(std::round(meterspersecond_to_kilometersperhour(current.wind_speed)));
#endif
    String windStr = String(getCompassPointNotation(current.wind_deg)) + ' '
                     + String(windSpeed);
    drawStatRow(y, TXT_WIND, windStr, &FONT_8pt8b);
    y += step;
  }

  // pressure
  {
    String presStr;
#ifdef UNITS_PRES_HECTOPASCALS
    presStr = String(current.pressure);
#elif defined(UNITS_PRES_PASCALS)
    presStr = String(static_cast<int>(std::round(hectopascals_to_pascals(current.pressure))));
#elif defined(UNITS_PRES_MILLIMETERSOFMERCURY)
    presStr = String(static_cast<int>(std::round(hectopascals_to_millimetersofmercury(current.pressure))));
#elif defined(UNITS_PRES_INCHESOFMERCURY)
    presStr = String(hectopascals_to_inchesofmercury(current.pressure), 1);
#elif defined(UNITS_PRES_ATMOSPHERES)
    presStr = String(hectopascals_to_atmospheres(current.pressure), 3);
#elif defined(UNITS_PRES_GRAMSPERSQUARECENTIMETER)
    presStr = String(static_cast<int>(std::round(hectopascals_to_gramspersquarecentimeter(current.pressure))));
#elif defined(UNITS_PRES_POUNDSPERSQUAREINCH)
    presStr = String(hectopascals_to_poundspersquareinch(current.pressure), 2);
#else // UNITS_PRES_MILLIBARS
    presStr = String(static_cast<int>(std::round(hectopascals_to_millibars(current.pressure))));
#endif
    drawStatRow(y, TXT_PRESSURE, presStr, &FONT_8pt8b);
    y += step;
  }

  // UV index (value + descriptor, smaller font to fit descriptor)
  {
    unsigned int uvi = static_cast<unsigned int>(std::max(std::round(current.uvi), 0.0f));
    String uvStr = String(uvi) + ' ' + String(getUVIdesc(uvi));
    drawStatRow(y, TXT_UV_INDEX, uvStr, &FONT_7pt8b);
    y += step;
  }

  // air quality index
  {
    const owm_components_t &c = owm_air_pollution.components;
    int aqi = calc_aqi(AQI_SCALE, c.co, c.nh3, c.no, c.no2, c.o3, NULL, c.so2,
                       c.pm10, c.pm2_5);
    int aqiMax = aqi_scale_max(AQI_SCALE);
    String aqiVal = (aqi > aqiMax ? "> " + String(aqiMax) : String(aqi));
    aqiVal += ' ';
    aqiVal += String(aqi_desc(AQI_SCALE, aqi));
    const char *aqiLabel = (aqi_desc_type(AQI_SCALE) == AIR_QUALITY_DESC)
                               ? TXT_AIR_QUALITY : TXT_AIR_POLLUTION;
    drawStatRow(y, aqiLabel, aqiVal, &FONT_7pt8b);
    y += step;
  }

  // sunrise / sunset
  drawStatRow(y, TXT_SUNRISE, fmtTime(current.sunrise), &FONT_8pt8b);
  y += step;
  drawStatRow(y, TXT_SUNSET, fmtTime(current.sunset), &FONT_8pt8b);
}

// ---- Lower hourly outlook ---------------------------------------------------

void drawForecast(owm_daily_t *const daily, tm timeInfo)
{
  // The 400x300 build uses this region for the more useful 24-hour combined
  // precipitation/UV chart drawn by drawOutlookGraph().
  (void)daily;
  (void)timeInfo;
}

// ---- 24-hour precipitation + UV chart --------------------------------------

void drawOutlookGraph(owm_hourly_t *const hourly, const owm_current_t &current,
                      tm timeInfo)
{
  (void)timeInfo;

  const int pastHours = 6;
  const int rangeHours = 30; // -6h through +24h
  // The Open-Meteo fetch requests past_hours=6 and forecast_hours=24, so
  // hourly[] holds one entry per slot of the -6h..+24h axis. Plot all of them;
  // capping below rangeHours leaves the tail of the chart empty.
  const int hours = rangeHours;
  const int plotL = 31;
  const int plotR = DISP_WIDTH - 31;
  const int plotT = 169;
  const int plotB = 269;
  const int plotW = plotR - plotL;
  const int plotH = plotB - plotT;

  float precipMax = 0.0f;
  float uvMax = 0.0f;
  float tempMin = 10000.0f;
  float tempMax = -10000.0f;
  for (int i = 0; i < hours; ++i)
  {
#ifdef UNITS_HOURLY_PRECIP_POP
    float precip = hourly[i].pop * 100.0f;
#else
    float precip = hourly[i].rain_1h + hourly[i].snow_1h;
#ifdef UNITS_HOURLY_PRECIP_CENTIMETERS
    precip = millimeters_to_centimeters(precip);
#elif defined(UNITS_HOURLY_PRECIP_INCHES)
    precip = millimeters_to_inches(precip);
#endif
#endif
    precipMax = std::max(precipMax, precip);
    uvMax = std::max(uvMax, hourly[i].uvi);
#ifdef UNITS_TEMP_KELVIN
    float temp = hourly[i].temp;
#elif defined(UNITS_TEMP_CELSIUS)
    float temp = kelvin_to_celsius(hourly[i].temp);
#else
    float temp = kelvin_to_fahrenheit(hourly[i].temp);
#endif
    tempMin = std::min(tempMin, temp);
    tempMax = std::max(tempMax, temp);
  }

#ifdef UNITS_HOURLY_PRECIP_POP
  const float precipScale = 100.0f;
  const char *precipUnit = "%";
#elif defined(UNITS_HOURLY_PRECIP_CENTIMETERS)
  const float precipScale = std::max(0.1f, std::ceil(precipMax * 10.0f) / 10.0f);
  const char *precipUnit = "cm";
#elif defined(UNITS_HOURLY_PRECIP_INCHES)
  const float precipScale = std::max(0.1f, std::ceil(precipMax * 10.0f) / 10.0f);
  const char *precipUnit = "in";
#else
  const float precipScale = std::max(1.0f, std::ceil(precipMax));
  const char *precipUnit = "mm";
#endif
  // A stable three-step UV scale makes the right axis easy to scan while still
  // expanding for unusually high values.
  const float uvScale = std::max(3.0f, std::ceil(uvMax / 3.0f) * 3.0f);
  float tempScaleMin = std::floor((tempMin - 1.0f) / 5.0f) * 5.0f;
  float tempScaleMax = std::ceil((tempMax + 1.0f) / 5.0f) * 5.0f;
  if (tempScaleMax <= tempScaleMin) tempScaleMax = tempScaleMin + 5.0f;

  fb.setFont(&FONT_5pt8b);
  String precipLegend = String("P dots ") +
      String(precipScale, precipScale < 1.0f ? 1 : 0) + precipUnit;
  drawString(2, 163, precipLegend, LEFT, INK);
  drawString(DISP_WIDTH / 2, 163, "Temp line", CENTER, INK);
  drawString(DISP_WIDTH - 2, 163,
             String("UV bars ") + String(static_cast<int>(uvScale)), RIGHT, INK);

  // Temperature uses the left scale. Precipitation dotted bars and solid UV
  // bars are normalized to the maxima printed in the legend.
  fb.drawFastHLine(plotL, plotB, plotW + 1, INK);
  fb.drawFastVLine(plotL, plotT, plotH + 1, INK);
  fb.drawFastVLine(plotR, plotT, plotH + 1, INK);
  for (int row = 0; row <= 2; ++row)
  {
    int y = plotT + (row * plotH / 2);
    if (row < 2)
      for (int x = plotL + 1; x < plotR; x += 4) fb.drawPixel(x, y, INK);
  }

  drawString(plotL - 3, plotT + 3,
             String(static_cast<int>(tempScaleMax)) + "\260", RIGHT, INK);
  drawString(plotL - 3, plotB,
             String(static_cast<int>(tempScaleMin)) + "\260", RIGHT, INK);
  drawString(plotR + 3, plotT + 3, String(static_cast<int>(uvScale)), LEFT, INK);
  drawString(plotR + 3, plotB, "0", LEFT, INK);

  int prevX = 0;
  int prevY = 0;
  for (int i = 0; i < hours; ++i)
  {
    int x0 = plotL + 1 + (i * (plotW - 1) / rangeHours);
    int x1 = plotL + 1 + ((i + 1) * (plotW - 1) / rangeHours);
#ifdef UNITS_HOURLY_PRECIP_POP
    float precip = hourly[i].pop * 100.0f;
#else
    float precip = hourly[i].rain_1h + hourly[i].snow_1h;
#ifdef UNITS_HOURLY_PRECIP_CENTIMETERS
    precip = millimeters_to_centimeters(precip);
#elif defined(UNITS_HOURLY_PRECIP_INCHES)
    precip = millimeters_to_inches(precip);
#endif
#endif
    int midX = (x0 + x1) / 2;
    int precipTop = plotB - static_cast<int>(std::round(plotH * precip / precipScale));
    // Left half: checker/dotted precipitation bar.
    if (precip <= 0.0f)
    {
      // A small dotted zero marker distinguishes a measured zero from missing
      // data, especially during long dry periods.
      for (int x = x0 + 1; x < midX; x += 2) fb.drawPixel(x, plotB - 3, INK);
    }
    else
    {
      for (int y = plotB - 1; y >= precipTop; y -= 2)
        for (int x = x0 + 1; x < midX; x += 2) fb.drawPixel(x, y, INK);
    }

    // Right half: solid UV bar.
    int uvTop = plotB - static_cast<int>(std::round(plotH * hourly[i].uvi / uvScale));
    uvTop = std::max(plotT, std::min(plotB, uvTop));
    if (uvTop < plotB)
      fb.fillRect(midX + 1, uvTop, std::max(1, x1 - midX - 1), plotB - uvTop, INK);
    else
      fb.fillRect(midX + 1, plotB - 3, std::max(1, x1 - midX - 1), 2, INK);

#ifdef UNITS_TEMP_KELVIN
    float temp = hourly[i].temp;
#elif defined(UNITS_TEMP_CELSIUS)
    float temp = kelvin_to_celsius(hourly[i].temp);
#else
    float temp = kelvin_to_fahrenheit(hourly[i].temp);
#endif
    int tempX = (x0 + x1) / 2;
    int tempY = plotB - static_cast<int>(std::round(
        plotH * (temp - tempScaleMin) / (tempScaleMax - tempScaleMin)));
    tempY = std::max(plotT, std::min(plotB, tempY));
    // White halo keeps the temperature curve legible over both bar styles.
    if (i > 0)
    {
      fb.drawLine(prevX, prevY - 1, tempX, tempY - 1, BG);
      fb.drawLine(prevX, prevY + 1, tempX, tempY + 1, BG);
      fb.drawLine(prevX - 1, prevY, tempX - 1, tempY, BG);
      fb.drawLine(prevX + 1, prevY, tempX + 1, tempY, BG);
      fb.drawLine(prevX, prevY, tempX, tempY, INK);
    }
    fb.fillRect(tempX - 2, tempY - 2, 5, 5, BG);
    fb.fillRect(tempX - 1, tempY - 1, 3, 3, INK);
    prevX = tempX;
    prevY = tempY;

    // Midnight separator.
    time_t ts = hourly[i].dt;
    tm *hourTm = localtime(&ts);
    if (hourTm != nullptr && hourTm->tm_hour == 0)
      for (int y = plotT; y < plotB; y += 3) fb.drawPixel(x0, y, INK);
  }

  // Open-Meteo supplies six past hours followed by the current/future hours,
  // so the complete -6h..+24h window is populated.
  const time_t firstTs = hourly[0].dt;
  for (int index = 0; index <= rangeHours; index += 6)
  {
    int x = plotL + (index * plotW / rangeHours);
    time_t ts = firstTs + static_cast<time_t>(index) * 3600;
    tm *hourTm = localtime(&ts);
    char label[12] = {};
    if (hourTm != nullptr) _strftime(label, sizeof(label), HOUR_FORMAT, hourTm);
    drawString(x, 284, label, CENTER, INK);
  }

  // High-contrast current-time marker, drawn last so it remains visible over
  // either bar style and the temperature curve.
  const int nowX = plotL + (pastHours * plotW / rangeHours);
  fb.drawFastVLine(nowX - 1, plotT, plotH + 1, BG);
  fb.drawFastVLine(nowX + 1, plotT, plotH + 1, BG);
  fb.drawFastVLine(nowX, plotT, plotH + 1, INK);
  fb.fillTriangle(nowX - 3, plotT, nowX + 3, plotT, nowX, plotT + 5, INK);

  time_t nowTs = time(nullptr);
  tm *nowTm = localtime(&nowTs);
  char nowLabel[12] = {};
  if (nowTm != nullptr) _strftime(nowLabel, sizeof(nowLabel), TIME_FORMAT, nowTm);
  fb.fillRect(nowX - 20, plotT - 12, 41, 10, BG);
  drawString(nowX, plotT - 3, nowLabel, CENTER, INK);

#ifdef UNITS_TEMP_KELVIN
  float nowTemp = current.temp;
#elif defined(UNITS_TEMP_CELSIUS)
  float nowTemp = kelvin_to_celsius(current.temp);
#else
  float nowTemp = kelvin_to_fahrenheit(current.temp);
#endif
  String tempLabel = String(static_cast<int>(std::round(nowTemp))) + "\260";
  fb.fillRect(nowX + 3, plotT + 6, 27, 12, BG);
  drawString(nowX + 6, plotT + 16, tempLabel, LEFT, INK);
}

// ---- Alerts -----------------------------------------------------------------

void drawAlerts(std::vector<owm_alerts_t> &alerts, const String &city,
                const String &date)
{
  (void)city;
  (void)date;
  if (alerts.empty())
  {
    return;
  }
  // An active alert is more important than the chart. Replace the chart area
  // with readable event/headline/instruction text instead of an unexplained
  // icon. The regular weather view returns automatically when alerts expire.
  const owm_alerts_t &alert = alerts.front();
  fb.fillRect(0, MAIN_BOTTOM + 1, DISP_WIDTH, 136, BG);
  fb.fillRect(0, MAIN_BOTTOM + 3, DISP_WIDTH, 24, INK);
  fb.setFont(&FONT_8pt8b);
  String heading = "! " + alert.event;
  drawString(DISP_WIDTH / 2, MAIN_BOTTOM + 20, heading, CENTER, BG);

  fb.setFont(&FONT_7pt8b);
  String detail = alert.description;
  if (detail.isEmpty()) detail = "See local authorities for details.";
  drawMultiLnString(8, MAIN_BOTTOM + 43, detail, LEFT,
                    DISP_WIDTH - 16, 7, 15, INK);

  fb.setFont(&FONT_5pt8b);
  String source = alert.sender_name;
  if (alerts.size() > 1) source += "  +" + String(alerts.size() - 1) + " more";
  drawString(DISP_WIDTH - 5, 284, source, RIGHT, INK);
}

// ---- Status bar -------------------------------------------------------------

void drawStatusBar(const String &statusStr, const String &refreshTimeStr,
                   int rssi, uint32_t batVoltage)
{
  (void)batVoltage; // battery monitoring disabled on this board
  fb.setFont(&FONT_6pt8b);
  const int y = DISP_HEIGHT - 3;

  // last refresh time (left)
  drawString(4, y, refreshTimeStr, LEFT, INK);

  // WiFi signal (right)
  String wifiStr = String(getWiFidesc(rssi));
#if STATUS_BAR_EXTRAS_WIFI_RSSI
  if (rssi != 0)
  {
    wifiStr += " (" + String(rssi) + "dBm)";
  }
#endif
  drawString(DISP_WIDTH - 4, y, wifiStr, RIGHT, INK);

  // optional status note (e.g. sensor error), centered if present
  if (!statusStr.isEmpty())
  {
    drawString(DISP_WIDTH / 2, y, statusStr, CENTER, INK);
  }
}

// ---- Error screen -----------------------------------------------------------

void drawError(const uint8_t *bitmap_196x196, const String &errMsgLn1,
               const String &errMsgLn2)
{
  (void)bitmap_196x196; // 196x196 icon does not fit; show text only
  fb.setFont(&FONT_16pt8b);
  drawString(DISP_WIDTH / 2, DISP_HEIGHT / 2 - 10, errMsgLn1, CENTER, INK);
  if (!errMsgLn2.isEmpty())
  {
    fb.setFont(&FONT_12pt8b);
    drawString(DISP_WIDTH / 2, DISP_HEIGHT / 2 + 24, errMsgLn2, CENTER, INK);
  }
}

// ---- OTA progress screen ----------------------------------------------------

/* Full-screen firmware-update status: title, progress bar, and a status line.
 * The panel only supports full refreshes (several seconds of flashing each), so
 * callers should redraw at coarse milestones (e.g. every 25%) rather than per
 * received chunk. percent < 0 draws the bar frame without a fill or percent
 * label (used for the failure notice).
 */
void drawOtaProgress(int percent, const String &statusLine)
{
  fb.setFont(&FONT_16pt8b);
  drawString(DISP_WIDTH / 2, 104, "Firmware Update", CENTER, INK);

  const int barW = 280;
  const int barH = 26;
  const int barX = (DISP_WIDTH - barW) / 2;
  const int barY = 132;
  fb.drawRect(barX, barY, barW, barH, INK);
  fb.drawRect(barX + 1, barY + 1, barW - 2, barH - 2, INK);
  if (percent >= 0)
  {
    int pct = std::min(percent, 100);
    int fillW = (barW - 8) * pct / 100;
    if (fillW > 0)
    {
      fb.fillRect(barX + 4, barY + 4, fillW, barH - 8, INK);
    }
    fb.setFont(&FONT_12pt8b);
    drawString(DISP_WIDTH / 2, barY + barH + 32, String(pct) + "%", CENTER, INK);
  }
  fb.setFont(&FONT_8pt8b);
  drawString(DISP_WIDTH / 2, 224, statusLine, CENTER, INK);
}

// ---- Bit-banged SSD1683 panel driver ----------------------------------------
// GxEPD2's hardware-SPI path leaves BUSY stuck on the CrowPanel, so the panel is
// driven directly with the command/GPIO sequence from Elecrow's reference driver
// (github.com/Elecrow-RD/CrowPanel-ESP32-4.2-E-paper-HMI-Display-with-400-300).
// The image drawn by GxEPD2/Adafruit_GFX into fb is pushed here.

static void epdWriteByte(uint8_t d)
{
  digitalWrite(PIN_EPD_CS, LOW);
  for (uint8_t i = 0; i < 8; ++i)
  {
    digitalWrite(PIN_EPD_SCK, LOW);
    digitalWrite(PIN_EPD_MOSI, (d & 0x80) ? HIGH : LOW);
    digitalWrite(PIN_EPD_SCK, HIGH);
    d <<= 1;
  }
  digitalWrite(PIN_EPD_CS, HIGH);
}

static void epdCmd(uint8_t reg)
{
  digitalWrite(PIN_EPD_DC, LOW);
  epdWriteByte(reg);
}

static void epdData(uint8_t dat)
{
  digitalWrite(PIN_EPD_DC, HIGH);
  epdWriteByte(dat);
}

// Wait while BUSY is HIGH (active-high busy). Returns false on timeout.
static bool epdWaitBusy(uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (digitalRead(PIN_EPD_BUSY) == HIGH)
  {
    if (millis() - start > timeoutMs)
    {
      Serial.println("[epd] busy wait timeout");
      return false;
    }
    delay(1);
  }
  return true;
}

static void epdReset()
{
#ifdef CROWPANEL_GREEN_STICKER
  // Exact reset timing from Elecrow's revised (green-sticker) reference.
  digitalWrite(PIN_EPD_RST, HIGH);
  delay(10);
  digitalWrite(PIN_EPD_RST, LOW);
  delay(100);
  digitalWrite(PIN_EPD_RST, HIGH);
  delay(100);
#else
  // Longer/stronger reset than the reference (200/50/200ms) to accommodate a
  // marginal panel power ramp.
  digitalWrite(PIN_EPD_RST, HIGH);
  delay(200);
  digitalWrite(PIN_EPD_RST, LOW);
  delay(50);
  digitalWrite(PIN_EPD_RST, HIGH);
  delay(200);
#endif
}

static void epdInitFull()
{
#ifdef CROWPANEL_GREEN_STICKER
  // The current green-sticker panel is not register-compatible with the
  // original SSD1683 despite sharing the same connector pins. These values are
  // from Elecrow's 4.2_Example7_Global_Refresh reference implementation.
  epdReset();
  // Do not wait here: on this controller BUSY remains HIGH after hardware reset
  // until the initialization registers below have been loaded. Elecrow's
  // green-sticker reference likewise resets and initializes without polling.
  epdCmd(0x00); epdData(0x3F); epdData(0x4D);
  epdCmd(0x01); epdData(0x03); epdData(0x10); epdData(0x3F);
  epdData(0x3F); epdData(0x03);
  epdCmd(0x06); epdData(0x96); epdData(0x96); epdData(0x29);
  epdCmd(0x30); epdData(0x09);
  epdCmd(0x61); epdData(0x01); epdData(0x90); epdData(0x01); epdData(0x2C);
  epdCmd(0x82); epdData(0x05);
  epdCmd(0x50); epdData(0x97);
  epdCmd(0x60); epdData(0x22);
  epdCmd(0xE3); epdData(0x88);
#else
  epdReset();
  epdWaitBusy(2000);
  epdCmd(0x12); // SWRESET
  epdWaitBusy(2000);
  epdCmd(0x21); epdData(0x40); epdData(0x00); // display update control
  epdCmd(0x3C); epdData(0x05);                // border waveform
  epdCmd(0x11); epdData(0x03);                // data entry: X inc, Y inc
  // RAM address window: X in bytes (0..W/8-1), Y in lines (0..H-1)
  epdCmd(0x44);
  epdData(0x00);
  epdData((DISP_WIDTH - 1) >> 3);
  epdCmd(0x45);
  epdData(0x00);
  epdData(0x00);
  epdData((DISP_HEIGHT - 1) & 0xFF);
  epdData(((DISP_HEIGHT - 1) >> 8) & 0xFF);
  epdCmd(0x4E); // X cursor
  epdData(0x00);
  epdCmd(0x4F); // Y cursor
  epdData(0x00);
  epdData(0x00);
  epdWaitBusy(2000);
#endif
}

#ifdef CROWPANEL_GREEN_STICKER
// Full-refresh waveforms supplied with Elecrow's revised panel driver.
static const uint8_t LUT_GC[5][42] = {
  {0x01,0x14,0x0A,0x14,0x00,0x01,0x01},
  {0x01,0x54,0x0A,0x94,0x00,0x01,0x01},
  {0x01,0x54,0x0A,0x94,0x00,0x01,0x01},
  {0x01,0x94,0x0A,0x54,0x00,0x01,0x01},
  {0x01,0x94,0x0A,0x54,0x00,0x01,0x01}
};

static void epdLoadFullRefreshLut()
{
  for (uint8_t reg = 0; reg < 5; ++reg)
  {
    epdCmd(0x20 + reg);
    for (uint8_t i = 0; i < 42; ++i) epdData(LUT_GC[reg][i]);
  }
}

// Elecrow's revised controller requires both previous-image RAM (0x10) and
// current-image RAM (0x13) to be initialized before its first differential
// waveform can move the panel. BUSY may still complete if this is skipped, but
// the glass can remain unchanged. Match the vendor's EPD_Clear() sequence once
// after each ESP32 boot.
static bool epdPrimeGreenPanel()
{
  static bool primed = false;
  if (primed) return true;

  Serial.println("[epd] priming revised panel: white clear");
  epdInitFull();
  const size_t n = static_cast<size_t>((DISP_WIDTH + 7) / 8) * DISP_HEIGHT;
  epdCmd(0x10);
  for (size_t i = 0; i < n; ++i) epdData(0x00);
  epdCmd(0x13);
  for (size_t i = 0; i < n; ++i) epdData(0x00);
  epdLoadFullRefreshLut();
  epdCmd(0x17);
  epdData(0xA5);
  if (!epdWaitBusy(30000))
  {
    Serial.println("[epd] initial white clear timed out");
    return false;
  }
  primed = true;
  Serial.println("[epd] initial white clear complete");
  delay(500);
  return true;
}
#endif

void ssd1683GpioInit()
{
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);
  pinMode(PIN_EPD_PWR_AUX, OUTPUT);
  digitalWrite(PIN_EPD_PWR_AUX, HIGH);
  pinMode(PIN_EPD_SCK, OUTPUT);
  pinMode(PIN_EPD_MOSI, OUTPUT);
  pinMode(PIN_EPD_RST, OUTPUT);
  pinMode(PIN_EPD_DC, OUTPUT);
  pinMode(PIN_EPD_CS, OUTPUT);
  pinMode(PIN_EPD_BUSY, INPUT);
  digitalWrite(PIN_EPD_CS, HIGH);
  digitalWrite(PIN_EPD_SCK, LOW);
}

void ssd1683Sleep()
{
#ifdef CROWPANEL_GREEN_STICKER
  epdCmd(0x07);
  epdData(0xA5);
#else
  epdCmd(0x10); // deep sleep mode
  epdData(0x01);
#endif
  delay(10);
  digitalWrite(PIN_EPD_PWR_AUX, LOW);
}

// Diagnostic: power the panel, reset it, and poll BUSY. Prints whether BUSY
// returns to idle (LOW) after reset. Then draws a full black then white screen.
void ssd1683SelfTest()
{
  Serial.println("[selftest] powering panel (GPIO7 HIGH)");
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);
  delay(500);
  ssd1683GpioInit();
  Serial.printf("[selftest] BUSY (powered, pre-reset) = %d\n",
                digitalRead(PIN_EPD_BUSY));
  epdReset();
  for (int t = 0; t < 15; ++t)
  {
    Serial.printf("[selftest] t=%dms BUSY=%d\n", t * 100,
                  digitalRead(PIN_EPD_BUSY));
    delay(100);
  }
  // Try a full init + all-black refresh so we can see if the panel changes.
  epdInitFull();
  const size_t n = static_cast<size_t>((DISP_WIDTH + 7) / 8) * DISP_HEIGHT;
  epdCmd(0x24);
  for (size_t i = 0; i < n; ++i) epdData(0x00); // 0x00 => black
  epdCmd(0x22);
  epdData(0xF7);
  epdCmd(0x20);
  bool ok = epdWaitBusy(20000);
  Serial.printf("[selftest] all-black refresh %s\n", ok ? "complete" : "TIMED OUT");
}

// ---- Canvas lifecycle -------------------------------------------------------

void ssd1683BeginCanvas()
{
  fb.fillScreen(BG); // clear to white before drawing a new frame
}

void ssd1683ConditionPanel()
{
#ifdef CROWPANEL_GREEN_STICKER
  // A framebuffer screenshot cannot reveal retained pigment. On boot/reset,
  // drive every pixel black and then white to erase physical ghost lines before
  // the weather frame is drawn. This expensive cycle is intentionally boot-only.
  epdPrimeGreenPanel();
  const size_t n = static_cast<size_t>((DISP_WIDTH + 7) / 8) * DISP_HEIGHT;
  const uint8_t levels[2] = {0xFF, 0x00}; // revised panel: 1=black, 0=white
  for (uint8_t level : levels)
  {
    epdInitFull();
    delay(300);
    epdCmd(0x10);
    for (size_t i = 0; i < n; ++i) epdData(level);
    epdCmd(0x13);
    for (size_t i = 0; i < n; ++i) epdData(level);
    epdLoadFullRefreshLut();
    epdCmd(0x17); epdData(0xA5);
    if (!epdWaitBusy(30000)) Serial.println("[epd] conditioning refresh timed out");
    delay(500);
  }
  Serial.println("[epd] boot conditioning complete (black -> white)");
#else
  ssd1683BeginCanvas();
  ssd1683CommitCanvas();
#endif
}

void ssd1683CommitCanvas()
{
#ifdef CROWPANEL_GREEN_STICKER
  epdPrimeGreenPanel();
#endif
  epdInitFull();
#ifdef CROWPANEL_GREEN_STICKER
  // Elecrow's revised-panel example waits for the power stages/controller RAM
  // to settle after the second reset+init. Without this delay the clear works,
  // but the immediately following image write can be ignored.
  delay(300);
#endif
  // Write the framebuffer to the B/W RAM (0x24). SSD1683 RAM uses 1=white,
  // 0=black; the canvas uses 1=ink (black), so invert each byte.
  const uint8_t *buf = fb.getBuffer();
  const size_t n = static_cast<size_t>((DISP_WIDTH + 7) / 8) * DISP_HEIGHT;
#ifdef CROWPANEL_GREEN_STICKER
  Serial.println("[epd] writing revised-panel weather frame");
  epdCmd(0x50);
  epdData(0xD7);
  epdCmd(0x13);
#else
  epdCmd(0x24);
#endif
  for (size_t i = 0; i < n; ++i)
  {
#ifdef CROWPANEL_GREEN_STICKER
    // This CrowPanel revision uses 0=white, 1=black on the panel RAM, matching
    // GFXcanvas1 directly. Inverting here makes the mostly-white UI turn black.
    epdData(buf[i]);
#else
    epdData(static_cast<uint8_t>(~buf[i]));
#endif
  }
  // Full refresh. The current green-sticker controller uses the externally
  // supplied waveform and command 0x17; the original SSD1683 uses 0x22/0x20.
#ifdef CROWPANEL_GREEN_STICKER
  epdLoadFullRefreshLut();
  epdCmd(0x17);
  epdData(0xA5);
#else
  epdCmd(0x22);
  epdData(0xF7);
  epdCmd(0x20);
#endif
  if (!epdWaitBusy(20000))
  {
    Serial.println("[epd] refresh timed out (panel BUSY stuck)");
  }
  else
  {
    Serial.println("[epd] weather frame refresh complete");
  }
#ifdef CROWPANEL_GREEN_STICKER
  // Match the vendor example: keep both display rails up briefly after BUSY
  // returns idle before issuing deep sleep and cutting panel power.
  delay(500);
#endif
}

// ---- Screenshot: 1-bit BMP of the canvas ------------------------------------

static void putLE16(uint8_t *p, uint16_t v)
{
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}
static void putLE32(uint8_t *p, uint32_t v)
{
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

size_t ssd1683BuildBmp(std::vector<uint8_t> &out)
{
  const int w = DISP_WIDTH;
  const int h = DISP_HEIGHT;
  const int srcRowBytes = (w + 7) / 8;         // canvas row stride (50)
  const int bmpRowBytes = ((w + 31) / 32) * 4; // BMP rows padded to 4 bytes (52)
  const int headerSize = 14 + 40 + 8;          // file + info + 2-color palette
  const int pixelSize = bmpRowBytes * h;
  const int fileSize = headerSize + pixelSize;

  out.assign(fileSize, 0);
  uint8_t *p = out.data();

  // BITMAPFILEHEADER
  p[0] = 'B';
  p[1] = 'M';
  putLE32(p + 2, fileSize);
  putLE32(p + 10, headerSize); // offset to pixel data
  // BITMAPINFOHEADER
  putLE32(p + 14, 40);
  putLE32(p + 18, w);
  putLE32(p + 22, h); // positive height => bottom-up rows
  putLE16(p + 26, 1); // planes
  putLE16(p + 28, 1); // bits per pixel
  putLE32(p + 30, 0); // BI_RGB (uncompressed)
  putLE32(p + 34, pixelSize);
  putLE32(p + 38, 2835); // ~72 DPI
  putLE32(p + 42, 2835);
  putLE32(p + 46, 2); // colors in palette
  putLE32(p + 50, 2);
  // palette: index 0 = white, index 1 = black (BGRA)
  p[54] = 0xFF; p[55] = 0xFF; p[56] = 0xFF; p[57] = 0x00;
  p[58] = 0x00; p[59] = 0x00; p[60] = 0x00; p[61] = 0x00;

  // pixel data (bottom-up). Canvas bit 1 = ink -> palette index 1 = black.
  const uint8_t *buf = fb.getBuffer();
  for (int row = 0; row < h; ++row)
  {
    const uint8_t *src = buf + static_cast<size_t>(h - 1 - row) * srcRowBytes;
    uint8_t *dst = p + headerSize + static_cast<size_t>(row) * bmpRowBytes;
    memcpy(dst, src, srcRowBytes); // trailing pad bytes remain 0
  }
  return fileSize;
}

/* Standard base64 (no line wrapping) straight to Serial. */
static void serialPrintBase64(const uint8_t *data, size_t len)
{
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char q[4];
  size_t i = 0;
  for (; i + 3 <= len; i += 3)
  {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8) |
                 static_cast<uint32_t>(data[i + 2]);
    q[0] = tbl[(n >> 18) & 63];
    q[1] = tbl[(n >> 12) & 63];
    q[2] = tbl[(n >> 6) & 63];
    q[3] = tbl[n & 63];
    Serial.write(reinterpret_cast<uint8_t *>(q), 4);
  }
  size_t rem = len - i;
  if (rem == 1)
  {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    q[0] = tbl[(n >> 18) & 63];
    q[1] = tbl[(n >> 12) & 63];
    q[2] = '=';
    q[3] = '=';
    Serial.write(reinterpret_cast<uint8_t *>(q), 4);
  }
  else if (rem == 2)
  {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8);
    q[0] = tbl[(n >> 18) & 63];
    q[1] = tbl[(n >> 12) & 63];
    q[2] = tbl[(n >> 6) & 63];
    q[3] = '=';
    Serial.write(reinterpret_cast<uint8_t *>(q), 4);
  }
}

void ssd1683SerialDumpScreenshot()
{
  std::vector<uint8_t> bmp;
  ssd1683BuildBmp(bmp);
  Serial.println();
  Serial.println("---SCREENSHOT-BMP-BASE64-BEGIN---");
  serialPrintBase64(bmp.data(), bmp.size());
  Serial.println();
  Serial.println("---SCREENSHOT-BMP-BASE64-END---");
}

#endif // DISP_SSD1683
