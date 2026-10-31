/*
  ESP32 Bluetooth TV Mouse + Keyboard ("Magic Remote")
  ----------------------------------------------------
  ESP32 pretends to be a Bluetooth mouse + keyboard named "MagicRemote".
  Pair it on the TV: Settings > General > External Device Manager >
                     Input Device Manager > Bluetooth Device List > MagicRemote.
  Your phone (on home Wi-Fi) opens http://magicremote.local (or the IP):
    - drag on the touchpad = move the TV cursor, tap = click
    - arrows / OK / Back / Home / Volume / Mute / Play buttons
    - type text into search boxes
  Power uses IR (G4 -> 100R -> IR LED -> GND), because Bluetooth can't wake a TV.
  BOOT button: hold 5 s = forget Wi-Fi.

  Build/flash (this board's flash misreads at 80 MHz once Wi-Fi + BT run):
    arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,FlashFreq=40,FlashMode=dio BleTvMouse
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

const uint16_t IR_LED_PIN = 4;
const uint8_t  STATUS_LED = 2;
const uint8_t  BOOT_BTN   = 0;
const char*    SETUP_SSID = "MagicRemote-Setup";
const char*    HOSTNAME   = "magicremote";
const uint32_t SAMSUNG_IR_POWER = 0xE0E040BF;

WebServer server(80);
WebSocketsServer wsServer(81);
DNSServer dns;
Preferences prefs;
IRsend irsend(IR_LED_PIN);
bool setupMode = false;

// ---------------- Bluetooth HID ----------------

enum { RID_MOUSE = 1, RID_KEYBOARD = 2, RID_CONSUMER = 3 };

const uint8_t REPORT_MAP[] = {
  // Mouse: 3 buttons, X, Y, wheel
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, RID_MOUSE, 0x09, 0x01, 0xA1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0xC0, 0xC0,
  // Keyboard: modifiers, reserved, 6 keys
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RID_KEYBOARD,
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
  0xC0,
  // Consumer control: one 16-bit usage (volume, mute, home, back...)
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RID_CONSUMER, 0x15, 0x00, 0x26, 0xFF, 0x03,
  0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
};

NimBLEHIDDevice* hid;
NimBLECharacteristic *mouseIn, *keyIn, *consumerIn;
volatile bool bleConnected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    bleConnected = true;
    Serial.printf("[BT] TV connected: %s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    bleConnected = false;
    Serial.printf("[BT] disconnected (reason %d), advertising again\n", reason);
    NimBLEDevice::startAdvertising();
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[BT] pairing %s\n", info.isEncrypted() ? "done (encrypted)" : "FAILED");
  }
};

void startBle() {
  NimBLEDevice::init("MagicRemote");
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCallbacks());

  hid = new NimBLEHIDDevice(srv);
  hid->setManufacturer("Espressif");
  hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
  mouseIn = hid->getInputReport(RID_MOUSE);
  keyIn = hid->getInputReport(RID_KEYBOARD);
  consumerIn = hid->getInputReport(RID_CONSUMER);
  hid->setBatteryLevel(100);
  srv->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C2);  // HID mouse
  adv->addServiceUUID(hid->getHidService()->getUUID());
  adv->setName("MagicRemote");
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("[BT] advertising as \"MagicRemote\"");
}

void mouseReport(uint8_t buttons, int dx, int dy, int wheel) {
  if (!bleConnected) return;
  uint8_t r[4] = {buttons, (uint8_t)(int8_t)constrain(dx, -127, 127),
                  (uint8_t)(int8_t)constrain(dy, -127, 127), (uint8_t)(int8_t)constrain(wheel, -127, 127)};
  mouseIn->setValue(r, sizeof(r));
  mouseIn->notify();
}

void click() { mouseReport(1, 0, 0, 0); delay(15); mouseReport(0, 0, 0, 0); }

void keyTap(uint8_t usage, uint8_t mods = 0) {
  if (!bleConnected) return;
  uint8_t r[8] = {mods, 0, usage, 0, 0, 0, 0, 0};
  keyIn->setValue(r, sizeof(r)); keyIn->notify();
  delay(12);
  memset(r, 0, sizeof(r));
  keyIn->setValue(r, sizeof(r)); keyIn->notify();
  delay(8);
}

void consumerTap(uint16_t usage) {
  if (!bleConnected) return;
  uint8_t r[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  consumerIn->setValue(r, 2); consumerIn->notify();
  delay(15);
  r[0] = r[1] = 0;
  consumerIn->setValue(r, 2); consumerIn->notify();
}

void typeChar(char c) {
  const uint8_t SHIFT = 0x02;
  if (c >= 'a' && c <= 'z') keyTap(0x04 + c - 'a');
  else if (c >= 'A' && c <= 'Z') keyTap(0x04 + c - 'A', SHIFT);
  else if (c >= '1' && c <= '9') keyTap(0x1E + c - '1');
  else if (c == '0') keyTap(0x27);
  else if (c == ' ') keyTap(0x2C);
  else if (c == '.') keyTap(0x37);
  else if (c == ',') keyTap(0x36);
  else if (c == '-') keyTap(0x2D);
  else if (c == '_') keyTap(0x2D, SHIFT);
  else if (c == '@') keyTap(0x1F, SHIFT);
  else if (c == '/') keyTap(0x38);
  else if (c == '?') keyTap(0x38, SHIFT);
  else if (c == '!') keyTap(0x1E, SHIFT);
}

struct NamedKey { const char* id; uint8_t kb; uint16_t cc; };
const NamedKey NAMED[] = {
  {"up", 0x52, 0}, {"down", 0x51, 0}, {"left", 0x50, 0}, {"right", 0x4F, 0},
  {"ok", 0x28, 0}, {"back", 0, 0x224}, {"home", 0, 0x223}, {"esc", 0x29, 0},
  {"bksp", 0x2A, 0}, {"volup", 0, 0xE9}, {"voldn", 0, 0xEA}, {"mute", 0, 0xE2},
  {"play", 0, 0xCD},
};

void handleKey(const String& id) {
  if (id == "power") {
    irsend.sendSAMSUNG(SAMSUNG_IR_POWER, 32);
    Serial.println("[IR] power");
    return;
  }
  for (const NamedKey& k : NAMED) {
    if (id != k.id) continue;
    if (k.kb) keyTap(k.kb); else consumerTap(k.cc);
    return;
  }
}

// Phone -> ESP32 messages over websocket:
//   m,dx,dy   move   | c  click | w,n  wheel | k,name  key | t,text  type text
void onWsMsg(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED) {
    wsServer.sendTXT(num, bleConnected ? "bt:1" : "bt:0");
    return;
  }
  if (type != WStype_TEXT || len == 0) return;
  String msg((const char*)payload, len);
  char cmd = msg[0];
  String arg = msg.length() > 2 ? msg.substring(2) : "";
  if (cmd == 'm') {
    int comma = arg.indexOf(',');
    mouseReport(0, arg.substring(0, comma).toInt(), arg.substring(comma + 1).toInt(), 0);
  } else if (cmd == 'c') {
    click(); digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
  } else if (cmd == 'w') {
    mouseReport(0, 0, 0, arg.toInt());
  } else if (cmd == 'k') {
    handleKey(arg);
  } else if (cmd == 't') {
    for (char c : arg) typeChar(c);
  }
}

// ---------------- Web pages ----------------

const char REMOTE_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Magic Remote</title><style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
body{margin:0;font-family:system-ui,sans-serif;background:#0f1115;color:#e8e8ea;display:flex;justify-content:center;user-select:none}
.r{width:100%;max-width:380px;padding:12px 16px}
#st{text-align:center;font-size:13px;margin:4px 0 10px}.on{color:#5c5}.off{color:#e96}
#pad{height:260px;border-radius:18px;background:#1b1e25;border:1px solid #2c313c;touch-action:none;display:flex;
 align-items:center;justify-content:center;color:#555;font-size:13px;margin-bottom:12px}
.g{display:grid;gap:9px;margin-bottom:12px}.g3{grid-template-columns:repeat(3,1fr)}.g4{grid-template-columns:repeat(4,1fr)}
button{border:0;border-radius:12px;padding:14px 0;font-size:15px;background:#232733;color:#fff}
button:active{background:#3b4252;transform:scale(.96)}
.pw{background:#c0392b}.ok{background:#1f5fd1}
.tx{display:flex;gap:8px}.tx input{flex:1;padding:12px;border-radius:12px;border:1px solid #333;background:#1b1e25;color:#fff;font-size:16px}
.tx button{padding:0 16px}
</style></head><body><div class="r">
<div id="st" class="off">Connecting...</div>
<div id="pad">drag = move cursor &middot; tap = click</div>
<div class="g g4"><button data-k="up">&#9650;</button><button data-w="3">Scroll &#9650;</button><button data-w="-3">Scroll &#9660;</button><button data-k="down">&#9660;</button></div>
<div class="g g3"><button data-k="left">&#9664;</button><button class="ok" data-k="ok">OK</button><button data-k="right">&#9654;</button>
<button data-k="back">Back</button><button data-k="home">Home</button><button data-k="play">Play/Pause</button>
<button data-k="voldn">VOL &minus;</button><button data-k="mute">Mute</button><button data-k="volup">VOL +</button></div>
<div class="g g3"><button class="pw" data-k="power">Power (IR)</button><button data-k="esc">Esc</button><button data-k="bksp">&#9003;</button></div>
<div class="tx"><input id="txt" placeholder="Type text for search..."><button onclick="sendText()">Send</button></div>
</div><script>
let ws,open=false;const st=document.getElementById('st');
function conn(){ws=new WebSocket('ws://'+location.hostname+':81/');
 ws.onopen=()=>{open=true};ws.onclose=()=>{open=false;st.textContent='Reconnecting...';st.className='off';setTimeout(conn,1500)};
 ws.onmessage=e=>{if(e.data=='bt:1'){st.textContent='TV connected via Bluetooth';st.className='on'}
  else if(e.data=='bt:0'){st.textContent='Not paired - pair "MagicRemote" in TV Bluetooth settings';st.className='off'}}}
conn();setInterval(()=>{if(open)ws.send('p')},3000);
function send(m){if(open)ws.send(m)}
const pad=document.getElementById('pad');let last=null,moved=0,acc=[0,0],t0=0;
pad.addEventListener('pointerdown',e=>{pad.setPointerCapture(e.pointerId);last=[e.clientX,e.clientY];moved=0;t0=Date.now()});
pad.addEventListener('pointermove',e=>{if(!last)return;const dx=e.clientX-last[0],dy=e.clientY-last[1];last=[e.clientX,e.clientY];
 moved+=Math.abs(dx)+Math.abs(dy);const sp=1.8+Math.min(2,Math.hypot(dx,dy)/12);acc[0]+=dx*sp;acc[1]+=dy*sp});
pad.addEventListener('pointerup',()=>{if(moved<8&&Date.now()-t0<300)send('c');last=null});
setInterval(()=>{const x=Math.round(acc[0]),y=Math.round(acc[1]);if(x||y){send('m,'+x+','+y);acc=[acc[0]-x,acc[1]-y]}},30);
document.querySelectorAll('[data-k]').forEach(b=>{let iv;const k=b.dataset.k;
 b.addEventListener('pointerdown',e=>{e.preventDefault();send('k,'+k);if(/vol|up|down|left|right|bksp/.test(k))iv=setInterval(()=>send('k,'+k),220)});
 ['pointerup','pointerleave','pointercancel'].forEach(ev=>b.addEventListener(ev,()=>clearInterval(iv)))});
document.querySelectorAll('[data-w]').forEach(b=>b.addEventListener('click',()=>send('w,'+b.dataset.w)));
function sendText(){const i=document.getElementById('txt');if(i.value){send('t,'+i.value);i.value=''}}
document.getElementById('txt').addEventListener('keydown',e=>{if(e.key=='Enter')sendText()});
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
    server.send(200, "text/html", "<body style='font-family:system-ui;padding:20px'><h3>Saved, restarting...</h3></body>");
    delay(1500);
    ESP.restart();
  });
  server.onNotFound([] { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); });
  server.begin();
  Serial.println("=== SETUP MODE ===");
  Serial.printf("Join Wi-Fi \"%s\" and open http://192.168.4.1\n", SETUP_SSID);
}

void startRemoteMode() {
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  server.on("/", [] { server.send_P(200, "text/html", REMOTE_PAGE); });
  server.begin();
  wsServer.begin();
  wsServer.onEvent(onWsMsg);
  Serial.println("=== MAGIC REMOTE READY (Bluetooth mouse) ===");
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
    ESP.restart();
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  irsend.begin();
  prefs.begin("remote", false);
  WiFi.setHostname(HOSTNAME);
  startBle();

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
    Serial.println("Wi-Fi connect failed -> setup mode");
    startSetupMode();
    return;
  }
  startRemoteMode();
}

void loop() {
  server.handleClient();
  checkButton();
  if (setupMode) { dns.processNextRequest(); return; }
  wsServer.loop();
  // Keep the phone's status line current when the TV pairs/unpairs.
  static bool lastBt = false;
  if (bleConnected != lastBt) {
    lastBt = bleConnected;
    wsServer.broadcastTXT(lastBt ? "bt:1" : "bt:0");
    digitalWrite(STATUS_LED, lastBt);
  }
}
