/* Over-the-air (OTA) firmware update declarations for esp32-weather-epd.
 *
 * Provides two update paths, both using libraries bundled with the ESP32
 * Arduino core (no extra dependencies):
 *   1. ArduinoOTA  - lets you upload from PlatformIO / Arduino IDE over WiFi
 *                    (`upload_protocol = espota`).
 *   2. A small web page at http://<device-ip>/ for drag-and-drop .bin uploads
 *      from any browser (no toolchain required).
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */

#ifndef __OTA_H__
#define __OTA_H__

#include "config.h"

#if OTA_ENABLED

/* Start the ArduinoOTA responder and the web-update server. Call once after
 * WiFi is connected. Logs the reachable URL to Serial.
 */
void initOTA();

/* Service pending OTA activity. Call frequently from loop(). */
void handleOTA();

/* True while a firmware upload is in progress, so the caller can avoid
 * blocking work (e.g. a display refresh) mid-upload.
 */
bool otaInProgress();

#endif // OTA_ENABLED
#endif // __OTA_H__
