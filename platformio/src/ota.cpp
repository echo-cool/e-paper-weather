/* Over-the-air (OTA) firmware update implementation for esp32-weather-epd.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */

#include "ota.h"

#if OTA_ENABLED

#include <algorithm>
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

#ifdef DISP_SSD1683
// On-panel update feedback. The SSD1683 only does full refreshes (several
// seconds of flashing each), so the progress bar advances in 25% milestones
// instead of per received chunk: 0% at start, then 25/50/75, then a final
// "complete - rebooting" (or failure) frame.
static int otaShownPct = -1;         // last milestone drawn on the panel
static size_t webUpdateTotal = 0;    // Content-Length of the running web upload

static void otaShowScreen(int percent, const String &statusLine)
{
  initDisplay();
  ssd1683BeginCanvas();
  drawOtaProgress(percent, statusLine);
  ssd1683CommitCanvas();
  powerOffDisplay();
}

static void otaShowProgress(size_t received, size_t total)
{
  if (total == 0) return; // size unknown; the 0% start frame is already up
  int pct = static_cast<int>(static_cast<uint64_t>(received) * 100 / total);
  int milestone = std::min(pct, 99) / 25 * 25;
  if (milestone > otaShownPct)
  {
    otaShownPct = milestone;
    otaShowScreen(milestone, "Receiving firmware - do not power off");
  }
}
#endif // DISP_SSD1683

static String htmlEscape(const String &value)
{
  String out = value;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

// JSON-escape a string for embedding in the /api/status response.
static String jsonEscape(const String &value)
{
  String out;
  out.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); i++)
  {
    char c = value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20)
    {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    }
    else out += c;
  }
  return out;
}

// Basic-auth gate for state-changing endpoints; a no-op while OTA_PASSWORD is
// empty (the default). Mirrors the ESP32WorldClock web UI.
static bool webAuthenticate()
{
  if (OTA_PASSWORD == nullptr || strlen(OTA_PASSWORD) == 0) return true;
  if (server.authenticate("admin", OTA_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

// Set by POST /api/refresh; consumed by the main loop, which then runs a full
// weather update outside any HTTP handler.
static volatile bool refreshRequested = false;

bool otaConsumePendingRefresh()
{
  if (!refreshRequested) return false;
  refreshRequested = false;
  return true;
}

/* Control-center page served at "/". Placeholders (%FW%, %IP%, ...) are
 * substituted in rootPage(). Visual language follows the ESP32WorldClock
 * settings page so the two devices feel like siblings. Fully self-contained:
 * no external assets, works without internet.
 */
static const char ROOT_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CrowPanel Weather</title>
<style>
:root{color-scheme:dark;--bg:#07111f;--panel:#0f1c2e;--panel2:#14243a;--line:#263a55;--text:#f4f7fb;--muted:#91a4bd;--blue:#55a7ff;--green:#40d89a;--amber:#ffbe55;--red:#ff6577}
*{box-sizing:border-box}
body{margin:0;padding:24px;font-family:Inter,ui-sans-serif,system-ui,-apple-system,sans-serif;background:radial-gradient(circle at 12% 0,#12345a 0,transparent 34rem),var(--bg);color:var(--text)}
.card{width:min(880px,100%);margin:auto;background:#0a1626dd;border:1px solid var(--line);border-radius:24px;padding:clamp(18px,3vw,36px);box-shadow:0 20px 55px #02071180}
.hero{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:20px}
.eyebrow{color:var(--blue);font-size:.72rem;font-weight:800;letter-spacing:.14em;text-transform:uppercase;margin:0 0 7px}
h1{font-size:clamp(1.6rem,4vw,2.3rem);letter-spacing:-.04em;margin:0}
p{color:var(--muted);font-size:.86rem;line-height:1.55;margin:.4rem 0}
a{color:var(--blue);text-decoration:none}
code{color:#c4d0df}
.device-pill{flex:none;text-align:right;padding:9px 13px;border:1px solid var(--line);border-radius:14px;background:var(--panel);font-size:.8rem;color:var(--muted)}
.device-pill b{color:var(--green)}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}
fieldset{min-width:0;border:1px solid var(--line);border-radius:16px;padding:10px 18px 18px;background:var(--panel);margin:0}
legend{color:var(--blue);font-size:.74rem;font-weight:800;letter-spacing:.1em;text-transform:uppercase;padding:0 7px}
.wide{grid-column:1/-1}
label{display:block;color:#c4d0df;font-size:.8rem;font-weight:650;margin:.85rem 0 0}
input[type=text],input[type=file]{width:100%;margin:.32rem 0 0;padding:.62rem .7rem;background:#0a1626;color:var(--text);border:1px solid #314761;border-radius:9px;font:inherit}
input:focus{outline:2px solid #55a7ff77;border-color:var(--blue)}
button{width:100%;margin-top:1rem;padding:.7rem 1rem;border:0;border-radius:9px;background:#2588f5;color:#fff;font-size:.9rem;font-weight:750;cursor:pointer}
button:hover{filter:brightness(1.08)}button:disabled{opacity:.45;cursor:not-allowed}
button.secondary{background:#263a51}
.screen-frame{margin-top:12px;padding:12px;background:#e9edf2;border-radius:12px}
.screen-frame img{display:block;width:100%;image-rendering:pixelated}
.btn-row{display:flex;gap:10px}.btn-row button{flex:1}
.stats{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px;margin-top:12px}
.stat{padding:10px 12px;background:var(--panel2);border:1px solid var(--line);border-radius:10px}
.stat small{display:block;color:var(--muted);font-size:.68rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase}
.stat span{font-size:.92rem;font-weight:700;word-break:break-word}
#bar{height:10px;background:#0a1626;border:1px solid #314761;border-radius:5px;overflow:hidden;margin:1rem 0 .4rem;display:none}
#fill{height:100%;width:0;background:var(--green);transition:width .15s}
.msg{min-height:1.1em;font-size:.85rem;margin-top:.5rem;color:var(--muted)}
.ok{color:var(--green)}.err{color:var(--red)}.warn{color:var(--amber)}
@media(max-width:700px){body{padding:10px}.card{border-radius:18px;padding:16px}.grid{grid-template-columns:1fr}.hero{flex-direction:column;align-items:flex-start}.device-pill{text-align:left}}
</style></head><body><div class="card">
<header class="hero"><div><p class="eyebrow">Control center</p>
<h1>CrowPanel Weather</h1>
<p>E-paper weather station &middot; firmware %FW%</p></div>
<div class="device-pill"><b>&#9679; Online</b>&nbsp; %HOST%.local<br>%IP%</div></header>
<div class="grid">

<fieldset class="wide"><legend>Current screen</legend>
<p>Live copy of the 400&times;300 e-paper panel. It redraws on its own every %REF% minutes.</p>
<div class="screen-frame"><img id="shot" src="/screenshot.bmp" alt="display screenshot"></div>
<div class="btn-row">
<button class="secondary" type="button" onclick="reloadShot()">Reload image</button>
<button type="button" id="rf">Refresh weather now</button></div>
<div class="msg" id="rfmsg"></div></fieldset>

<fieldset><legend>Weather location</legend>
<p>Current: <b style="color:var(--text)">%CITY%</b></p>
<form id="locf"><label>City or postal code
<input type="text" id="city" value="%LOCQ%" required placeholder="Santa Clara 95051"></label>
<button type="submit">Save location &amp; reboot</button></form>
<p>Examples: Santa Clara 95051, London, Tokyo. Saving reboots the display (&asymp;1 minute).</p>
<div class="msg" id="locmsg"></div></fieldset>

<fieldset><legend>Firmware update</legend>
<p>Upload a <code>firmware.bin</code> built for this board. The e-paper panel
shows progress in 25% steps; keep the device powered until it reboots.</p>
<form id="fwf"><input type="file" id="fw" accept=".bin" required>
<button id="fwbtn" type="submit">Upload &amp; flash</button></form>
<div id="bar"><div id="fill"></div></div>
<div class="msg" id="fwmsg"></div></fieldset>

<fieldset class="wide"><legend>Device status</legend>
<div class="stats" id="stats"><div class="stat"><small>Status</small><span>Loading&hellip;</span></div></div>
</fieldset>

</div></div>
<script>
var $=function(id){return document.getElementById(id)};
function reloadShot(){$('shot').src='/screenshot.bmp?t='+Date.now()}
setInterval(reloadShot,60000);

// Refresh now: the POST returns immediately; the follow-up status fetch only
// completes once the (single-threaded) device finishes fetching + redrawing.
$('rf').onclick=function(){
  $('rf').disabled=true;$('rfmsg').textContent='Refreshing - the panel flashes while it redraws...';$('rfmsg').className='msg warn';
  fetch('/api/refresh',{method:'POST'}).then(function(r){if(!r.ok)throw 0;
    return fetch('/api/status',{cache:'no-store'});
  }).then(function(){
    reloadShot();$('rf').disabled=false;
    $('rfmsg').textContent='Panel updated.';$('rfmsg').className='msg ok';
  }).catch(function(){
    $('rf').disabled=false;
    $('rfmsg').textContent='Refresh request failed.';$('rfmsg').className='msg err';
  })};

$('locf').addEventListener('submit',function(e){e.preventDefault();
  var b=new URLSearchParams();b.set('city',$('city').value);
  $('locmsg').textContent='Saving...';$('locmsg').className='msg warn';
  fetch('/location',{method:'POST',body:b}).then(function(r){
    return r.text().then(function(t){return{ok:r.ok,text:t}});
  }).then(function(res){
    if(!res.ok){$('locmsg').textContent=res.text||'Could not set that location.';$('locmsg').className='msg err';return}
    $('locmsg').textContent=res.text+' - rebooting; this page reloads when the display is back.';$('locmsg').className='msg ok';
    setTimeout(function poll(){fetch('/api/status',{cache:'no-store'})
      .then(function(){location.reload()})
      .catch(function(){setTimeout(poll,4000)})},20000);
  }).catch(function(){$('locmsg').textContent='Request failed.';$('locmsg').className='msg err'})});

$('fwf').addEventListener('submit',function(e){e.preventDefault();
  var f=$('fw').files[0];if(!f)return;
  $('fwbtn').disabled=true;$('bar').style.display='block';$('fill').style.width='0';
  $('fwmsg').textContent='Uploading...';$('fwmsg').className='msg warn';
  var x=new XMLHttpRequest();
  // The exact file size rides along as ?size= so the on-panel percentage is
  // exact (multipart Content-Length slightly overshoots).
  x.open('POST','/update?size='+f.size);
  x.upload.onprogress=function(ev){if(ev.lengthComputable)$('fill').style.width=Math.round(ev.loaded*100/ev.total)+'%'};
  x.onload=function(){
    if(x.status==200){$('fwmsg').textContent='Success - the display is rebooting; this page reloads when it is back.';$('fwmsg').className='msg ok';
      setTimeout(function poll(){fetch('/api/status',{cache:'no-store'})
        .then(function(){location.reload()})
        .catch(function(){setTimeout(poll,4000)})},30000);}
    else{$('fwmsg').textContent='Update failed: '+x.responseText;$('fwmsg').className='msg err';$('fwbtn').disabled=false}};
  x.onerror=function(){$('fwmsg').textContent='Connection lost during upload.';$('fwmsg').className='msg err';$('fwbtn').disabled=false};
  var d=new FormData();d.append('firmware',f);x.send(d)});

function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+h+'h '+m+'m'}
function wifiDesc(r){return r>=-55?'Excellent':r>=-65?'Good':r>=-75?'Fair':'Weak'}
function esc(v){var d=document.createElement('div');d.textContent=v;return d.innerHTML}
function stat(label,val){return '<div class="stat"><small>'+label+'</small><span>'+val+'</span></div>'}
function loadStatus(){fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){
  $('stats').innerHTML=
    stat('Firmware',esc(s.fw))+stat('Build',esc(s.build))+
    stat('City',esc(s.city))+stat('Refresh every',s.refreshMin+' min')+
    stat('Wi-Fi',esc(s.ssid))+stat('Signal',s.rssi+' dBm ('+wifiDesc(s.rssi)+')')+
    stat('IP address',esc(s.ip))+stat('Uptime',fmtUptime(s.uptimeSec))+
    stat('Free heap',Math.round(s.heapFree/1024)+' KB')+stat('Chip',esc(s.chip));
}).catch(function(){})}
loadStatus();setInterval(loadStatus,15000);
</script></body></html>)HTML";

static String rootPage()
{
  String page = FPSTR(ROOT_PAGE);
  page.replace("%FW%", FW_VERSION);
  page.replace("%HOST%", OTA_HOSTNAME);
  page.replace("%IP%", WiFi.localIP().toString());
  page.replace("%REF%", String(SLEEP_DURATION));
  page.replace("%CITY%", htmlEscape(CITY_STRING));
  page.replace("%LOCQ%", htmlEscape(LOCATION_QUERY));
  return page;
}

/* Diagnostics as JSON for the status grid on the page (and for scripts). */
static void handleApiStatus()
{
  String j;
  j.reserve(384);
  j += "{\"fw\":\"" FW_VERSION "\",\"build\":\"" __DATE__ " " __TIME__ "\"";
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"host\":\"" + String(OTA_HOSTNAME) + "\"";
  j += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"city\":\"" + jsonEscape(CITY_STRING) + "\"";
  j += ",\"refreshMin\":" + String(SLEEP_DURATION);
  j += ",\"uptimeSec\":" + String(millis() / 1000UL);
  j += ",\"heapFree\":" + String(ESP.getFreeHeap());
  j += ",\"chip\":\"" + String(ESP.getChipModel()) + "\"}";
  server.send(200, "application/json", j);
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
#ifdef DISP_SSD1683
    otaShownPct = 0;
    otaShowScreen(0, "Receiving firmware - do not power off");
#endif
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[ota] ArduinoOTA end");
    otaBusy = false;
#ifdef DISP_SSD1683
    otaShowScreen(100, "Update complete - rebooting");
#endif
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[ota] %u%%\r", total ? (progress * 100u / total) : 0u);
#ifdef DISP_SSD1683
    otaShowProgress(progress, total);
#endif
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaBusy = false;
    Serial.printf("[ota] error %u\n", error);
#ifdef DISP_SSD1683
    if (otaShownPct >= 0)
    { // a transfer had started: replace the stale progress frame, then reboot
      // back into the (unchanged) current firmware to redraw the weather.
      otaShownPct = -1;
      otaShowScreen(-1, "Update failed - rebooting");
      delay(1000);
      ESP.restart();
    }
#endif
  });
  ArduinoOTA.begin();

  // --- Web control center ---
  server.on("/", HTTP_GET, []() {
    if (!webAuthenticate()) return;
    server.send(200, "text/html", rootPage());
  });

  server.on("/api/status", HTTP_GET, handleApiStatus);

  // Queue an immediate weather refresh; the main loop performs it so the
  // half-minute fetch+redraw doesn't run inside an HTTP handler.
  server.on("/api/refresh", HTTP_POST, []() {
    if (!webAuthenticate()) return;
    refreshRequested = true;
    server.send(200, "text/plain", "ok");
  });

  // Plain-text response: the page posts this via fetch() and shows the
  // message inline.
  server.on("/location", HTTP_POST, []() {
    if (!webAuthenticate()) return;
    String message;
    bool ok = server.hasArg("city") && setLocationFromQuery(server.arg("city"), message);
    server.sendHeader("Connection", "close");
    server.send(ok ? 200 : 400, "text/plain", message);
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
        bool failed = Update.hasError();
        server.sendHeader("Connection", "close");
        if (failed)
          server.send(500, "text/plain", "flash write failed - device reboots into the current firmware");
        else
          server.send(200, "text/plain", "OK - rebooting");
#ifdef DISP_SSD1683
        otaShowScreen(failed ? -1 : 100,
                      failed ? "Update failed - rebooting"
                             : "Update complete - rebooting");
#endif
        delay(500);
        ESP.restart();
      },
      []() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
          otaBusy = true;
          Serial.printf("[ota] web update: %s\n", upload.filename.c_str());
#ifdef DISP_SSD1683
          // The page passes the exact file size as ?size=. Fall back to the
          // multipart Content-Length (a few hundred bytes of boundary
          // overhead), which is negligible at 25% milestones.
          webUpdateTotal = server.hasArg("size")
                               ? static_cast<size_t>(server.arg("size").toInt())
                               : 0;
          if (webUpdateTotal == 0)
            webUpdateTotal = static_cast<size_t>(
                std::max(server.clientContentLength(), 0));
          otaShownPct = 0;
          otaShowScreen(0, "Receiving firmware - do not power off");
#endif
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
#ifdef DISP_SSD1683
          otaShowProgress(upload.totalSize, webUpdateTotal);
#endif
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
#ifdef DISP_SSD1683
          // The completion handler never runs for an aborted upload, so the
          // panel would keep showing a stale progress bar. Reboot back into
          // the (unchanged) current firmware to redraw the weather.
          otaShowScreen(-1, "Update failed - rebooting");
          delay(1000);
          ESP.restart();
#endif
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
