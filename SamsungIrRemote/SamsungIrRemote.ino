/*
  ESP32 Samsung IR Magic Remote
  -----------------------------
  ESP32 joins your home Wi-Fi; open http://magicremote.local (or its IP) on your phone.
  Every button is sent to the TV as a Samsung infrared signal from an IR LED.

  Wiring:  G4 --[100 ohm]-- IR LED long leg (+)
           GND ------------- IR LED short leg (-)
  Point the LED at the TV's front panel (bottom centre), within ~1-2 m.

  First boot / new Wi-Fi: ESP32 makes Wi-Fi "MagicRemote-Setup" -> open 192.168.4.1.
  BOOT button: short press = TV power, hold 5 s = forget Wi-Fi.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

const uint16_t IR_LED_PIN = 4;
const uint8_t  STATUS_LED = 2;
const uint8_t  BOOT_BTN   = 0;
const char*    SETUP_SSID = "MagicRemote-Setup";
const char*    HOSTNAME   = "magicremote";

WebServer server(80);
DNSServer dns;
Preferences prefs;
IRsend irsend(IR_LED_PIN);
bool setupMode = false;

struct KeyDef { const char* id; uint32_t code; };
const KeyDef KEYS[] = {
  {"power", 0xE0E040BF}, {"volup", 0xE0E0E01F}, {"voldn", 0xE0E0D02F}, {"mute", 0xE0E0F00F},
  {"chup", 0xE0E048B7}, {"chdn", 0xE0E008F7}, {"up", 0xE0E006F9}, {"down", 0xE0E08679},
  {"left", 0xE0E0A659}, {"right", 0xE0E046B9}, {"ok", 0xE0E016E9}, {"menu", 0xE0E058A7},
  {"home", 0xE0E09E61}, {"back", 0xE0E01AE5}, {"exit", 0xE0E0B44B}, {"input", 0xE0E0807F},
  {"play", 0xE0E0E21D}, {"pause", 0xE0E052AD},
  {"1", 0xE0E020DF}, {"2", 0xE0E0A05F}, {"3", 0xE0E0609F}, {"4", 0xE0E010EF}, {"5", 0xE0E0906F},
  {"6", 0xE0E050AF}, {"7", 0xE0E030CF}, {"8", 0xE0E0B04F}, {"9", 0xE0E0708F}, {"0", 0xE0E08877},
};
const int KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

void blink() { digitalWrite(STATUS_LED, HIGH); delay(30); digitalWrite(STATUS_LED, LOW); }

String press(const String& id) {
  for (int i = 0; i < KEY_COUNT; i++) {
    if (id != KEYS[i].id) continue;
    irsend.sendSAMSUNG(KEYS[i].code, 32);
    blink();
    Serial.printf("[TX] %s 0x%08X\n", KEYS[i].id, KEYS[i].code);
    return String(KEYS[i].id) + " sent";
  }
  return "unknown key";
}

const char REMOTE_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Samsung Magic Remote</title><style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font-family:system-ui,sans-serif;background:#0f1115;color:#e8e8ea;display:flex;justify-content:center}
.r{width:100%;max-width:360px;padding:16px}
h1{font-size:18px;text-align:center;margin:4px 0 14px}
.g{display:grid;gap:10px;margin-bottom:14px}.g3{grid-template-columns:repeat(3,1fr)}
button{border:0;border-radius:14px;padding:16px 0;font-size:15px;background:#232733;color:#fff;cursor:pointer;user-select:none}
button:active{background:#3b4252;transform:scale(.96)}
.pw{background:#c0392b}.ok{background:#1f5fd1;border-radius:50%;aspect-ratio:1}
.n{padding:12px 0;background:#1b1e25}.sm{background:#2a2f3b;font-size:13px;padding:12px 0}
.rk{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
.col{display:grid;gap:10px}.lab{text-align:center;font-size:12px;color:#888}
#t{text-align:center;font-size:12px;color:#8a8;min-height:16px}
</style></head><body><div class="r">
<h1>Samsung Magic Remote</h1>
<div class="g g3"><button class="pw" data-k="power">Power</button><button data-k="input">Source</button><button data-k="mute">Mute</button></div>
<div class="g g3"><button data-k="menu">Menu</button><button data-k="up">&#9650;</button><button data-k="home">Home</button>
<button data-k="left">&#9664;</button><button class="ok" data-k="ok">OK</button><button data-k="right">&#9654;</button>
<button data-k="back">Back</button><button data-k="down">&#9660;</button><button data-k="exit">Exit</button></div>
<div class="rk"><div class="col"><button data-k="volup">VOL +</button><div class="lab">Volume</div><button data-k="voldn">VOL &minus;</button></div>
<div class="col"><button data-k="chup">CH +</button><div class="lab">Channel</div><button data-k="chdn">CH &minus;</button></div></div>
<div class="g g3"><button data-k="play">Play</button><button data-k="pause">Pause</button><span></span></div>
<div class="g g3"><button class="n" data-k="1">1</button><button class="n" data-k="2">2</button><button class="n" data-k="3">3</button>
<button class="n" data-k="4">4</button><button class="n" data-k="5">5</button><button class="n" data-k="6">6</button>
<button class="n" data-k="7">7</button><button class="n" data-k="8">8</button><button class="n" data-k="9">9</button>
<span></span><button class="n" data-k="0">0</button><span></span></div>
<div class="g g3"><span></span><button class="sm" onclick="if(confirm('Forget home Wi-Fi?'))go('/reset')">Reset Wi-Fi</button><span></span></div>
<div id="t"></div></div><script>
const t=document.getElementById('t');
function go(u){fetch(u).then(r=>r.text()).then(x=>{t.textContent=x;setTimeout(()=>t.textContent='',1000)})}
document.querySelectorAll('[data-k]').forEach(b=>{let iv;const k=b.dataset.k;const s=()=>go('/key?k='+k);
 b.addEventListener('pointerdown',e=>{e.preventDefault();s();if(/vol|ch|^up$|down|left|right/.test(k))iv=setInterval(s,250)});
 ['pointerup','pointerleave','pointercancel'].forEach(ev=>b.addEventListener(ev,()=>clearInterval(iv)))});
</script></body></html>)HTML";

String setupPage() {
  String opts;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    if (ss.isEmpty() || opts.indexOf(">" + ss + "<") >= 0) continue;
    ss.replace("\"", "&quot;");
    opts += "<option value=\"" + ss + "\">" + ss + "</option>";
  }
  return String(F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MagicRemote Setup</title><style>body{font-family:system-ui;background:#0f1115;color:#eee;display:flex;justify-content:center}"
    "form{width:100%;max-width:340px;padding:20px}select,input,button{width:100%;padding:12px;margin:8px 0 14px;border-radius:10px;"
    "border:1px solid #333;background:#1b1e25;color:#fff;font-size:16px}button{background:#1f5fd1;border:0}</style></head><body>"
    "<form action='/save' method='post'><h2>Connect to your Wi-Fi</h2><label>Wi-Fi network</label><select name='ssid'>")) + opts +
    F("</select><label>Or type network name</label><input name='ssid2' placeholder='(optional)'>"
      "<label>Password</label><input name='pass' type='password'><button>Save &amp; connect</button></form></body></html>");
}

void startSetupMode() {
  setupMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_SSID);
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", [] { server.send(200, "text/html", setupPage()); });
  server.on("/save", HTTP_POST, [] {
    String ssid = server.arg("ssid2").length() ? server.arg("ssid2") : server.arg("ssid");
    prefs.putString("ssid", ssid);
    prefs.putString("pass", server.arg("pass"));
    server.send(200, "text/html",
      "<meta name='viewport' content='width=device-width,initial-scale=1'><body style='font-family:system-ui;padding:20px'>"
      "<h3>Saved! Connecting to " + ssid + "...</h3><p>Switch your phone back to your home Wi-Fi and open "
      "<b>http://magicremote.local</b></p></body>");
    delay(1500);
    ESP.restart();
  });
  // The remote also works in setup mode, straight from the hotspot.
  server.on("/remote", [] { server.send_P(200, "text/html", REMOTE_PAGE); });
  server.on("/key", [] { server.send(200, "text/plain", press(server.arg("k"))); });
  server.onNotFound([] { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); });
  server.begin();
  Serial.println("=== SETUP MODE ===");
  Serial.printf("Join Wi-Fi \"%s\" and open http://192.168.4.1\n", SETUP_SSID);
}

void startRemoteMode() {
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  server.on("/", [] { server.send_P(200, "text/html", REMOTE_PAGE); });
  server.on("/key", [] { server.send(200, "text/plain", press(server.arg("k"))); });
  server.on("/reset", [] {
    server.send(200, "text/plain", "Wi-Fi forgotten, restarting into setup mode");
    prefs.remove("ssid"); prefs.remove("pass");
    delay(800); ESP.restart();
  });
  server.begin();
  Serial.println("=== MAGIC REMOTE READY (IR) ===");
  Serial.print("Open: http://"); Serial.print(WiFi.localIP());
  Serial.println("  or  http://magicremote.local");
}

void checkButton() {
  static uint32_t downAt = 0;
  static bool wasDown = false;
  bool down = digitalRead(BOOT_BTN) == LOW;
  if (down && !wasDown) downAt = millis();
  if (down && millis() - downAt > 5000) {
    Serial.println("Forgetting Wi-Fi...");
    prefs.remove("ssid"); prefs.remove("pass");
    for (int i = 0; i < 6; i++) blink(), delay(100);
    ESP.restart();
  }
  if (!down && wasDown && millis() - downAt > 40) press("power");
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  irsend.begin();
  prefs.begin("remote", false);
  WiFi.setHostname(HOSTNAME);

  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid.isEmpty()) { startSetupMode(); return; }

  Serial.println("Connecting to Wi-Fi: " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connect failed (wrong password?) -> setup mode");
    startSetupMode();
    return;
  }
  WiFi.setSleep(false);
  startRemoteMode();
}

void loop() {
  server.handleClient();
  checkButton();
  if (setupMode) dns.processNextRequest();
}
