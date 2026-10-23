/*
 * ESP32-S3 WiFi Keystroke Typer  —  Pocket-Dongle-S3 (ST7735 160x80)
 * WiFiManager style: joins your home WiFi, or falls back to a setup hotspot.
 * ------------------------------------------------------------------------
 * FIRST BOOT (or if home WiFi fails):
 *   - Dongle starts its own hotspot:  ESP32_Typer / 12345678
 *   - Join it, a setup page opens (or go to http://192.168.4.1)
 *   - Enter your home WiFi name + password -> Save. It reboots and joins.
 * NORMAL BOOT:
 *   - Joins your saved home WiFi. Open http://esptype.local (IP on screen).
 * USE:
 *   - Plug into the TARGET PC (it's a USB keyboard). Focus a window,
 *     paste text/code on the page, Send. Screen shows line progress.
 *   - To change WiFi later: open the page and use "Forget WiFi".
 *
 * Arduino IDE: Board = ESP32S3 Dev Module, USB Mode = USB-OTG (TinyUSB),
 *   USB CDC On Boot = Enabled. Libraries: Adafruit GFX + Adafruit ST7735/ST7789.
 * After flashing, fully UNPLUG and REPLUG (a reset won't re-enumerate USB).
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#endif

// ====== Fallback hotspot (setup mode) ======
const char* AP_SSID  = "ESP32_Typer";
const char* AP_PASS  = "12345678";     // >= 8 chars
const char* HOSTNAME = "esptype";      // -> http://esptype.local
const IPAddress AP_IP(192, 168, 4, 1);
// ===========================================

// Display pins (software SPI) + origin offset
#define LCD_MOSI 11
#define LCD_SCLK 10
#define LCD_CS   12
#define LCD_DC   13
#define LCD_RST  14
#define LCD_X_OFF 1
#define LCD_Y_OFF 26

class LcdPanel : public Adafruit_ST7735 {
public:
  using Adafruit_ST7735::Adafruit_ST7735;
  void setOrigin(int16_t x, int16_t y) { _xstart = x; _ystart = y; }
  void clearRam() {
    static const int16_t o[4][2] = {{0, 0}, {2, 0}, {0, 52}, {2, 52}};
    for (int i = 0; i < 4; i++) { setOrigin(o[i][0], o[i][1]); fillScreen(ST77XX_BLACK); }
  }
};

LcdPanel lcd(LCD_CS, LCD_DC, LCD_MOSI, LCD_SCLK, LCD_RST);

USBHIDKeyboard Keyboard;
WebServer server(80);
DNSServer dns;
Preferences prefs;

int defaultDelayMs = 8;
bool apMode = false;          // true = setup hotspot, false = joined home WiFi
String ipLine = "";
String netName = "";

// ---------- Credential storage (NVS) ----------
void loadCreds(String& s, String& p) {
  prefs.begin("wifi", true);
  s = prefs.getString("ssid", "");
  p = prefs.getString("pass", "");
  prefs.end();
}
void saveCreds(const String& s, const String& p) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}
void clearCreds() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

// ---------- Display ----------
void lcdInit() {
  lcd.initR(INITR_MINI160x80_PLUGIN);
  lcd.setRotation(1);
  lcd.clearRam();
  lcd.setOrigin(LCD_X_OFF, LCD_Y_OFF);
  lcd.fillScreen(ST77XX_BLACK);
  lcd.setTextWrap(false);
}

void drawHeader() {
  lcd.fillScreen(ST77XX_BLACK);
  lcd.fillRect(0, 0, 160, 12, ST77XX_GREEN);   // top bar: green
  lcd.setTextSize(1);
  lcd.setTextColor(ST77XX_BLACK);              // black text on green reads best
  lcd.setCursor(34, 2); lcd.print("ESP32 Typer");

  if (apMode) {
    lcd.setTextColor(ST77XX_ORANGE);
    lcd.setCursor(2, 14); lcd.print("SETUP MODE");
    lcd.setTextColor(ST77XX_CYAN);
    lcd.setCursor(2, 24); lcd.print("join:"); lcd.print(AP_SSID);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setCursor(2, 34); lcd.print("pass:"); lcd.print(AP_PASS);
    lcd.setTextColor(ST77XX_GREEN);
    lcd.setCursor(2, 44); lcd.print("go:  "); lcd.print(ipLine);
  } else {
    lcd.setTextColor(ST77XX_CYAN);
    lcd.setCursor(2, 15); lcd.print("net:"); lcd.print(netName);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setCursor(2, 25); lcd.print(String(HOSTNAME) + ".local");
    lcd.setTextColor(ST77XX_GREEN);
    lcd.setCursor(2, 35); lcd.print("ip:  "); lcd.print(ipLine);
  }
}

void drawStatus(const String& msg, int pct, int ts = 2) {
  lcd.fillRect(0, 46, 160, 34, ST77XX_BLACK);
  lcd.setTextSize(ts);
  lcd.setTextColor(ST77XX_YELLOW);
  lcd.setCursor(2, 47);
  lcd.print(msg);

  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  int x = 2, y = 65, w = 156, h = 13;
  lcd.drawRect(x, y, w, h, ST77XX_WHITE);
  lcd.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, ST77XX_BLUE);
}

// ---------- Typing ----------
void typeText(const String& text, int d) {
  if (d < 0) d = 0;
  int totalLines = 1;
  for (size_t i = 0; i < text.length(); i++) if (text[i] == '\n') totalLines++;

  int curLine = 1;
  size_t n = text.length();
  drawStatus("Line 1/" + String(totalLines), 0);

  for (size_t i = 0; i < n; i++) {
    Keyboard.write(text[i]);     // '\n' -> Enter, '\t' -> Tab
    delay(d);
    if (text[i] == '\n') {
      curLine++;
      int pct = (int)((i + 1) * 100 / (n == 0 ? 1 : n));
      drawStatus("Line " + String(curLine) + "/" + String(totalLines), pct);
    }
  }
  drawStatus("Done: " + String(n) + " chars", 100, 1);
}

// ---------- Web pages ----------
const char TYPER_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 Typer</title>
<style>
 :root{color-scheme:dark}
 body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:640px;margin:auto}
 h1{font-size:18px;font-weight:600}
 .ed{display:flex;border:1px solid #333;border-radius:8px;overflow:hidden;background:#0b0c0f;height:240px;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px;line-height:19px}
 .g{padding:10px 8px;text-align:right;color:#5b616e;background:#0a0b0e;border-right:1px solid #333;white-space:pre;overflow:hidden;user-select:none;min-width:38px}
 #t{flex:1;border:0;outline:0;resize:none;background:transparent;color:#eee;padding:10px;white-space:pre;overflow:auto;font:inherit;tab-size:4}
 .row{display:flex;align-items:center;gap:10px;margin:12px 0}
 button{background:#3b82f6;color:#fff;border:0;border-radius:8px;padding:12px 20px;font-size:15px;cursor:pointer;flex:1}
 button.sec{background:#374151;flex:0 0 auto;padding:12px 18px}
 button:disabled{opacity:.5}
 input[type=range]{flex:1}
 #status{min-height:20px;font-size:14px;color:#9ca3af}
 label{font-size:14px;color:#9ca3af}
 a{color:#60a5fa;font-size:13px}
 .credit{color:#5b616e;font-size:12px;margin-top:14px}
 .credit b{color:#9ca3af}
</style></head><body>
<h1>ESP32 Keystroke Typer</h1>
<div class="ed"><div class="g" id="g">1</div><textarea id="t" spellcheck="false" wrap="off" placeholder="Paste text or code here..."></textarea></div>
<div class="row"><label>Speed</label><input type="range" id="d" min="0" max="40" value="8"><span id="dv">8ms/char</span></div>
<div class="row"><button class="sec" onclick="clr()">Clear</button><button class="sec" onclick="pst()">Paste</button><button id="b" onclick="send()">Send to Device</button></div>
<div id="status">Focus the target window before sending.</div>
<p class="credit">Made by <b>poordevman</b> &middot; <a href="/wifi">WiFi settings</a></p>
<script>
 const t=document.getElementById('t'),d=document.getElementById('d'),dv=document.getElementById('dv'),g=document.getElementById('g');
 d.oninput=()=>dv.textContent=d.value+'ms/char';
 function gut(){let L=t.value.split('\n').length,s='';for(let i=1;i<=L;i++)s+=i+'\n';g.textContent=s;g.scrollTop=t.scrollTop;}
 t.addEventListener('input',gut);t.addEventListener('scroll',()=>g.scrollTop=t.scrollTop);
 t.addEventListener('keydown',e=>{if(e.key==='Tab'){e.preventDefault();const s=t.selectionStart,x=t.selectionEnd;t.value=t.value.slice(0,s)+'\t'+t.value.slice(x);t.selectionStart=t.selectionEnd=s+1;gut();}});
 gut();
 async function pst(){const s=document.getElementById('status');t.focus();
   try{const x=await navigator.clipboard.readText();t.setRangeText(x,t.selectionStart,t.selectionEnd,'end');gut();s.textContent='Pasted.';}
   catch(e){s.textContent='Press Ctrl+V to paste (browser blocks the button on http).';}}
 function clr(){t.value='';gut();document.getElementById('status').textContent='Cleared.';t.focus();}
 async function send(){
   const v=t.value,b=document.getElementById('b'),s=document.getElementById('status');
   if(!v){s.textContent='Nothing to send.';return;}
   b.disabled=true;s.textContent='Typing '+v.length+' chars... watch the screen.';
   try{const r=await fetch('/type',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({text:v,delay:d.value})});s.textContent=await r.text();}
   catch(e){s.textContent='Error: '+e;}
   b.disabled=false;
 }
</script></body></html>
)HTML";

const char CONFIG_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>WiFi Setup</title>
<style>
 :root{color-scheme:dark}
 body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:420px;margin:auto}
 h1{font-size:18px}
 input,select{width:100%;box-sizing:border-box;background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:8px;padding:11px;font-size:15px;margin:6px 0}
 button{width:100%;background:#3b82f6;color:#fff;border:0;border-radius:8px;padding:12px;font-size:15px;cursor:pointer;margin-top:8px}
 button.sec{background:#374151;padding:8px;font-size:13px;margin-top:0}
 label{font-size:13px;color:#9ca3af}
 .muted{font-size:13px;color:#9ca3af}
</style></head><body>
<h1>WiFi Setup</h1>
<p class="muted">Pick your home WiFi. Saved on the dongle and reused on every boot.</p>
<label>Network</label>
<select id="sel"><option>Scanning...</option></select>
<button type="button" id="rescan" class="sec">Rescan</button>
<input id="other" placeholder="Type network name" autocapitalize="off" autocorrect="off" spellcheck="false" style="display:none">
<label>Password</label>
<input id="pass" type="password">
<button id="save">Save &amp; Connect</button>
<div id="msg" class="muted" style="margin-top:10px"></div>
<script>
 const sel=document.getElementById('sel'),other=document.getElementById('other'),
       pass=document.getElementById('pass'),msg=document.getElementById('msg'),
       rescan=document.getElementById('rescan'),save=document.getElementById('save');
 function toggle(){other.style.display=sel.value=='__other__'?'block':'none';}
 async function scan(){
   sel.innerHTML='<option>Scanning...</option>';
   try{
     const a=await (await fetch('/scan')).json();
     a.sort((x,y)=>y.r-x.r);
     const seen={};let h='';
     for(const n of a){if(!n.s||seen[n.s])continue;seen[n.s]=1;
       h+='<option value="'+n.s.replace(/"/g,'&quot;')+'">'+n.s+'  ('+n.r+'dBm)</option>';}
     h+='<option value="__other__">Other… (type manually)</option>';
     sel.innerHTML=h;
   }catch(e){sel.innerHTML='<option value="__other__">Other… (type manually)</option>';}
   toggle();
 }
 sel.onchange=toggle; rescan.onclick=scan;
 save.onclick=async()=>{
   const ssid=sel.value=='__other__'?other.value.trim():sel.value;
   if(!ssid){msg.textContent='Pick or type a network.';return;}
   msg.textContent='Saving...';
   try{await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
     body:new URLSearchParams({ssid,pass:pass.value})});}catch(e){}
   msg.textContent='Saved. The dongle is rebooting and joining "'+ssid+'". Reconnect to your normal WiFi, then open http://esptype.local';
 };
 scan();
</script>
<p class="muted" style="margin-top:16px">Made by <b style="color:#ccc">poordevman</b></p>
</body></html>
)HTML";

void handleRoot() {
  if (apMode) server.send_P(200, "text/html", CONFIG_HTML);
  else        server.send_P(200, "text/html", TYPER_HTML);
}

void handleWifiPage() { server.send_P(200, "text/html", CONFIG_HTML); }

static String jsonEsc(const String& in) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 32)  o += c;
  }
  return o;
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    j += "{\"s\":\"" + jsonEsc(WiFi.SSID(i)) + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

void handleType() {
  if (!server.hasArg("text")) { server.send(400, "text/plain", "no text"); return; }
  String text = server.arg("text");
  int d = server.hasArg("delay") ? server.arg("delay").toInt() : defaultDelayMs;
  typeText(text, d);
  server.send(200, "text/plain", "Typed " + String(text.length()) + " characters.");
}

void handleSave() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  if (s.length() == 0) { server.send(400, "text/plain", "SSID required"); return; }
  saveCreds(s, p);
  server.send(200, "text/html",
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
    "Saved. Rebooting and connecting to <b>" + s + "</b>...<br><br>"
    "Reconnect to your normal WiFi, then open http://esptype.local</body>");
  drawStatus("Saved, reboot", 100, 1);
  delay(1200);
  ESP.restart();
}

void handleForget() {
  clearCreds();
  server.send(200, "text/html",
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
    "WiFi forgotten. Rebooting into setup hotspot <b>ESP32_Typer</b>...</body>");
  delay(1200);
  ESP.restart();
}

void handleNotFound() {
  if (apMode) {  // captive-portal: push everything to the setup page
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "404");
  }
}

// ---------- WiFi bring-up ----------
bool tryJoin(const String& s, const String& p) {
  drawHeader();
  drawStatus("Joining...", 0, 2);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(s.c_str(), p.c_str());
  for (int i = 0; i < 40; i++) {           // ~12s
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      netName = s;
      ipLine = WiFi.localIP().toString();
      return true;
    }
    delay(300);
  }
  return false;
}

void startSetupAP() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);   // AP_STA so we can also scan for networks
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  ipLine = WiFi.softAPIP().toString();
  dns.start(53, "*", AP_IP);               // captive portal
}

void setup() {
#if CONFIG_IDF_TARGET_ESP32S3
  // Release GPIO19/20 from the USB-Serial-JTAG PHY so USB-OTG (TinyUSB HID)
  // can drive them; without this the PC sees no keyboard.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
#endif

  Serial.begin(115200);
  lcdInit();
  drawHeader();

  Keyboard.begin();
  USB.productName("HID Keyboard");
  USB.begin();

  String s, p;
  loadCreds(s, p);

  bool joined = false;
  if (s.length() > 0) joined = tryJoin(s, p);
  if (!joined) startSetupAP();

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  drawHeader();
  if (!apMode) drawStatus("Ready", 0);   // setup screen keeps its 4 info lines

  server.on("/", handleRoot);
  server.on("/wifi", handleWifiPage);
  server.on("/scan", handleScan);
  server.on("/type", HTTP_POST, handleType);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/forget", handleForget);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  if (apMode) dns.processNextRequest();
  server.handleClient();
}
