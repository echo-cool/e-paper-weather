/* Over-the-air (OTA) firmware update implementation for esp32-weather-epd.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */

#include "ota.h"

#if OTA_ENABLED

#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>

#include "config.h"
#include "location.h"
#include "renderer.h" // ssd1683BuildBmp() for the screenshot endpoint

static WebServer server(80);
static volatile bool otaBusy = false;

static String htmlEscape(const String &value)
{
  String out = value;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

/* Minimal upload page served at "/". */
static String rootPage()
{
  String ip = WiFi.localIP().toString();
  String html =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>esp32-weather-epd OTA</title>"
      "<style>body{font-family:sans-serif;max-width:32rem;margin:2rem auto;"
      "padding:0 1rem;color:#222}h1{font-size:1.3rem}.card{border:1px solid #ccc;"
      "border-radius:8px;padding:1rem}input[type=submit]{margin-top:.75rem;"
      "padding:.5rem 1rem}</style></head><body>"
      "<h1>CrowPanel Weather &mdash; Firmware Update</h1>"
      "<div class='card'>"
      "<p>Firmware: <b>" FW_VERSION "</b><br>Device: " +
      ip +
      "</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin' required>"
      "<br><input type='submit' value='Upload &amp; flash'>"
      "</form>"
      "<p style='color:#888;font-size:.85rem'>Upload a .bin built for this "
      "board. The device reboots automatically when the flash completes.</p>"
      "</div>"
      "<h2 style='font-size:1.1rem'>Weather location</h2>"
      "<div class='card'><p>Current: <b>" + htmlEscape(CITY_STRING) +
      "</b></p><form method='POST' action='/location'>"
      "<label for='city'>City or postal code</label><br>"
      "<input id='city' name='city' value='" + htmlEscape(LOCATION_QUERY) +
      "' required style='box-sizing:border-box;width:100%;padding:.5rem;margin-top:.35rem'>"
      "<br><input type='submit' value='Save location &amp; reboot'></form>"
      "<p style='color:#888;font-size:.85rem'>Examples: Santa Clara 95051, London, Tokyo.</p>"
      "</div>"
#ifdef DISP_SSD1683
      "<h2 style='font-size:1.1rem'>Current screen</h2>"
      "<div class='card'>"
      "<p><a href='/screenshot.bmp'>screenshot.bmp</a></p>"
      "<img src='/screenshot.bmp' alt='display screenshot' "
      "style='width:100%;image-rendering:pixelated;border:1px solid #ccc'>"
      "</div>"
#endif
      "</body></html>";
  return html;
}

void initOTA()
{
  // --- ArduinoOTA (espota / IDE network upload) ---
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (OTA_PASSWORD != nullptr && strlen(OTA_PASSWORD) > 0)
  {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }
  ArduinoOTA.onStart([]() {
    otaBusy = true;
    Serial.println("[ota] ArduinoOTA start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[ota] ArduinoOTA end");
    otaBusy = false;
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[ota] %u%%\r", total ? (progress * 100u / total) : 0u);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaBusy = false;
    Serial.printf("[ota] error %u\n", error);
  });
  ArduinoOTA.begin();

  // --- Web-browser upload ---
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", rootPage());
  });

  server.on("/location", HTTP_POST, []() {
    String message;
    bool ok = server.hasArg("city") && setLocationFromQuery(server.arg("city"), message);
    server.sendHeader("Connection", "close");
    server.send(ok ? 200 : 400, "text/html",
                "<!doctype html><meta name='viewport' content='width=device-width'>"
                "<h1>" + htmlEscape(message) + "</h1><a href='/'>Back</a>");
    if (ok) { delay(750); ESP.restart(); }
  });

#ifdef DISP_SSD1683
  // --- Screenshot of the current display (1-bit BMP) ---
  server.on("/screenshot.bmp", HTTP_GET, []() {
    std::vector<uint8_t> bmp;
    ssd1683BuildBmp(bmp);
    server.setContentLength(bmp.size());
    server.send(200, "image/bmp", "");
    server.sendContent(reinterpret_cast<const char *>(bmp.data()), bmp.size());
  });
#endif
  server.on(
      "/update", HTTP_POST,
      []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/html",
                    Update.hasError()
                        ? "<h1>Update failed.</h1><a href='/'>Back</a>"
                        : "<h1>Update OK &mdash; rebooting...</h1>");
        delay(500);
        ESP.restart();
      },
      []() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
          otaBusy = true;
          Serial.printf("[ota] web update: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN))
          {
            Update.printError(Serial);
          }
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
          {
            Update.printError(Serial);
          }
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
          if (Update.end(true))
          {
            Serial.printf("[ota] web update success: %u bytes\n",
                          upload.totalSize);
          }
          else
          {
            Update.printError(Serial);
          }
          otaBusy = false;
        }
        else if (upload.status == UPLOAD_FILE_ABORTED)
        {
          Update.end();
          otaBusy = false;
          Serial.println("[ota] web update aborted");
        }
      });
  server.begin();

  Serial.print("[ota] ready at http://");
  Serial.print(WiFi.localIP());
  Serial.print("/  (hostname: ");
  Serial.print(OTA_HOSTNAME);
  Serial.println(".local)");
}

void handleOTA()
{
  ArduinoOTA.handle();
  server.handleClient();
}

bool otaInProgress()
{
  return otaBusy;
}

#endif // OTA_ENABLED
