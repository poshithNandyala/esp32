/*
  ESP32 BLE Typist — New simplified Code Mode + Play/Pause
  - Fixed: Code Mode skipping leading ASCII spaces at the start of each line
    * More robust: tracks "start-of-line" state (including start of text) and skips ASCII 0x20 spaces
    * Tabs (\t) and other whitespace are left alone per original spec
  - Added a Play/Pause toggle that pauses and resumes an in-progress typing job
  - Pause is cooperative: the typing loop and coopDelay yield to HTTP so /pause and /stop work
  - Everything else left intact; minimal additions only
*/

#include <WiFi.h>
#include <WebServer.h>
#include <BleKeyboard.h>

// BLE identity
BleKeyboard bleKeyboard("Logitech K380", "Logitech", 100);

// WiFi AP
const char* AP_SSID = "ESP32_Control";
const char* AP_PASS = "qwertyuiop120";

WebServer server(80);

// Runtime config (defaults)
volatile int configuredWPM = 100;
volatile bool strictWPM = false;
volatile int jitterStrengthPct = 12;
volatile int thinkingSpaceChance = 0;
volatile int mistakePercent = 3;
volatile bool enableTypos = true;
volatile bool enableLongPauses = true;
volatile int longPausePercent = 5;
volatile int longPauseMinMs = 600;
volatile int longPauseMaxMs = 1200;
volatile int newlineMode = 1; // 0 keep,1 space,2 remove
volatile bool extraPunctPause = true;

// Code mode config (single toggle)
volatile bool codeMode = false;    // OFF by default

// Runtime state
volatile bool typingActive = false;
volatile bool paused = false; // pause/resume state
volatile unsigned long typedChars = 0;

// HTML UI
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en"><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 BLE Typist</title>
<style>
:root{--bg:#0b0f14;--card:#121821;--muted:#a7b1c2;--acc:#4bb3fd}
*{box-sizing:border-box}
body{margin:12px;font-family:Inter,system-ui,Arial,sans-serif;background:#0b0f14;color:#e6edf6}
.container{max-width:1100px;margin:0 auto;display:grid;grid-template-columns:1fr;gap:14px}
@media(min-width:980px){.container{grid-template-columns:1fr 380px}}
.card{background:var(--card);border-radius:12px;padding:12px;border:1px solid #172027}
label{display:block;color:var(--muted);font-size:13px;margin-bottom:6px}
input[type=number],select,textarea{width:100%;padding:8px;border-radius:8px;border:1px solid #22303a;background:#071018;color:#e6edf6}
textarea{min-height:220px;font-family:ui-monospace,Consolas,monospace;white-space:pre;overflow-wrap:normal}
.row{margin-bottom:10px}
.controls{display:flex;gap:8px;flex-wrap:wrap}
button{padding:8px 12px;border-radius:999px;border:none;background:var(--acc);color:#071019;font-weight:700;cursor:pointer}
button.ghost{background:transparent;border:1px solid #22303a;color:#e6edf6}
.status{font-family:ui-monospace,Consolas,monospace;background:#071018;padding:10px;border-radius:8px;border:1px solid #122026}
.small{font-size:12px;color:var(--muted)}
</style>
</head><body>
<div class="container">
  <div class="card">
    <h1>ESP32 BLE Typist</h1>
    <div class="row">
      <label title="Paste or type the text you want the ESP32 to send as keyboard input">Text to type</label>
      <textarea id="text" placeholder="Paste your code or text here..."></textarea>
    </div>
    <div class="row controls">
      <button onclick="sendText()" title="Send the text to the paired device">Type on target</button>
      <button class="ghost" onclick="applyConfig()" title="Apply current settings">Apply settings</button>
      <button class="ghost" onclick="getStatus()" title="Refresh status from ESP32">Status</button>
      <button class="ghost" onclick="stopTyping()" title="Immediately stop an in-progress typing job">STOP</button>
      <!-- Play/Pause button -->
      <button class="ghost" id="playpause" onclick="togglePause()" title="Pause or resume typing">Play/Pause</button>
    </div>
    <div class="row"><div class="status" id="status">Ready.</div></div>
  </div>

  <div class="card">
    <h2>Behavior</h2>
    <div class="row">
      <label title="Words per minute the device will aim for">WPM (10–300)</label>
      <input id="wpm" type="number" min="10" max="300" value="100" />
    </div>

    <div class="row">
      <label title="Strict mode disables extras (pauses, typos) and aims for exact timing">Strict WPM</label>
      <select id="strict"><option value="0">Off</option><option value="1">On</option></select>
    </div>

    <div class="row">
      <label title="Random timing variation in percent — higher = more human-like jitter">Jitter (%)</label>
      <input id="jitter" type="number" min="5" max="45" value="12" />
    </div>

    <div class="row">
      <label title="Chance (1 in N) that a thinking pause occurs after a space — set 0 to disable">Thinking pause (1 in N)</label>
      <input id="think" type="number" min="0" max="100" value="0" />
    </div>

    <div class="row">
      <label title="Enable simulated typos and corrections">Enable typos</label>
      <select id="typos"><option value="1">Yes</option><option value="0">No</option></select>
    </div>

    <div class="row">
      <label title="Enable occasional longer thinking pauses at spaces">Enable long pauses</label>
      <select id="lpen"><option value="1">Yes</option><option value="0">No</option></select>
    </div>

    <div class="row">
      <label title="When encountering newlines: keep Enter / replace with space / remove">Newline handling</label>
      <select id="nl"><option value="0">Keep Enter</option><option value="1" selected>Replace with space</option><option value="2">Remove</option></select>
    </div>

    <hr style="border-color:#172027" />

    <h2>Code mode (simple)</h2>
    <div class="row">
      <label title="Enable simple Code Mode (ignore spaces at start of line)">Code Mode</label>
      <select id="codemode"><option value="0">Off</option><option value="1">On</option></select>
    </div>

    <div class="small">When Code Mode is ON the typist will skip ASCII space characters at the start of each line only. Tabs and all other characters are sent normally.</div>
  </div>
</div>

<script>
async function applyConfig(){
  const p = new URLSearchParams({
    wpm: document.getElementById('wpm').value,
    strict: document.getElementById('strict').value,
    jitter: document.getElementById('jitter').value,
    think: document.getElementById('think').value,
    typos: document.getElementById('typos').value,
    lpen: document.getElementById('lpen').value,
    nl: document.getElementById('nl').value,
    codemode: document.getElementById('codemode').value
  });
  const r = await fetch('/config?' + p.toString());
  const t = await r.text();
  document.getElementById('status').innerText = t;
}

async function sendText(){
  await applyConfig();
  const data = document.getElementById('text').value;
  if(!data || data.trim()===''){ document.getElementById('status').innerText='Nothing to send'; return; }
  document.getElementById('status').innerText='Sending...';
  try{
    const r = await fetch('/type', {method:'POST', headers:{'Content-Type':'text/plain'}, body:data});
    const t = await r.text();
    document.getElementById('status').innerText = t;
  }catch(e){ document.getElementById('status').innerText = 'Error: '+e }
}

async function stopTyping(){
  try{ const r = await fetch('/stop'); const t = await r.text(); document.getElementById('status').innerText = t; getStatus(); }catch(e){ document.getElementById('status').innerText = 'Stop error'; }
}

// toggle pause/resume
async function togglePause(){
  try{
    const r = await fetch('/pause');
    const t = await r.text();
    document.getElementById('status').innerText = t;
    getStatus();
  }catch(e){ document.getElementById('status').innerText='Pause error: '+e }
}

async function getStatus(){
  try{
    const r = await fetch('/status');
    const j = await r.json();
    document.getElementById('status').innerText = JSON.stringify(j, null, 2);
    document.getElementById('wpm').value=j.wpm;
    document.getElementById('strict').value = j.strict?1:0;
    document.getElementById('jitter').value=j.jitter;
    document.getElementById('think').value=j.think;
    document.getElementById('typos').value=j.typos?1:0;
    document.getElementById('lpen').value=j.lpen?1:0;
    document.getElementById('nl').value=j.nl;
    document.getElementById('codemode').value = j.codemode?1:0;
  }catch(e){ document.getElementById('status').innerText='Status error: '+e }
}

getStatus();
</script>
</body></html>
)rawliteral";

// Utilities
String readRequestBody(){ if(server.hasArg("plain")) return server.arg("plain"); return String(); }
int clampInt(int v,int a,int b){ if(v<a) return a; if(v>b) return b; return v; }
static inline float ms_per_char_for_wpm(int wpm){ if(wpm<1) wpm=1; return 60000.0f / (wpm * 5.0f); }

// Cooperative delay that allows STOP + HTTP handling during waits
void coopDelay(unsigned long ms){
  unsigned long start = millis();
  while(typingActive && (millis() - start < ms)){
    server.handleClient();
    // If paused, remain responsive to HTTP but don't advance time-based waits
    while(paused && typingActive){
      server.handleClient();
      delay(1);
      yield();
    }
    delay(1);
    yield();
  }
}

void sendChar(char c){ bleKeyboard.print(c); }
void sendBackspace(){ bleKeyboard.write(KEY_BACKSPACE); }

// Preprocess newline handling (non-code mode)
String preprocessText(const String &in){
  if(newlineMode==0) return in;
  String out; out.reserve(in.length());
  for(size_t i=0;i<in.length();++i){
    char c = in[i];
    if(c == '\r' || c == '\n'){
      if(newlineMode == 1) out += ' ';
      // if mode 2 => drop entirely
    } else {
      out += c;
    }
  }
  return out;
}

// Small helper to cap jitter at very high WPM
static inline float capJitterForWPM(int wpm, float jpct){ if(wpm >= 140 && jpct > 0.08f) return 0.08f; return jpct; }

// Typing engine — simplified: codeMode only skips leading spaces after newlines
void typeLikeHuman(const String &rawText){
  if(!bleKeyboard.isConnected()) return;

  // If code mode OFF, use existing pipeline with newline preprocessing
  if(!codeMode){
    String text = preprocessText(rawText);
    size_t N = text.length();
    if(N == 0) return;

    typingActive = true;
    paused = false; // ensure not paused when starting
    typedChars = 0;

    int sessionWPM = clampInt(configuredWPM + random(-2,3), 10, 300);
    float baseMs = ms_per_char_for_wpm(sessionWPM);
    float jitterPct = clampInt(jitterStrengthPct,5,45) / 100.0f;
    jitterPct = capJitterForWPM(sessionWPM, jitterPct);
    bool strict = strictWPM;

    const float MIN_DELAY = 6.0f; const float CORR_LIMIT = 0.5f;
    unsigned long startMs = millis();

    for(size_t i=0;i<N && typingActive;++i){
      if(!bleKeyboard.isConnected()) break;

      // If paused, wait here (still service HTTP)
      while(paused && typingActive){ server.handleClient(); delay(1); yield(); }

      unsigned long now = millis(); float elapsed = float(now - startMs);
      size_t remaining = (N - i); if(remaining==0) remaining = 1;
      float idealElapsed = float(i) * baseMs;
      float error = elapsed - idealElapsed;
      float correction = -error / float(remaining);
      if(correction > baseMs*CORR_LIMIT) correction = baseMs*CORR_LIMIT;
      if(correction < -baseMs*CORR_LIMIT) correction = -baseMs*CORR_LIMIT;

      float nextDelay = baseMs + correction; if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;
      float jitterFactor = 1.0f + ((random(-1000,1001)/1000.0f) * jitterPct);
      nextDelay *= jitterFactor; if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;

      char c = text[i];
      bool isSpace = (c==' ');
      bool isPunct = (c=='.'||c==','||c=='!'||c=='?'||c==';'||c==':');

      if(!strict && enableLongPauses && isSpace && (random(0,100) < longPausePercent)){
        coopDelay(random(longPauseMinMs, longPauseMaxMs+1)); if(!typingActive) break;
      }

      bool alnum = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'));
      bool makeTypo = (!strict) && enableTypos && (random(0,100) < mistakePercent) && alnum;
      if(makeTypo){
        char wrong = char('a' + (random(0,25)));
        if(isupper(c)) wrong = toupper(wrong);
        sendChar(wrong);
        coopDelay(max(60, (int)nextDelay)); if(!typingActive) break;
        sendBackspace(); coopDelay(random(110,380)); if(!typingActive) break;
        sendChar(c); coopDelay(clampInt((int)(nextDelay*0.5f)+random(20,120), 20, 800)); if(!typingActive) break;
      } else {
        sendChar(c);
        int extra = 0;
        if(!strict){
          if(isSpace) extra += random(40,140);
          if(extraPunctPause && isPunct) extra += random(80,220);
          if(c == '\n' || c == '\r') extra += random(120,320);
        }
        coopDelay((unsigned long)nextDelay + extra); if(!typingActive) break;
      }

      if(!strict && isSpace && thinkingSpaceChance>0 && (random(0,thinkingSpaceChance)==0)){
        coopDelay(random(400,1000)); if(!typingActive) break;
      }

      typedChars = i+1;
    }

    typingActive = false; paused = false; // clear paused state when finished
    coopDelay(120 + random(0,300));
    return;
  }

  // ---------------- Code Mode ON: only skip ASCII spaces after newlines ----------------
  String text = rawText; // do not preprocess newlines here
  int N = (int)text.length();
  if(N == 0) return;

  typingActive = true;
  paused = false; // ensure not paused when starting
  typedChars = 0;

  int sessionWPM = clampInt(configuredWPM + random(-2,3), 10, 300);
  float baseMs = ms_per_char_for_wpm(sessionWPM);
  float jitterPct = clampInt(jitterStrengthPct,5,45) / 100.0f;
  jitterPct = capJitterForWPM(sessionWPM, jitterPct);
  bool strict = strictWPM;

  const float MIN_DELAY = 6.0f; const float CORR_LIMIT = 0.5f;
  unsigned long startMs = millis();

  // Track start-of-line state. Initialize to true so leading spaces at the very start are skipped.
  bool startOfLine = true;

  for(int i=0; i < N && typingActive; ++i){
    if(!bleKeyboard.isConnected()) break;

    // If paused, wait here (still service HTTP)
    while(paused && typingActive){ server.handleClient(); delay(1); yield(); }

    char c = text[i];

    // If we're at the start of a line and encounter ASCII space (0x20), skip it.
    // This also handles the very start of the input since startOfLine starts true.
    if(startOfLine && c == ' '){
      // advance typed count and continue without sending or adding delays
      typedChars = i+1;
      continue; // skip this space
    }

    // timing correction like before (moved after the quick skip to avoid skew)
    unsigned long now = millis(); float elapsed = float(now - startMs);
    int remaining = (N - i); if(remaining==0) remaining = 1;
    float idealElapsed = float(i) * baseMs;
    float error = elapsed - idealElapsed;
    float correction = -error / float(remaining);
    if(correction > baseMs*CORR_LIMIT) correction = baseMs*CORR_LIMIT;
    if(correction < -baseMs*CORR_LIMIT) correction = -baseMs*CORR_LIMIT;

    float nextDelay = baseMs + correction; if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;
    float jitterFactor = 1.0f + ((random(-1000,1001)/1000.0f) * jitterPct);
    nextDelay *= jitterFactor; if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;

    // Newline handling: send newline and then skip ONLY ASCII spaces (0x20) at the start of next line
    if(c == '\r' || c == '\n'){
      // Detect CRLF and skip the LF in the input
      bool isCRLF = false;
      if(c == '\r' && (i+1) < N && text[i+1] == '\n'){
        isCRLF = true;
      }

      // Send a single newline for portability
      sendChar('\n');

      if(isCRLF) { i++; }

      // After a newline, mark startOfLine so subsequent ASCII spaces are skipped by the fast path above
      startOfLine = true;

      // small pause after newline
      coopDelay((unsigned long)nextDelay);
      typedChars = i+1;
      continue;
    }

    // Normal character sending with existing typo/timing logic
    bool isSpace = (c == ' ');
    bool isPunct = (c=='.'||c==','||c=='!'||c=='?'||c==';'||c==':');

    if(!strict && enableLongPauses && isSpace && (random(0,100) < longPausePercent)){
      coopDelay(random(longPauseMinMs, longPauseMaxMs+1)); if(!typingActive) break;
    }

    bool alnum = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'));
    bool makeTypo = (!strict) && enableTypos && (random(0,100) < mistakePercent) && alnum;

    if(makeTypo){
      char wrong = char('a' + (random(0,25)));
      if(isupper(c)) wrong = toupper(wrong);
      sendChar(wrong);
      coopDelay(max(60, (int)nextDelay)); if(!typingActive) break;
      sendBackspace(); coopDelay(random(110,380)); if(!typingActive) break;
      sendChar(c); coopDelay(clampInt((int)(nextDelay*0.5f)+random(20,120), 20, 800)); if(!typingActive) break;
    } else {
      sendChar(c);
      int extra = 0;
      if(!strict){
        if(isSpace) extra += random(40,140);
        if(extraPunctPause && isPunct) extra += random(80,220);
        if(c=='\n' || c=='\r') extra += random(120,320);
      }
      coopDelay((unsigned long)nextDelay + extra); if(!typingActive) break;
    }

    // Any non-newline character means we're no longer at start-of-line
    startOfLine = false;

    if(!strict && isSpace && thinkingSpaceChance>0 && (random(0,thinkingSpaceChance)==0)){
      coopDelay(random(400,1000)); if(!typingActive) break;
    }

    typedChars = i+1;
  }

  typingActive = false;
  paused = false; // clear paused state when finished
  coopDelay(120 + random(0,300));
}

// HTTP Handlers
void handleRoot(){ server.send_P(200, "text/html", INDEX_HTML); }

void handleStatus(){
  String s = "{";
  s += "\"ble\":" + String(bleKeyboard.isConnected()?"true":"false") + ",";
  s += "\"wpm\":" + String(configuredWPM) + ",";
  s += "\"strict\":" + String(strictWPM?"true":"false") + ",";
  s += "\"jitter\":" + String(jitterStrengthPct) + ",";
  s += "\"think\":" + String(thinkingSpaceChance) + ",";
  s += "\"typos\":" + String(enableTypos?"true":"false") + ",";
  s += "\"lpen\":" + String(enableLongPauses?"true":"false") + ",";
  s += "\"nl\":" + String(newlineMode) + ",";
  s += "\"codemode\":" + String(codeMode?"true":"false") + ",";
  s += "\"typed\":" + String((unsigned long)typedChars) + ",";
  s += "\"running\":" + String(typingActive?"true":"false") + ",";
  s += "\"paused\":" + String(paused?"true":"false") + ",";
  s += "\"state\":\"" + String(typingActive?"Typing...":"Ready.") + "\"";
  s += "}";
  server.send(200, "application/json", s);
}

void handleConfig(){
  bool changed=false;
  if(server.hasArg("wpm")){ configuredWPM = clampInt(server.arg("wpm").toInt(), 10, 300); changed=true; }
  if(server.hasArg("strict")){ strictWPM = (server.arg("strict").toInt()!=0); changed=true; }
  if(server.hasArg("jitter")){ jitterStrengthPct = clampInt(server.arg("jitter").toInt(), 5, 45); changed=true; }
  if(server.hasArg("think")){ thinkingSpaceChance = clampInt(server.arg("think").toInt(), 0, 100); changed=true; }
  if(server.hasArg("typos")){ enableTypos = (server.arg("typos").toInt()!=0); changed=true; }
  if(server.hasArg("lpen")){ enableLongPauses = (server.arg("lpen").toInt()!=0); changed=true; }
  if(server.hasArg("lpc")){ longPausePercent = clampInt(server.arg("lpc").toInt(), 0, 100); changed=true; }
  if(server.hasArg("lpmin")){ longPauseMinMs = clampInt(server.arg("lpmin").toInt(), 50, 20000); changed=true; }
  if(server.hasArg("lpmax")){ longPauseMaxMs = clampInt(server.arg("lpmax").toInt(), 50, 30000); changed=true; }
  if(server.hasArg("nl")){ newlineMode = clampInt(server.arg("nl").toInt(), 0, 2); changed=true; }

  // code mode (single toggle)
  if(server.hasArg("codemode")){ codeMode = (server.arg("codemode").toInt()!=0); changed=true; }

  if(longPauseMinMs > longPauseMaxMs){ int t = longPauseMinMs; longPauseMinMs = longPauseMaxMs; longPauseMaxMs = t; }
  server.send(changed?200:400, "text/plain", changed?"Config updated":"No changes");
}

void handleType(){
  if(typingActive){ server.send(409, "text/plain", "Busy: already typing"); return; }
  String body = readRequestBody();
  if(body.length()==0){ server.send(400, "text/plain", "Empty body"); return; }
  if(!bleKeyboard.isConnected()){ server.send(503, "text/plain", "BLE not connected"); return; }
  server.send(200, "text/plain", "Typing started (" + String(body.length()) + " chars)");
  delay(10);
  paused = false; // ensure not paused on new job
  typeLikeHuman(body);
}

void handleStop(){ typingActive = false; paused = false; server.send(200, "text/plain", "Stop requested"); }

// toggle pause/resume while typing
void handlePause(){
  if(!typingActive){ server.send(409, "text/plain", "Not typing"); return; }
  paused = !paused;
  server.send(200, "text/plain", paused?"Paused":"Resumed");
}

// Setup / Loop
void setup(){
  Serial.begin(115200);
  delay(100);
  randomSeed(esp_random());
  Serial.println("Starting BLE...");
  bleKeyboard.begin();
  delay(200);

  Serial.println("Starting Wi-Fi AP...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/type", HTTP_POST, handleType);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/pause", HTTP_GET, handlePause); // pause/resume endpoint

  server.begin();
  Serial.println("Server ready. Open http://" + WiFi.softAPIP().toString());
  Serial.println("Pair your target device to BLE name shown in console.");
}

void loop(){ server.handleClient(); }
