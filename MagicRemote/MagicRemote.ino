/*
  ESP32 Magic Universal TV Remote
  --------------------------------
  - Creates Wi-Fi hotspot "MagicRemote" (password 12345678)
  - Open http://192.168.4.1 on your phone -> full TV remote
  - Brands: Samsung, LG, Sony, Panasonic, Philips + "Auto" (tries all)
  - BOOT button (GPIO0): short press = power toggle on selected brand,
                         long press  = universal power (all brands)
  - Optional IR receiver on GPIO15: learn any remote button and replay it

  Wiring:
    IR LED (940nm): GPIO4 -> 100R -> IR LED anode, cathode -> GND
      (for long range: GPIO4 -> 1k -> NPN base, LED+33R from 3V3/5V to collector, emitter GND)
    IR receiver VS1838B/TSOP38238 (optional): OUT->GPIO15, VCC->3V3, GND->GND
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

const uint16_t IR_LED_PIN  = 4;
const uint16_t IR_RECV_PIN = 15;
const uint8_t  STATUS_LED  = 2;
const uint8_t  BOOT_BTN    = 0;
const char*    AP_SSID     = "MagicRemote";
const char*    AP_PASS     = "12345678";

IRsend irsend(IR_LED_PIN);
IRrecv irrecv(IR_RECV_PIN, 1024, 50, true);
decode_results results;
WebServer server(80);
Preferences prefs;

enum Brand { SAMSUNG_B, LG_B, SONY_B, PANASONIC_B, PHILIPS_B, BRAND_COUNT, AUTO_B = BRAND_COUNT };
const char* BRAND_IDS[] = {"samsung", "lg", "sony", "panasonic", "philips", "auto"};

enum Key { K_POWER, K_VOLUP, K_VOLDN, K_MUTE, K_CHUP, K_CHDN, K_UP, K_DOWN, K_LEFT, K_RIGHT,
           K_OK, K_MENU, K_HOME, K_BACK, K_INPUT, KEY_COUNT };
const char* KEY_IDS[] = {"power", "volup", "voldn", "mute", "chup", "chdn", "up", "down", "left",
                         "right", "ok", "menu", "home", "back", "input"};

// Samsung (32-bit SAMSUNG protocol)
const uint32_t SAMSUNG_CODES[KEY_COUNT] = {
  0xE0E040BF, 0xE0E0E01F, 0xE0E0D02F, 0xE0E0F00F, 0xE0E048B7, 0xE0E008F7, 0xE0E006F9, 0xE0E08679,
  0xE0E0A659, 0xE0E046B9, 0xE0E016E9, 0xE0E058A7, 0xE0E09E61, 0xE0E01AE5, 0xE0E0807F};
// LG (32-bit NEC)
const uint32_t LG_CODES[KEY_COUNT] = {
  0x20DF10EF, 0x20DF40BF, 0x20DFC03F, 0x20DF906F, 0x20DF00FF, 0x20DF807F, 0x20DF02FD, 0x20DF827D,
  0x20DFE01F, 0x20DF609F, 0x20DF22DD, 0x20DFC23D, 0x20DF3EC1, 0x20DF14EB, 0x20DFD02F};
// Sony SIRC (12-bit, except Back which is 15-bit)
const uint16_t SONY_CODES[KEY_COUNT] = {
  0xA90, 0x490, 0xC90, 0x290, 0x090, 0x890, 0x2F0, 0xAF0, 0x2D0, 0xCD0, 0xA70, 0x070, 0x070, 0x62E9, 0xA50};
// Panasonic (48-bit, address 0x4004)
const uint64_t PANASONIC_CODES[KEY_COUNT] = {
  0x40040100BCBD, 0x400401000405, 0x400401008485, 0x400401004C4D, 0x400401002C2D, 0x40040100ACAD,
  0x400401005253, 0x40040100D2D3, 0x400401007273, 0x40040100F2F3, 0x400401009293, 0x400401004A4B,
  0x400401004A4B, 0x400401002B2A, 0x40040100A0A1};
// Philips RC6 mode 0 (address 0)
const uint32_t PHILIPS_CODES[KEY_COUNT] = {
  0x0C, 0x10, 0x11, 0x0D, 0x4C, 0x4D, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x40, 0x54, 0x0A, 0x38};

int currentBrand = AUTO_B;
bool rc6Toggle = false;

// Learned code
bool hasLearned = false;
decode_type_t learnedType = UNKNOWN;
uint64_t learnedValue = 0;
uint16_t learnedBits = 0;
uint16_t* learnedRaw = nullptr;
uint16_t learnedRawLen = 0;
String learnedInfo = "none";

void blink() { digitalWrite(STATUS_LED, HIGH); delay(30); digitalWrite(STATUS_LED, LOW); }

void sendKey(int brand, int key) {
  switch (brand) {
    case SAMSUNG_B:   irsend.sendSAMSUNG(SAMSUNG_CODES[key], 32); break;
    case LG_B:        irsend.sendNEC(LG_CODES[key], 32); break;
    case SONY_B:      irsend.sendSony(SONY_CODES[key], SONY_CODES[key] > 0xFFF ? 15 : 12, 2); break;
    case PANASONIC_B: irsend.sendPanasonic64(PANASONIC_CODES[key], 48); break;
    case PHILIPS_B: {
      uint64_t d = PHILIPS_CODES[key];
      if (rc6Toggle) d = irsend.toggleRC6(d, kRC6Mode0Bits);
      irsend.sendRC6(d, kRC6Mode0Bits);
      break;
    }
  }
}

void press(int key) {
  irrecv.pause();  // don't capture our own transmission
  if (currentBrand == AUTO_B) {
    for (int b = 0; b < BRAND_COUNT; b++) { sendKey(b, key); delay(60); }
  } else {
    sendKey(currentBrand, key);
  }
  rc6Toggle = !rc6Toggle;
  irrecv.resume();
  blink();
  Serial.printf("[TX] brand=%s key=%s\n", BRAND_IDS[currentBrand], KEY_IDS[key]);
}

void universalPowerOff() {
  irrecv.pause();
  for (int round = 0; round < 2; round++)
    for (int b = 0; b < BRAND_COUNT; b++) { sendKey(b, K_POWER); delay(120); }
  irrecv.resume();
  blink(); blink();
  Serial.println("[TX] universal power sweep");
}

void replayLearned() {
  if (!hasLearned) return;
  irrecv.pause();
  bool ok = false;
  if (learnedType != UNKNOWN && !hasACState(learnedType))
    ok = irsend.send(learnedType, learnedValue, learnedBits);
  if (!ok && learnedRaw) irsend.sendRaw(learnedRaw, learnedRawLen, 38);
  irrecv.resume();
  blink();
  Serial.println("[TX] replay learned: " + learnedInfo);
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Magic Remote</title><style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font-family:system-ui,sans-serif;background:#0f1115;color:#e8e8ea;display:flex;justify-content:center}
.r{width:100%;max-width:360px;padding:16px}
h1{font-size:18px;text-align:center;margin:4px 0 12px;letter-spacing:.5px}
select{width:100%;padding:10px;border-radius:10px;border:1px solid #333;background:#1b1e25;color:#fff;font-size:15px;margin-bottom:14px}
.g{display:grid;gap:10px;margin-bottom:14px}.g3{grid-template-columns:repeat(3,1fr)}
button{border:0;border-radius:14px;padding:16px 0;font-size:15px;background:#232733;color:#fff;cursor:pointer;user-select:none}
button:active{background:#3b4252;transform:scale(.96)}
.pw{background:#c0392b}.pw:active{background:#e74c3c}.ok{background:#2d6cdf;border-radius:50%;aspect-ratio:1}
.u{background:#6c3fc7}.e{visibility:hidden}
.rk{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
.col{display:grid;gap:10px}.lab{text-align:center;font-size:12px;color:#888}
#st{text-align:center;font-size:12px;color:#8a8;min-height:16px}
#lr{font-size:12px;color:#aaa;text-align:center;word-break:break-all;margin:6px 0}
</style></head><body><div class="r">
<h1>✨ Magic Remote</h1>
<select id="b" onchange="fetch('/brand?b='+this.value)">
<option value="auto">Auto (all brands)</option><option value="samsung">Samsung</option><option value="lg">LG</option>
<option value="sony">Sony</option><option value="panasonic">Panasonic</option><option value="philips">Philips</option></select>
<div class="g g3"><button class="pw" data-k="power">⏻ Power</button><button data-k="input">Input</button><button data-k="mute">🔇</button></div>
<div class="g g3"><button data-k="menu">Menu</button><button data-k="up">▲</button><button data-k="home">⌂ Home</button>
<button data-k="left">◀</button><button class="ok" data-k="ok">OK</button><button data-k="right">▶</button>
<button data-k="back">↩ Back</button><button data-k="down">▼</button><span></span></div>
<div class="rk"><div class="col"><button data-k="volup">VOL +</button><div class="lab">Volume</div><button data-k="voldn">VOL −</button></div>
<div class="col"><button data-k="chup">CH +</button><div class="lab">Channel</div><button data-k="chdn">CH −</button></div></div>
<div class="g g3"><button class="u" onclick="go('/poweroff')">All OFF</button><button class="u" onclick="go('/replay')">Replay</button><button class="u" onclick="learn()">Learned?</button></div>
<div id="lr"></div><div id="st"></div></div><script>
const st=document.getElementById('st');
function go(u){fetch(u).then(r=>r.text()).then(t=>{st.textContent=t;setTimeout(()=>st.textContent='',900)})}
document.querySelectorAll('[data-k]').forEach(b=>{let t;const k=b.dataset.k;
 const s=()=>go('/key?k='+k);
 b.addEventListener('pointerdown',e=>{e.preventDefault();s();if(/vol|ch|up|down|left|right/.test(k))t=setInterval(s,220)});
 ['pointerup','pointerleave','pointercancel'].forEach(ev=>b.addEventListener(ev,()=>clearInterval(t)))});
function learn(){fetch('/learned').then(r=>r.text()).then(t=>document.getElementById('lr').textContent='Last learned: '+t)}
fetch('/brand').then(r=>r.text()).then(t=>document.getElementById('b').value=t);
</script></body></html>)HTML";

void handleKey() {
  String k = server.arg("k");
  for (int i = 0; i < KEY_COUNT; i++)
    if (k == KEY_IDS[i]) { press(i); server.send(200, "text/plain", "sent " + k); return; }
  server.send(400, "text/plain", "unknown key");
}

void handleBrand() {
  if (server.hasArg("b")) {
    String b = server.arg("b");
    for (int i = 0; i <= BRAND_COUNT; i++)
      if (b == BRAND_IDS[i]) { currentBrand = i; prefs.putInt("brand", i); }
  }
  server.send(200, "text/plain", BRAND_IDS[currentBrand]);
}

void checkReceiver() {
  if (!irrecv.decode(&results)) return;
  if (results.overflow || results.rawlen < 12) { irrecv.resume(); return; }
  learnedType = results.decode_type;
  learnedValue = results.value;
  learnedBits = results.bits;
  if (learnedRaw) delete[] learnedRaw;
  learnedRaw = resultToRawArray(&results);
  learnedRawLen = getCorrectedRawLength(&results);
  hasLearned = true;
  learnedInfo = typeToString(learnedType) + " 0x" + uint64ToString(learnedValue, 16) + " (" + String(learnedBits) + " bits)";
  Serial.println("[RX] learned: " + learnedInfo);
  // Auto-detect brand from a received code
  if (learnedType == SAMSUNG) currentBrand = SAMSUNG_B;
  else if (learnedType == NEC && (learnedValue >> 16) == 0x20DF) currentBrand = LG_B;
  else if (learnedType == SONY) currentBrand = SONY_B;
  else if (learnedType == PANASONIC) currentBrand = PANASONIC_B;
  else if (learnedType == RC6 || learnedType == RC5) currentBrand = PHILIPS_B;
  prefs.putInt("brand", currentBrand);
  blink();
  irrecv.resume();
}

void checkButton() {
  static uint32_t downAt = 0;
  static bool wasDown = false;
  bool down = digitalRead(BOOT_BTN) == LOW;
  if (down && !wasDown) downAt = millis();
  if (!down && wasDown) {
    uint32_t held = millis() - downAt;
    if (held > 800) universalPowerOff();
    else if (held > 40) press(K_POWER);
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  irsend.begin();
  irrecv.enableIRIn();
  pinMode(IR_RECV_PIN, INPUT_PULLUP);  // keep line idle-high when no receiver is fitted
  prefs.begin("remote", false);
  currentBrand = prefs.getInt("brand", AUTO_B);
  if (currentBrand < 0 || currentBrand > BRAND_COUNT) currentBrand = AUTO_B;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/key", handleKey);
  server.on("/brand", handleBrand);
  server.on("/poweroff", [] { universalPowerOff(); server.send(200, "text/plain", "power sweep sent"); });
  server.on("/replay", [] {
    if (!hasLearned) { server.send(200, "text/plain", "nothing learned yet"); return; }
    replayLearned(); server.send(200, "text/plain", "replayed");
  });
  server.on("/learned", [] { server.send(200, "text/plain", learnedInfo); });
  server.begin();

  Serial.println();
  Serial.println("=== MAGIC REMOTE READY ===");
  Serial.printf("WiFi: %s  pass: %s\n", AP_SSID, AP_PASS);
  Serial.print("Open: http://"); Serial.println(WiFi.softAPIP());
  Serial.printf("IR LED GPIO%d | IR RX GPIO%d | Brand: %s\n", IR_LED_PIN, IR_RECV_PIN, BRAND_IDS[currentBrand]);
}

void loop() {
  server.handleClient();
  checkReceiver();
  checkButton();
}
