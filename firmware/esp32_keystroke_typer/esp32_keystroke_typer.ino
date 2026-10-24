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
// Per line: Home,Home -> type content -> Shift+End,Delete (removes the target
// editor's auto-indent) -> Enter -> wait for auto-indent. Prevents cascading indents.
void tapKey(uint8_t k) { Keyboard.press(k); Keyboard.release(k); }
void goHome() { tapKey(KEY_HOME); delay(60); tapKey(KEY_HOME); delay(60); }
void clearRight() {
  Keyboard.press(KEY_LEFT_SHIFT);
  tapKey(KEY_END);
  Keyboard.release(KEY_LEFT_SHIFT);
  delay(60);
  tapKey(KEY_DELETE);
  Keyboard.releaseAll();
  delay(60);
}

void typeText(const char* text, size_t n, int d) {
  if (d < 0) d = 0;
  const char* p = text;
  const char* end = text + n;
  int totalLines = 1;
  for (const char* q = p; q < end; q++) if (*q == '\n') totalLines++;
  int curLine = 1;
  drawStatus("Line 1/" + String(totalLines), 0);

  goHome();
  clearRight();
  delay(20);

  bool first = true;
  while (p < end) {
    if (!first) goHome();
    first = false;
    while (p < end && *p != '\n') {
      if (*p != '\r') { Keyboard.write(*p); delay(d); }
      p++;
    }
    if (p < end && *p == '\n') {
      clearRight();
      Keyboard.write('\n');
      p++;
      delay(250);                       // let the editor auto-indent
      curLine++;
      int pct = (int)((p - text) * 100 / (n == 0 ? 1 : n));
      drawStatus("Line " + String(curLine) + "/" + String(totalLines), pct);
    }
  }
  Keyboard.releaseAll();
  drawStatus("Done: " + String(n) + " chars", 100, 1);
}

const char BOOT_TEXT[] PROGMEM = R"BOOT(' Switch 2 / SmileBASIC 4.4.9 bootstrap. See README before RUN.
' Addresses are rediscovered; headers, native code and the SDK CRC are checked.
' REMOVE is issued once from Direct mode after RUN; its handled error is expected.
' START verifies the bootstrap, connects to the computer and runs the demo.

VAR NSR,B#,C#,T#,SH#,S#,M#,D#,G#,MM#,H$,QF$,GH$,DP$,CP$
VAR PHASE=0,FD=-1,POOL#=0,ZERO$=CHR$(0),I
FOR I=1 TO 18:ZERO$=ZERO$+ZERO$:NEXT
NSR=ARRAY$(4)
COPY NSR[0],0,"0000":COPY NSR[1],0,"1111"
COPY NSR[2],0,"2222":COPY NSR[3],0,"3333"
PRINT "DIST BOOT READY",1015
END
COMMON DEF P$(V#)
 VAR S$="",J
 FOR J=0 TO 3
  S$=S$+CHR$(V#-FLOOR(V#/65536)*65536):V#=FLOOR(V#/65536)
 NEXT
 RETURN S$
END
COMMON DEF U#(S$)
 VAR V#=0,J
 FOR J=3 TO 0 STEP -1:V#=V#*65536+ASC(MID$(S$,J,1)):NEXT
 RETURN V#
END
COMMON DEF HASH#(S$)
 VAR V#=0,J
 FOR J=0 TO LEN(S$)-1
  V#=V#*65599+ASC(MID$(S$,J,1))
  V#=V#-FLOOR(V#/2147483647)*2147483647
 NEXT
 RETURN V#
END
COMMON DEF PTR(V#,A)
 RETURN V#>0 && V#<549755813888 && V#==FLOOR(V#/A)*A
END
COMMON DEF FAIL(N)
 PHASE=-1:PRINT "BOOT FAILED",N:RETURN 0
END
COMMON DEF DUMP S$
 VAR J
 FOR J=0 TO LEN(S$)-1
  PRINT HEX$(ASC(MID$(S$,J,1)),4);
  IF (J AND 3)==3 THEN PRINT
 NEXT
END
COMMON DEF W$(A#,N)
 VAR S$
 COPY NSR[2],0,P$(A#)+P$(N/2)
 S$=COPY(NSR[0],0,N/2):COPY NSR[2],0,H$
 RETURN S$
END
COMMON DEF PUT O,V#
 COPY NSR[1],O/2,P$(V#)
END
COMMON DEF CALL#(F,A#,B#,C#,E#)
 RETURN RAW#(P$(D#+F)+P$(A#)+P$(B#)+P$(C#)+P$(E#))
END
COMMON DEF RAW#(V$)
 COPY NSR[1],126976,QF$
 COPY NSR[1],1036,V$,0,4
 COPY NSR[1],127004,V$,4,4
 COPY NSR[1],127120,V$,8,4
 COPY NSR[1],127128,V$,12,8
 COPY NSR[2],0,GH$:COPY NSR[0],0,DP$
 COPY NSR[1],128,CP$:COPY NSR[2],0,H$
 RETURN U#(COPY(NSR[1],8216,4))
END
COMMON DEF BOOT()
 VAR V$,Q$,J,V#,L#,K#=0,FOUND=0,KH#,NATIVE#,CRC#,R#,ATTEMPT
 IF PHASE!=0 THEN RETURN FAIL(1)
 PHASE=1
 V$=COPY(NSR[0],0,16)
 FOR J=0 TO 3
  V#=U#(MID$(V$,J*4,4))
  IF !PTR(V#,16) THEN RETURN FAIL(2)
  IF J>0 && (V#<=U#(MID$(V$,(J-1)*4,4)) || V#-U#(LEFT$(V$,4))>=4096) THEN RETURN FAIL(3)
 NEXT
 B#=U#(LEFT$(V$,4)):C#=U#(MID$(V$,8,4))+16:T#=U#(MID$(V$,12,4))+16
 H$=P$(T#)+P$(4):COPY NSR[2],0,H$:COPY NSR[0],0,P$(C#)
 IF LEN(NSR[0])!=4 || NSR[0]!=NSR[3] || NSR[3]!="2222" THEN RETURN FAIL(4)
 COPY NSR[0],1,"9"
 IF NSR[3]!="2922" THEN RETURN FAIL(5)
 COPY NSR[0],1,"2"
 IF NSR[3]!="2222" THEN RETURN FAIL(6)
 Q$=COPY(ZERO$,0,2048)
 FOR ATTEMPT=0 TO 2
  NSR[1]=KEY(2):COPY NSR[2],0,P$(B#+8)+P$(4):KH#=U#(NSR[0]):COPY NSR[2],0,H$
  IF !PTR(KH#,16) THEN RETURN FAIL(10)
  COPY NSR[2],0,P$(FLOOR(KH#/4096)*4096)+P$(2048)
  COPY Q$,MIN(0,LEN(KEY(1)),LEN(KEY(2))),NSR[0]:COPY NSR[2],0,H$
  FOR J=0 TO 2040 STEP 8
   V#=U#(MID$(Q$,J,4)):L#=U#(MID$(Q$,J+4,4))
   IF L#==5 && PTR(V#-&H9DB49C,4096) THEN
    IF K#!=V# THEN K#=V#:FOUND=FOUND+1
   ENDIF
  NEXT
  IF FOUND!=0 THEN BREAK
 NEXT
 IF FOUND!=1 THEN RETURN FAIL(11)
 M#=K#-&H9DB49C
 IF W$(K#,10)!=KEY(2) THEN RETURN FAIL(12)
 IF U#(W$(FLOOR(K#/4096)*4096+&H170,8))!=M#+&H3854D0 THEN RETURN FAIL(13)
 IF HASH#(W$(M#,128))!=2137937599 THEN RETURN FAIL(14)
 Q$=W$(M#+&H9C9298,128):MM#=U#(MID$(Q$,44,4))
 D#=MM#-&H5872F0:G#=M#+&H9C92F0
 IF !PTR(D#,4096) || U#(MID$(Q$,40,4))!=D#+&H586FB0 THEN RETURN FAIL(15)
 IF HASH#(W$(D#,128))!=1437608693 THEN RETURN FAIL(16)
 IF HASH#(W$(D#+1242644,120))!=943625752 THEN RETURN FAIL(20)
 IF HASH#(W$(D#+1178912,16))!=1012566187 THEN RETURN FAIL(21)
 IF HASH#(W$(D#+310672,24))!=236409839 THEN RETURN FAIL(22)
 IF HASH#(W$(D#+3378856,56))!=260441352 THEN RETURN FAIL(23)
 IF HASH#(W$(D#+1621008,16))!=2075897594 THEN RETURN FAIL(24)
 IF HASH#(W$(D#+202056,52))!=1662332171 THEN RETURN FAIL(25)
 IF HASH#(W$(D#+5796592,380))!=1057474404 THEN RETURN FAIL(26)
 IF HASH#(W$(D#+1409812,24))!=1911277185 THEN RETURN FAIL(27)
 IF HASH#(W$(D#+463632,512))!=301599630 THEN RETURN FAIL(28)
 NSR[1]=CHR$(0):FOR J=1 TO 18:NSR[1]=NSR[1]+NSR[1]:NEXT
 SH#=U#(W$(B#+8,8))
 IF !PTR(SH#,16) THEN RETURN FAIL(7)
 Q$=W$(SH#,16):S#=U#(LEFT$(Q$,4))
 IF !PTR(S#,16) || U#(MID$(Q$,4,4))!=262144 THEN RETURN FAIL(8)
 H$=P$(S#)+P$(4):COPY NSR[2],0,H$
 IF NSR[0]!=LEFT$(ZERO$,4) THEN RETURN FAIL(9)
 PUT 552,S#+&H900
 PUT 2072,D#+&H5872F0
 PUT 2176,S#+&H7B0
 PUT 2336,D#+&H12F614
 PUT 16432,1
 PUT 253960,MM#
 PUT 253968,G#
 PUT 253976,S#+&H880
 PUT 254008,S#+&H4040
 PUT 254048,D#+&H11FD20
 PUT 254200,D#+&H4BD90
 PUT 254232,D#+&H338EA8
 PUT 254240,S#+&H4080
 PUT 254256,32
 PUT 254264,0
 PUT 254440,D#+&H18BC10
 PUT 254496,S#+&H100
 PUT 254504,S#+&H4030
 PUT 254520,D#+&H31548
 PUT 254528,S#+&H200
 IF W$(S#+552,8)!=P$(S#+&H900) THEN RETURN FAIL(39)
 IF W$(S#+2072,8)!=P$(D#+&H5872F0) THEN RETURN FAIL(39)
 IF W$(S#+2176,8)!=P$(S#+&H7B0) THEN RETURN FAIL(39)
 IF W$(S#+2336,8)!=P$(D#+&H12F614) THEN RETURN FAIL(39)
 IF W$(S#+16432,8)!=P$(1) THEN RETURN FAIL(39)
 IF W$(S#+253960,8)!=P$(MM#) THEN RETURN FAIL(39)
 IF W$(S#+253968,8)!=P$(G#) THEN RETURN FAIL(39)
 IF W$(S#+253976,8)!=P$(S#+&H880) THEN RETURN FAIL(39)
 IF W$(S#+254008,8)!=P$(S#+&H4040) THEN RETURN FAIL(39)
 IF W$(S#+254048,8)!=P$(D#+&H11FD20) THEN RETURN FAIL(39)
 IF W$(S#+254200,8)!=P$(D#+&H4BD90) THEN RETURN FAIL(39)
 IF W$(S#+254232,8)!=P$(D#+&H338EA8) THEN RETURN FAIL(39)
 IF W$(S#+254240,8)!=P$(S#+&H4080) THEN RETURN FAIL(39)
 IF W$(S#+254256,8)!=P$(32) THEN RETURN FAIL(39)
 IF W$(S#+254264,8)!=P$(0) THEN RETURN FAIL(39)
 IF W$(S#+254440,8)!=P$(D#+&H18BC10) THEN RETURN FAIL(39)
 IF W$(S#+254496,8)!=P$(S#+&H100) THEN RETURN FAIL(39)
 IF W$(S#+254504,8)!=P$(S#+&H4030) THEN RETURN FAIL(39)
 IF W$(S#+254520,8)!=P$(D#+&H31548) THEN RETURN FAIL(39)
 IF W$(S#+254528,8)!=P$(S#+&H200) THEN RETURN FAIL(39)
 QF$=COPY(NSR[1],126976,296)
 IF W$(S#+&H3E000,592)!=QF$ THEN RETURN FAIL(40)
 GH$=P$(G#)+P$(4):DP$=P$(D#+&H12F614):CP$=P$(S#+&H3E000)
 COPY NSR[1],&H2040,"NS2DISTCHECK1234"
 R#=CALL#(&H5872F0,S#+&H4040,S#+&H4080,32,0)
 IF R#!=S#+&H4040 || COPY(NSR[1],&H2020,16)!="NS2DISTCHECK1234" THEN RETURN FAIL(41)
 IF U#(W$(G#,8))!=MM# || NSR[0]!=LEFT$(ZERO$,4) THEN RETURN FAIL(42)
 R#=CALL#(&H158314,S#+&H4100,S#+&H4140,D#,0)
 Q$=COPY(NSR[1],&H2080,32)
 IF R#!=0 || U#(LEFT$(Q$,4))!=D# || U#(MID$(Q$,4,4))!=6164480 THEN RETURN FAIL(43)
 IF U#(MID$(Q$,8,4))!=3 || U#(MID$(Q$,12,4))!=5 THEN RETURN FAIL(44)
 CRC#=CALL#(463632,0,D#,6164480,0)
 IF CRC#!=3925528160 THEN RETURN FAIL(45)
 PHASE=2:CLS:PRINT "BINDINGS"
 DUMP P$(B#)+P$(C#)+P$(T#)+P$(SH#)+P$(S#)+P$(M#)+P$(D#)+P$(G#)+P$(MM#)
 PRINT "BOOT OK",CRC#:RETURN 1
END
COMMON DEF START(PORT,A,B,C,D,T0#,T1#,T2#,T3#)
 VAR R#,J,A$,HELLO$
 IF !BOOT() THEN RETURN 0
 IF PHASE!=2 OR PORT<1024 OR PORT>65535 THEN RETURN FAIL(50)
 IF MIN(A,B,C,D)<0 OR MAX(A,B,C,D)>255 THEN RETURN FAIL(50)
 PHASE=3
 IF CALL#(&H144DE0,0,0,0,0)==0 THEN
  IF CALL#(&H577710,S#+&H5100,4096,&H400000,0)!=0 THEN RETURN FAIL(52)
  POOL#=U#(COPY(NSR[1],&H2880,4))
  IF !PTR(POOL#,4096) THEN RETURN FAIL(53)
  IF CALL#(&H1453E0,POOL#,&H400000,&H80000,4)!=0 THEN RETURN FAIL(54)
 ENDIF
 IF CALL#(&H3904A0,0,0,0,0)!=0 THEN RETURN FAIL(55)
 R#=CALL#(&H391650,1,0,0,0):R#=CALL#(&H391480,0,0,0,0)
 FOR J=0 TO 3599
  R#=CALL#(&H3915B0,0,0,0,0):IF R#==1 THEN BREAK
  VSYNC
 NEXT
 IF R#!=1 THEN RETURN FAIL(56)
 FD=CALL#(&H14A210,2,1,0,0)
 IF FD<0 OR FD>4095 THEN RETURN FAIL(57)
 A$=CHR$(&H210)+CHR$(FLOOR(PORT/256)+(PORT MOD 256)*256)+CHR$(A+B*256)+CHR$(C+D*256)+P$(0)
 COPY NSR[1],&H2900,A$
 IF CALL#(&H147AF0,FD,S#+&H5200,16,0)!=0 THEN RETURN FAIL(58)
 HELLO$=P$(&H31545344)+P$(1)+P$(2)+P$(S#)+P$(&H80000)+P$(M#)+P$(D#)+P$(G#)+P$(MM#)+P$(B#)+P$(C#)
 HELLO$=HELLO$+P$(T0#)+P$(T1#)+P$(T2#)+P$(T3#)+P$(3925528160)
 COPY NSR[1],&H2A00,HELLO$
 IF CALL#(&H140BD0,FD,S#+&H5400,128,0)!=128 THEN RETURN FAIL(59)
 PHASE=4:PRINT "HOST CONNECTED":RETURN RPC()
END
COMMON DEF RPC()
 VAR R#,V$,FIRST$,LAST$
 IF PHASE!=4 THEN RETURN FAIL(60)
 PHASE=5:PRINT "RPC RUNNING"
 WHILE 1
  R#=CALL#(&H140B90,FD,S#+&H40000,&H8080,&H40)
  IF R#!=&H8080 THEN BREAK
  V$=COPY(NSR[1],&H20000,40)
  IF LEFT$(V$,20)==CHR$(0)*20 THEN BREAK
  R#=RAW#(LEFT$(V$,20))
  FIRST$=COPY(NSR[1],8216,4)
  R#=RAW#(MID$(V$,20,20))
  COPY NSR[1],8220,FIRST$
  R#=CALL#(&H140BD0,FD,S#+&H4030,&H8010,0)
  IF R#!=&H8010 THEN BREAK
 WEND
 PHASE=6:PRINT "RPC STOP",R#:RETURN 1
END
)BOOT";

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
 .cb{flex-wrap:wrap;gap:8px}
 .cb button{flex:0 0 auto;padding:8px 14px;font-size:13px}
 .cb button.x{padding:8px 10px;background:#4b1d1d}
 #cf{display:none;flex-wrap:wrap;gap:8px}
 #cf input[type=text]{background:#0b0c0f;color:#eee;border:1px solid #333;border-radius:8px;padding:8px;min-width:0}
 #cn{width:90px}
 #ct{flex:1;min-width:140px}
 #cf button{padding:8px 14px;font-size:13px}
 .credit{color:#5b616e;font-size:12px;margin-top:14px}
 .credit b{color:#9ca3af}
</style></head><body>
<h1>ESP32 Keystroke Typer</h1>
<div class="row cb" id="cb"></div>
<div class="row cb" id="cb2"></div>
<div class="row" id="cf"><input type="text" id="cn" placeholder="Name"><input type="text" id="ct" placeholder="Text to type"><label><input type="checkbox" id="ce" checked> Enter</label><button class="sec" onclick="ad()">Save</button></div>
<div class="ed"><div class="g" id="g">1</div><textarea id="t" spellcheck="false" wrap="off" placeholder="Paste text or code here..."></textarea></div>
<div class="row"><label>Speed</label><input type="range" id="d" min="0" max="40" value="8"><span id="dv">8ms/char</span></div>
<div class="row"><button class="sec" onclick="clr()">Clear</button><button id="b" onclick="send()">Send to Device</button><button id="b2" onclick="send(1)">Send + Enter</button></div>
<div id="status">Focus the target window before sending.</div>
<p class="credit">Made by <b>poordevman</b> &middot; <a href="/wifi">WiFi settings</a></p>
<script>
 const t=document.getElementById('t'),d=document.getElementById('d'),dv=document.getElementById('dv'),g=document.getElementById('g');
 d.oninput=()=>dv.textContent=d.value+'ms/char';
 function gut(){let L=t.value.split('\n').length,s='';for(let i=1;i<=L;i++)s+=i+'\n';g.textContent=s;g.scrollTop=t.scrollTop;}
 t.addEventListener('input',gut);t.addEventListener('scroll',()=>g.scrollTop=t.scrollTop);
 t.addEventListener('keydown',e=>{if(e.key==='Tab'){e.preventDefault();const s=t.selectionStart,x=t.selectionEnd;t.value=t.value.slice(0,s)+'\t'+t.value.slice(x);t.selectionStart=t.selectionEnd=s+1;gut();}});
 gut();
 function clr(){t.value='';gut();document.getElementById('status').textContent='Cleared.';t.focus();}
 async function tx(v,u){
   const s=document.getElementById('status'),bs=document.querySelectorAll('button');
   bs.forEach(x=>x.disabled=true);s.textContent=u?'Typing bootstrap... watch the screen.':'Typing '+v.length+' chars... watch the screen.';
   try{const r=await fetch(u||'/type',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({text:v,delay:d.value})});s.textContent=await r.text();}
   catch(e){s.textContent='Error: '+e;}
   bs.forEach(x=>x.disabled=false);
 }
 function send(en){if(!t.value){document.getElementById('status').textContent='Nothing to send.';return;}tx(t.value+(en?'\n':''));}
 const CB=[['LOAD','LOAD "TXT:NS2"'],['RUN','RUN'],['REMOVE','REMOVE NSR,&H60000000,&H3FFFFFFF']];
 let cu=[];
 function sv(){fetch('/custom',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({list:JSON.stringify(cu)})}).then(r=>{if(!r.ok)document.getElementById('status').textContent='Could not save (too many buttons?)';}).catch(()=>{});}
 function mk(c,n,v,e){const b=document.createElement('button');b.className='sec';b.textContent=n;b.onclick=()=>tx(v+(e?'\n':''));c.appendChild(b);}
 function rb(){const c=document.getElementById('cb'),c2=document.getElementById('cb2');c.innerHTML=c2.innerHTML='';
   CB.forEach(x=>mk(c,x[0],x[1],1));
   const bb=document.createElement('button');bb.className='sec';bb.textContent='BOOTSTRAP';bb.onclick=()=>tx('','/boot');c.appendChild(bb);
   cu.forEach((x,i)=>{mk(c2,x.n,x.t,x.e);const r=document.createElement('button');r.className='sec x';r.textContent='x';r.title='Delete';r.onclick=()=>{cu.splice(i,1);sv();rb();};c2.appendChild(r);});
   if(cu.length<4){const a=document.createElement('button');a.className='sec';a.textContent='+ Custom';a.onclick=()=>{const f=document.getElementById('cf');f.style.display=f.style.display==='flex'?'none':'flex';};c2.appendChild(a);}}
 function ad(){const n=document.getElementById('cn'),x=document.getElementById('ct'),e=document.getElementById('ce');
   if(!n.value||!x.value||cu.length>=4)return;cu.push({n:n.value,t:x.value,e:e.checked});sv();n.value=x.value='';document.getElementById('cf').style.display='none';rb();}
 rb();
 fetch('/custom').then(r=>r.json()).then(j=>{if(Array.isArray(j)){cu=j.slice(0,4);rb();}}).catch(()=>{});
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
  typeText(text.c_str(), text.length(), d);
  server.send(200, "text/plain", "Typed " + String(text.length()) + " characters.");
}

void handleBoot() {
  int d = server.hasArg("delay") ? server.arg("delay").toInt() : defaultDelayMs;
  size_t n = strlen(BOOT_TEXT);
  typeText(BOOT_TEXT, n, d);
  server.send(200, "text/plain", "Typed bootstrap (" + String(n) + " characters).");
}

void handleCustomGet() {
  prefs.begin("buttons", true);
  String j = prefs.getString("list", "[]");
  prefs.end();
  server.send(200, "application/json", j);
}

void handleCustomSave() {
  String j = server.arg("list");
  if (j.length() > 3500) { server.send(400, "text/plain", "Too large"); return; }
  prefs.begin("buttons", false);
  prefs.putString("list", j);
  prefs.end();
  server.send(200, "text/plain", "ok");
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
  server.on("/boot", HTTP_POST, handleBoot);
  server.on("/custom", HTTP_GET, handleCustomGet);
  server.on("/custom", HTTP_POST, handleCustomSave);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/forget", handleForget);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  if (apMode) dns.processNextRequest();
  server.handleClient();
}
