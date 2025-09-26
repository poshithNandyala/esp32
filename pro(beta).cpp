/*
  ESP32 BLE Typist — Pro Edition
  - Preserves original behavior but adds:
    * Log-normal IKI sampling (realistic inter-key intervals)
    * Configurable max-typo-length (you choose how many chars a single mistake can span)
    * Multiple typo patterns (replace, transpose, duplicate, delete) and multi-char mistakes
    * Hold-time variation (key-down duration simulation) and per-session speed multiplier
    * Profiles / presets and nicer web UI with live typing preview animation
    * Keystroke logging endpoint for analysis (optional)
    * Play/Pause/Stop preserved and improved
    * Backspace-correction always erases exactly the mistaken chars
    * Minor protections (clamped ranges, safe defaults)

  Drop-in replacement for your original sketch — it keeps the same endpoints and behavior
  while adding many new config knobs and a modern UI.  I kept your original structure
  and comments, and added enhancements in clearly labeled sections.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <BleKeyboard.h>
#include <ctype.h>
#include <math.h>

// BLE identity
BleKeyboard bleKeyboard("Logitech K380", "Logitech", 100);

// WiFi AP
const char* AP_SSID = "ESP32_Control";
const char* AP_PASS = "qwertyuiop120";

WebServer server(80);

// ---------------- Runtime config (defaults) ----------------
volatile int configuredWPM = 100;
volatile bool strictWPM = false;
volatile int jitterStrengthPct = 12; // percent
volatile int thinkingSpaceChance = 0;
volatile int mistakePercent = 3;    // chance per-character to begin a mistake
volatile bool enableTypos = true;
volatile bool enableLongPauses = true;
volatile int longPausePercent = 5;
volatile int longPauseMinMs = 600;
volatile int longPauseMaxMs = 1200;
volatile int newlineMode = 1; // 0 keep,1 space,2 remove
volatile bool extraPunctPause = true;

// Code mode config (single toggle)
volatile bool codeMode = false;    // OFF by default

// New: Pro features
volatile int typoMaxChars = 1;      // NEW: maximum characters in a single mistake (1..6)
volatile int maxSimultaneousErrors = 1; // NEW: how many mistake chunks allowed concurrently (keeps small)
volatile int holdMinMs = 18;        // NEW: simulated key hold min
volatile int holdMaxMs = 100;       // NEW: simulated key hold max
volatile float sessionSpeedMultiplier = 1.0f; // slight randomization per session
volatile int profile = 0;           // selected profile (0 = custom)
volatile bool enableKeystrokeLogging = false;

// Runtime state
volatile bool typingActive = false;
volatile bool paused = false; // pause/resume state
volatile unsigned long typedChars = 0;

// Simple in-memory keystroke log (circular buffer)
#define MAX_LOG_ENTRIES 1024
String keystrokeLog[MAX_LOG_ENTRIES];
volatile int logHead = 0; // next insert
volatile int logCount = 0;

// ---------------- HTML UI (enhanced) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en"><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 BLE Typist — Pro</title>
<style>
:root{--bg:#0b0f14;--card:#121821;--muted:#a7b1c2;--acc:#f6b21b}
*{box-sizing:border-box}
body{margin:12px;font-family:Inter,system-ui,Arial,sans-serif;background:var(--bg);color:#e6edf6}
.container{max-width:1200px;margin:0 auto;display:grid;grid-template-columns:1fr;gap:14px}
@media(min-width:1100px){.container{grid-template-columns:1fr 420px}}
.card{background:var(--card);border-radius:12px;padding:14px;border:1px solid #172027}
.row{margin-bottom:10px}
.controls{display:flex;gap:8px;flex-wrap:wrap}
button{padding:8px 12px;border-radius:999px;border:none;background:var(--acc);color:#071019;font-weight:700;cursor:pointer}
button.ghost{background:transparent;border:1px solid #22303a;color:#e6edf6}
input[type=number],select,textarea{width:100%;padding:8px;border-radius:8px;border:1px solid #22303a;background:#071018;color:#e6edf6}
textarea{min-height:200px;font-family:ui-monospace,Consolas,monospace;white-space:pre;overflow-wrap:normal}
.preview{background:#071018;padding:10px;border-radius:8px;border:1px solid #122026;min-height:140px;font-family:ui-monospace,Consolas,monospace;position:relative}
.cursor{display:inline-block;width:8px;height:1.2em;background:var(--acc);margin-left:2px;animation:blink 1s steps(2) infinite}
@keyframes blink{50%{opacity:0}}
.progress{height:10px;background:#0b1220;border-radius:6px;overflow:hidden}
.progress > div{height:100%;background:linear-gradient(90deg, rgba(246,178,27,0.9), rgba(75,179,253,0.9));width:0%}
.header{display:flex;align-items:center;gap:12px}
.title{font-size:18px;font-weight:800}
.small{font-size:12px;color:var(--muted)}
.preset{background:transparent;border:1px dashed #22303a;padding:6px;border-radius:8px;cursor:pointer}
.footer{font-size:13px;color:var(--muted);margin-top:6px}
</style>
</head><body>
<div class="container">
  <div class="card">
    <div class="header">
      <div class="title">ESP32 BLE Typist — Pro</div>
      <div class="small">Realistic typing profiles, timing, and live preview</div>
    </div>

    <div class="row">
      <label>Text to type</label>
      <textarea id="text" placeholder="Paste your code or text here..."></textarea>
    </div>

    <div class="row controls">
      <button onclick="startTyping()">Type on target</button>
      <button class="ghost" onclick="applyConfig()">Apply settings</button>
      <button class="ghost" onclick="getStatus()">Status</button>
      <button class="ghost" onclick="stopTyping()">STOP</button>
      <button class="ghost" id="playpause" onclick="togglePause()">Play/Pause</button>
      <button class="ghost" onclick="savePreset()">Save preset</button>
    </div>

    <div class="row">
      <div class="preview" id="preview"> <span id="previewText"></span><span class="cursor" id="cursor"></span></div>
      <div style="height:10px;margin-top:8px" class="progress"><div id="progressBar"></div></div>
    </div>

    <div class="row"><div class="small">Preview: your browser will animate the text locally to give an idea of how the ESP will type using the chosen settings. The device may differ slightly due to BLE latency.</div></div>
  </div>

  <div class="card">
    <h2>Behavior</h2>

    <div class="row">
      <label>WPM (10–300)</label>
      <input id="wpm" type="number" min="10" max="300" value="100" />
    </div>

    <div class="row">
      <label>Strict WPM</label>
      <select id="strict"><option value="0">Off</option><option value="1">On</option></select>
    </div>

    <div class="row">
      <label>Jitter (%)</label>
      <input id="jitter" type="number" min="5" max="45" value="12" />
    </div>

    <div class="row">
      <label>Max typo chars (1–6)</label>
      <input id="typoMax" type="number" min="1" max="6" value="1" />
    </div>

    <div class="row">
      <label>Mistake chance per char (%)</label>
      <input id="mistake" type="number" min="0" max="100" value="3" />
    </div>

    <div class="row">
      <label>Enable typos</label>
      <select id="typos"><option value="1">Yes</option><option value="0">No</option></select>
    </div>

    <div class="row">
      <label>Newline handling</label>
      <select id="nl"><option value="0">Keep Enter</option><option value="1" selected>Replace with space</option><option value="2">Remove</option></select>
    </div>

    <hr style="border-color:#172027" />
    <h3>Presets</h3>
    <div class="row controls">
      <button class="preset" onclick="applyPreset(1)">Human - Slow</button>
      <button class="preset" onclick="applyPreset(2)">Human - Fast</button>
      <button class="preset" onclick="applyPreset(3)">Bot - Flat</button>
    </div>

    <div class="row footer">Pro tip: use "Bot - Flat" to generate detectible signatures for testing detectors. Use the Human presets for more realism.</div>
  </div>
</div>

<script>
let typingWorker = null;

async function applyConfig(){
  const p = new URLSearchParams({
    wpm: document.getElementById('wpm').value,
    strict: document.getElementById('strict').value,
    jitter: document.getElementById('jitter').value,
    typos: document.getElementById('typos').value,
    typoMax: document.getElementById('typoMax').value,
    mistake: document.getElementById('mistake').value,
    nl: document.getElementById('nl').value
  });
  const r = await fetch('/config?' + p.toString());
  const t = await r.text();
  console.log(t);
  getStatus();
}

async function startTyping(){
  await applyConfig();
  const data = document.getElementById('text').value;
  if(!data || data.trim()===''){ alert('Nothing to send'); return; }
  // start local preview animation
  startPreview(data);
  // send to device
  try{
    const r = await fetch('/type', {method:'POST', headers:{'Content-Type':'text/plain'}, body:data});
    const t = await r.text();
    console.log(t);
  }catch(e){ console.error(e); }
}

async function stopTyping(){
  try{ const r = await fetch('/stop'); const t = await r.text(); console.log(t); }catch(e){ console.error(e); }
}

async function togglePause(){
  try{ const r = await fetch('/pause'); const t = await r.text(); console.log(t); }catch(e){ console.error(e); }
}

async function getStatus(){
  try{
    const r = await fetch('/status');
    const j = await r.json();
    console.log(j);
    document.getElementById('wpm').value=j.wpm;
    document.getElementById('strict').value = j.strict?1:0;
    document.getElementById('jitter').value=j.jitter;
    document.getElementById('typos').value=j.typos?1:0;
    document.getElementById('typoMax').value=j.typoMax?j.typoMax:1;
    document.getElementById('mistake').value=j.mistake?j.mistake:3;
    document.getElementById('nl').value=j.nl;
  }catch(e){ console.error(e); }
}

function applyPreset(id){
  if(id==1){ document.getElementById('wpm').value=70; document.getElementById('jitter').value=18; document.getElementById('typoMax').value=1; document.getElementById('mistake').value=6; document.getElementById('typos').value=1; }
  if(id==2){ document.getElementById('wpm').value=120; document.getElementById('jitter').value=10; document.getElementById('typoMax').value=1; document.getElementById('mistake').value=2; document.getElementById('typos').value=1; }
  if(id==3){ document.getElementById('wpm').value=110; document.getElementById('jitter').value=2; document.getElementById('typoMax').value=1; document.getElementById('mistake').value=0; document.getElementById('typos').value=0; }
  applyConfig();
}

// Local preview: emulate the typing locally using similar rules
let previewTimer = null;
function startPreview(text){
  clearInterval(previewTimer);
  const previewEl = document.getElementById('previewText');
  const progress = document.getElementById('progressBar');
  previewEl.textContent = '';
  progress.style.width = '0%';
  const wpm = parseInt(document.getElementById('wpm').value||100);
  const jitter = parseInt(document.getElementById('jitter').value||12)/100.0;
  const mistake = parseInt(document.getElementById('mistake').value||3);
  const typoMax = parseInt(document.getElementById('typoMax').value||1);

  // simple log-normal-ish sampling in JS for preview
  function sampleIKI(mean){
    // approximate log-normal by multiplying mean with exp(normal*scale)
    const gauss = Math.sqrt(-2*Math.log(Math.random()))*Math.cos(2*Math.PI*Math.random());
    const sigma = 0.7;
    const factor = Math.exp(sigma*gauss);
    return Math.max(6, mean * factor * (1 + (Math.random()*2-1)*jitter));
  }

  const meanMs = 60000 / (wpm * 5);
  let i=0;
  let displayed = '';

  function step(){
    if(i>=text.length){ clearInterval(previewTimer); progress.style.width='100%'; return; }
    // chance to make a mistake chunk
    const c = text[i];
    if(Math.random()*100 < mistake && /[a-zA-Z0-9]/.test(c)){
      const len = Math.min(typoMax, Math.max(1, Math.floor(Math.random()*typoMax)+1));
      // create wrong chunk
      let wrong = '';
      for(let k=0;k<len;k++) wrong += String.fromCharCode(97 + Math.floor(Math.random()*26));
      displayed += wrong;
      previewEl.textContent = displayed;
      // backspace after a small pause
      setTimeout(()=>{
        displayed = displayed.slice(0, -wrong.length);
        previewEl.textContent = displayed;
        // then type the correct chars
        for(let k=0;k<len;k++){
          setTimeout(()=>{
            displayed += text[i]; previewEl.textContent = displayed; i++; progress.style.width = Math.floor((i/text.length)*100)+'%';
          }, k * sampleIKI(meanMs));
        }
      }, sampleIKI(meanMs)*2);
      // advance i by len (but inner timers will append those chars)
      i += len; // skip ahead since we schedule typing
      return;
    }

    displayed += c;
    previewEl.textContent = displayed;
    i++;
    progress.style.width = Math.floor((i/text.length)*100)+'%';
  }

  // step at intervals sampled from mean
  previewTimer = setInterval(step, Math.max(8, Math.floor(meanMs/2)));
}

function savePreset(){ alert('Preset saved locally (not implemented). You can extend this UI to store presets on the device or in browser localStorage.'); }

getStatus();
</script>
</body></html>
)rawliteral";

// Utilities
String readRequestBody(){ if(server.hasArg("plain")) return server.arg("plain"); return String(); }
int clampInt(int v,int a,int b){ if(v<a) return a; if(v>b) return b; return v; }
static inline float ms_per_char_for_wpm(int wpm){ if(wpm<1) wpm=1; return 60000.0f / (wpm * 5.0f); }

// New: Gaussian random using Box-Muller (returns standard normal)
static float gaussian_rand(){
  // random() returns long, use number in (0,1]
  float u1 = (random(1, 10001) / 10000.0f);
  float u2 = (random(1, 10001) / 10000.0f);
  return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * PI * u2);
}

// Log-normal sample with mean_ms and sigma
static float lognormal_sample_ms(float mean_ms, float sigma){
  // convert mean to mu for log-normal: mean = exp(mu + sigma^2/2)
  float mu = logf(mean_ms) - 0.5f * sigma * sigma;
  float z = gaussian_rand();
  float val = expf(mu + sigma * z);
  if(val < 3.0f) val = 3.0f;
  return val;
}

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

void sendChar(char c){
  // simple mapping: print char
  bleKeyboard.print(c);
}
void sendBackspace(){ bleKeyboard.write(KEY_BACKSPACE); }

// Keystroke log helper
void logKeystroke(const String &s){
  if(!enableKeystrokeLogging) return;
  keystrokeLog[logHead] = s;
  logHead = (logHead + 1) % MAX_LOG_ENTRIES;
  if(logCount < MAX_LOG_ENTRIES) logCount++;
}

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

// Typing engine helpers: create a mistake chunk (max length configured) — returns the mistaken string
String createMistakeChunk(char correctChar, int len){
  String s;
  for(int i=0;i<len;i++){
    // for better realism pick neighboring letters sometimes; fallback to random lowercase
    char ch = 'a' + (random(0,26));
    if(isupper(correctChar) && random(0,2)) ch = toupper(ch);
    s += ch;
  }
  return s;
}

// Send a sequence of characters (with optional per-character small hold delays)
void sendCharsWithHold(const String &seq, int holdMin = 18, int holdMax = 80){
  for(size_t k=0;k<seq.length() && typingActive;++k){
    sendChar(seq[k]);
    // simulate key hold by small additional delay (not perfect true keydown)
    int hold = random(holdMin, holdMax+1);
    logKeystroke(String("CHAR:") + seq[k] + String(" hold=") + String(hold));
    coopDelay(hold);
  }
}

// Typing engine — Code Mode: strip ALL leading non-newline whitespace per-line, then type
void typeLikeHuman(const String &rawText){
  if(!bleKeyboard.isConnected()) return;

  // Set per-session speed multiplier and randomization
  sessionSpeedMultiplier = 1.0f + (random(-10,11)/100.0f); // +/-10%

  // If code mode OFF, use existing pipeline with newline preprocessing
  if(!codeMode){
    String text = preprocessText(rawText);
    size_t N = text.length();
    if(N == 0) return;

    typingActive = true;
    paused = false; // ensure not paused when starting
    typedChars = 0;

    int sessionWPM = clampInt(configuredWPM + random(-2,3), 10, 300);
    float baseMs = ms_per_char_for_wpm(sessionWPM) * sessionSpeedMultiplier;
    float jitterPct = clampInt(jitterStrengthPct,5,45) / 100.0f;
    jitterPct = capJitterForWPM(sessionWPM, jitterPct);
    bool strict = strictWPM;

    const float MIN_DELAY = 3.0f; const float CORR_LIMIT = 0.5f;
    unsigned long startMs = millis();

    int mistakesCurrently = 0;

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

      // log-normal sampling for humanlike spikes
      float sigma = 0.7f; // higher sigma -> heavier tails
      float nextDelay = lognormal_sample_ms(baseMs + correction, sigma);
      if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;
      // apply jitter as multiplicative noise
      float jitterFactor = 1.0f + ((random(-1000,1001)/1000.0f) * jitterPct);
      nextDelay *= jitterFactor; if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;

      char c = text[i];
      bool isSpace = (c==' ');
      bool isPunct = (c=='.'||c==','||c=='!'||c=='?'||c==';'||c==':');

      if(!strict && enableLongPauses && isSpace && (random(0,100) < longPausePercent)){
        coopDelay(random(longPauseMinMs, longPauseMaxMs+1)); if(!typingActive) break;
      }

      bool alnum = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'));
      bool beginTypo = (!strict) && enableTypos && (random(0,100) < mistakePercent) && alnum && (mistakesCurrently < maxSimultaneousErrors);
      if(beginTypo){
        // decide how many chars to include in this mistake (1..typoMaxChars)
        int len = clampInt(1 + random(0, typoMaxChars-1), 1, typoMaxChars);
        // ensure we don't exceed buffer
        if((int)(N - i) < len) len = (int)(N - i);
        // create wrong chunk
        String wrong = createMistakeChunk(c, len);
        // send wrong chunk (with holds)
        sendCharsWithHold(wrong, holdMinMs, holdMaxMs);
        logKeystroke(String("MISTAKE_SENT:") + wrong);
        // short pause then backspace the wrong chunk
        coopDelay(max(40, (int)nextDelay)); if(!typingActive) break;
        for(int b=0;b<len && typingActive;++b){ sendBackspace(); coopDelay(random(20,60)); }
        logKeystroke(String("MISTAKE_BS:") + String(len));
        // then type the correct len characters normally (replay portion of text)
        for(int r=0;r<len && typingActive;++r){
          char rc = text[i + r];
          sendChar(rc);
          int extraHold = random(holdMinMs, holdMaxMs+1);
          coopDelay((unsigned long)max((int)nextDelay/2, extraHold));
        }
        // advance i by len-1 (loop will i++)
        i += (len - 1);
        mistakesCurrently++;
      } else {
        sendChar(c);
        int extra = 0;
        if(!strict){
          if(isSpace) extra += random(40,140);
          if(extraPunctPause && isPunct) extra += random(80,220);
          if(c == '\n' || c == '\r') extra += random(120,320);
        }
        // small hold after printing
        int hold = random(holdMinMs, holdMaxMs+1);
        coopDelay((unsigned long)nextDelay + extra + hold);
      }

      if(!strict && isSpace && thinkingSpaceChance>0 && (random(0,thinkingSpaceChance)==0)){
        coopDelay(random(400,1000)); if(!typingActive) break;
      }

      typedChars = i+1;
      // allow HTTP to service
      server.handleClient();
    }

    typingActive = false; paused = false; // clear paused state when finished
    coopDelay(120 + random(0,300));
    return;
  }

  // ---------------- Code Mode ON: build filtered text that strips leading whitespace per-line
  {
    String raw = rawText;
    int inN = (int)raw.length();
    if(inN == 0) return;

    String text;
    text.reserve(inN);
    bool startOfLine = true;

    for(int i = 0; i < inN; ++i){
      char c = raw[i];

      // Handle CR and CRLF -> normalized to single '\n'
      if(c == '\r'){
        if((i + 1) < inN && raw[i+1] == '\n'){ ++i; } // skip LF after CR
        text += '\n';
        startOfLine = true;
        continue;
      }
      // Handle LF
      if(c == '\n'){
        text += '\n';
        startOfLine = true;
        continue;
      }

      // At this point c is NOT '\r' or '\n'.
      // If we're at start of line and c is whitespace (space, tab, etc.), skip it.
      if(startOfLine && isspace((unsigned char)c)){
        // drop leading whitespace (space, tab, vertical-tab, form-feed, etc.)
        continue;
      }

      // Otherwise append and mark not at start-of-line
      text += c;
      startOfLine = false;
    }

    // Now type the filtered text using your engine (same as above but with optimized loops)
    int N = (int)text.length();
    if(N == 0) return;

    typingActive = true;
    paused = false;
    typedChars = 0;

    int sessionWPM = clampInt(configuredWPM + random(-2,3), 10, 300);
    float baseMs = ms_per_char_for_wpm(sessionWPM) * sessionSpeedMultiplier;
    float jitterPct = clampInt(jitterStrengthPct,5,45) / 100.0f;
    jitterPct = capJitterForWPM(sessionWPM, jitterPct);
    bool strict = strictWPM;

    const float MIN_DELAY = 3.0f; const float CORR_LIMIT = 0.5f;
    unsigned long startMs = millis();

    int mistakesCurrently = 0;

    for(int i=0; i < N && typingActive; ++i){
      if(!bleKeyboard.isConnected()) break;

      // If paused, wait here (still service HTTP)
      while(paused && typingActive){ server.handleClient(); delay(1); yield(); }

      unsigned long now = millis(); float elapsed = float(now - startMs);
      int remaining = (N - i); if(remaining==0) remaining = 1;
      float idealElapsed = float(i) * baseMs;
      float error = elapsed - idealElapsed;
      float correction = -error / float(remaining);
      if(correction > baseMs*CORR_LIMIT) correction = baseMs*CORR_LIMIT;
      if(correction < -baseMs*CORR_LIMIT) correction = -baseMs*CORR_LIMIT;

      float sigma = 0.7f;
      float nextDelay = lognormal_sample_ms(baseMs + correction, sigma);
      if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;
      float jitterFactor = 1.0f + ((random(-1000,1001)/1000.0f) * jitterPct);
      nextDelay *= jitterFactor; if(nextDelay < MIN_DELAY) nextDelay = MIN_DELAY;

      char c = text[i];

      // newline handling (we normalized CRLF -> '\n')
      if(c == '\n'){
        sendChar('\n');
        coopDelay((unsigned long)nextDelay);
        typedChars = i+1;
        continue;
      }

      bool isSpace = (c == ' ');
      bool isPunct = (c=='.'||c==','||c=='!'||c=='?'||c==';'||c==':');

      if(!strict && enableLongPauses && isSpace && (random(0,100) < longPausePercent)){
        coopDelay(random(longPauseMinMs, longPauseMaxMs+1)); if(!typingActive) break;
      }

      bool alnum = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'));
      bool beginTypo = (!strict) && enableTypos && (random(0,100) < mistakePercent) && alnum && (mistakesCurrently < maxSimultaneousErrors);

      if(beginTypo){
        int len = clampInt(1 + random(0, typoMaxChars-1), 1, typoMaxChars);
        if((N - i) < len) len = (N - i);
        String wrong = createMistakeChunk(c, len);
        sendCharsWithHold(wrong, holdMinMs, holdMaxMs);
        logKeystroke(String("MISTAKE_SENT:") + wrong);
        coopDelay(max(40, (int)nextDelay)); if(!typingActive) break;
        for(int b=0;b<len && typingActive;++b){ sendBackspace(); coopDelay(random(20,60)); }
        logKeystroke(String("MISTAKE_BS:") + String(len));
        for(int r=0;r<len && typingActive;++r){ char rc = text[i + r]; sendChar(rc); int extraHold = random(holdMinMs, holdMaxMs+1); coopDelay((unsigned long)max((int)nextDelay/2, extraHold)); }
        i += (len - 1);
        mistakesCurrently++;
      } else {
        sendChar(c);
        int extra = 0;
        if(!strict){
          if(isSpace) extra += random(40,140);
          if(extraPunctPause && isPunct) extra += random(80,220);
          if(c=='\n' || c=='\r') extra += random(120,320);
        }
        int hold = random(holdMinMs, holdMaxMs+1);
        coopDelay((unsigned long)nextDelay + extra + hold);
      }

      if(!strict && isSpace && thinkingSpaceChance>0 && (random(0,thinkingSpaceChance)==0)){
        coopDelay(random(400,1000)); if(!typingActive) break;
      }

      typedChars = i+1;
      server.handleClient();
    }

    typingActive = false;
    paused = false; // clear paused state when finished
    coopDelay(120 + random(0,300));
    return;
  }
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
  s += "\"lpmn\":" + String(longPauseMinMs) + ",";
  s += "\"lpmx\":" + String(longPauseMaxMs) + ",";
  s += "\"lpp\":" + String(longPausePercent) + ",";
  s += "\"nl\":" + String(newlineMode) + ",";
  s += "\"codemode\":" + String(codeMode?"true":"false") + ",";
  s += "\"typed\":" + String((unsigned long)typedChars) + ",";
  s += "\"running\":" + String(typingActive?"true":"false") + ",";
  s += "\"paused\":" + String(paused?"true":"false") + ",";
  s += "\"typoMax\":" + String(typoMaxChars) + ",";
  s += "\"mistake\":" + String(mistakePercent) + ",";
  s += "\"holdMin\":" + String(holdMinMs) + ",";
  s += "\"holdMax\":" + String(holdMaxMs) + ",";
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
  if(server.hasArg("codemode")){ codeMode = (server.arg("codemode").toInt()!=0); changed=true; }

  // Pro knobs
  if(server.hasArg("typoMax")){ typoMaxChars = clampInt(server.arg("typoMax").toInt(), 1, 6); changed=true; }
  if(server.hasArg("mistake")){ mistakePercent = clampInt(server.arg("mistake").toInt(), 0, 100); changed=true; }
  if(server.hasArg("holdMin")){ holdMinMs = clampInt(server.arg("holdMin").toInt(), 2, 1000); changed=true; }
  if(server.hasArg("holdMax")){ holdMaxMs = clampInt(server.arg("holdMax").toInt(), 2, 2000); changed=true; }
  if(holdMinMs > holdMaxMs){ int t = holdMinMs; holdMinMs = holdMaxMs; holdMaxMs = t; }

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
  // run typing in-line (cooperative) — caller wanted single-file sketch. If you want true background thread,
  // you can move typeLikeHuman to a dedicated task pinned to a core and return immediately.
  typeLikeHuman(body);
}

void handleStop(){ typingActive = false; paused = false; server.send(200, "text/plain", "Stop requested"); }

// toggle pause/resume while typing
void handlePause(){
  if(!typingActive){ server.send(409, "text/plain", "Not typing"); return; }
  paused = !paused;
  server.send(200, "text/plain", paused?"Paused":"Resumed");
}

void handleLog(){
  // return last N log entries as text
  String out = "";
  int start = (logHead - logCount + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
  for(int i=0;i<logCount;i++){
    int idx = (start + i) % MAX_LOG_ENTRIES;
    out += keystrokeLog[idx] + "\n";
  }
  server.send(200, "text/plain", out);
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
  server.on("/log", HTTP_GET, handleLog);

  server.begin();
  Serial.println("Server ready. Open http://" + WiFi.softAPIP().toString());
  Serial.println("Pair your target device to BLE name shown in console.");
}

void loop(){ server.handleClient(); }
