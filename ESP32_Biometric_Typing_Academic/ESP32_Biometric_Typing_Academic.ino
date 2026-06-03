/**
 * ESP32 BIOMETRIC TYPING ENGINE (ACADEMIC EDITION)
 * * STATISTICAL UPGRADE:
 * - Replaced standard random() with Box-Muller Transform.
 * - Implemented Log-Normal Distribution for keystroke latencies (Reaction Time Model).
 * - Implemented Gaussian Distribution for cognitive pauses.
 * * LOGIC PRESERVED: 
 * - Smart Brace '}' Skipping Loop.
 * - "Down Arrow + Enter" Navigation.
 * - Pause/Resume & Live Tuning.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BleKeyboard.h>
#include <ctype.h>
#include <math.h> 

// ===================================================================================
//  DEVICE SETUP
// ===================================================================================
BleKeyboard bleKeyboard("Human Biometric HID", "ESP32", 100);

const char* AP_SSID = "ESP32_Biometric_Lab";
const char* AP_PASS = "research123";

WebServer server(80);

// ===================================================================================
//  CONFIGURATION
// ===================================================================================

// --- Motor Control ---
volatile int targetWPM = 80;            
volatile int burstVariance = 30;        
volatile int jitterStrength = 15;       

// --- Error Dynamics ---
volatile int mistakeProb = 2;           
volatile int reactionLagChance = 50;    

// --- Cognitive Biometrics ---
volatile int shiftDwellMs = 120;        
volatile int thinkingPauseProb = 20;    
volatile int zoneOutProb = 1;           

// --- Logic ---
volatile bool codeMode = true;          
volatile int newlineMode = 0;           

// ===================================================================================
//  RUNTIME VARIABLES
// ===================================================================================
volatile bool isTyping = false;
volatile bool isPaused = false;
volatile bool stopSignal = false;
volatile unsigned long charsTyped = 0;
volatile int instantaneousWPM = 0;

// ===================================================================================
//  WEB INTERFACE
// ===================================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
    <meta charset="utf-8"/>
    <meta name="viewport" content="width=device-width,initial-scale=1"/>
    <title>Biometric Typing Research</title>
    <style>
        :root { --bg:#111; --card:#1a1a1a; --text:#ddd; --acc:#00d4ff; --brd:#333; }
        body { font-family: monospace; background: var(--bg); color: var(--text); padding: 20px; max-width: 900px; margin: 0 auto; }
        .card { background: var(--card); border: 1px solid var(--brd); padding: 20px; margin-bottom: 20px; border-radius: 4px; }
        h2 { border-bottom: 1px solid var(--acc); color: var(--acc); margin-top: 0; }
        .stat { color: #fff; font-weight: bold; }
        textarea { width: 100%; height: 200px; background: #000; border: 1px solid #444; color: #0f0; padding: 10px; }
        input[type=range] { width: 100%; }
        label { display: block; margin-top: 10px; color: #888; }
        .row { display: flex; gap: 20px; } .col { flex: 1; }
        button { width: 100%; padding: 10px; margin-top: 10px; background: var(--acc); border: none; font-weight: bold; cursor: pointer; }
        button.stop { background: #ff4444; color: white; }
        button.pause { background: #ffbb00; color: black; }
    </style>
</head>
<body>
    <div class="card" style="text-align:center">
        STATUS: <span id="st" class="stat">READY</span> | WPM: <span id="wpm" class="stat">0</span>
    </div>
    <div class="card">
        <h2>Input Source</h2>
        <textarea id="txt"></textarea>
        <div class="row">
            <button onclick="start()">INITIATE</button>
            <button class="pause" onclick="pause()">PAUSE/RESUME</button>
            <button class="stop" onclick="stop()">ABORT</button>
        </div>
    </div>
    <div class="card">
        <h2>Biometric Parameters</h2>
        <div class="row">
            <div class="col"><label>Gaussian Mean (WPM): <span id="v_wpm">80</span></label><input type="range" min="40" max="150" value="80" oninput="set('wpm',this.value)"></div>
            <div class="col"><label>Mistake Rate: <span id="v_mis">2</span>%</label><input type="range" min="0" max="10" value="2" oninput="set('mis',this.value)"></div>
        </div>
        <div class="row">
            <div class="col"><label>Flow Variance: <span id="v_var">30</span>%</label><input type="range" min="0" max="100" value="30" oninput="set('var',this.value)"></div>
            <div class="col"><label>Zone Out Prob: <span id="v_zone">1</span>%</label><input type="range" min="0" max="5" value="1" oninput="set('zone',this.value)"></div>
        </div>
    </div>
    <script>
        function set(k, v) { document.getElementById('v_' + k).innerText = v; fetch('/live?k=' + k + '&v=' + v); }
        function start() { fetch('/type', { method: 'POST', body: document.getElementById('txt').value }); }
        function stop() { fetch('/stop'); }
        function pause() { fetch('/pause'); }
        setInterval(async () => {
            try {
                const d = await (await fetch('/status')).json();
                document.getElementById('st').innerText = d.state;
                document.getElementById('wpm').innerText = d.act_wpm;
            } catch(e) {}
        }, 500);
    </script>
</body>
</html>
)rawliteral";

// ===================================================================================
//  STATISTICAL MATH HELPERS (Box-Muller & Log-Normal)
// ===================================================================================

int clamp(int v, int mn, int mx) { return (v < mn) ? mn : (v > mx ? mx : v); }

/**
 * Generates a standard Gaussian (Normal) random number.
 * Uses Box-Muller Transform to convert Uniform Random to Normal.
 * @param mean The target average value.
 * @param stdDev The spread (standard deviation).
 */
int gaussianRandom(float mean, float stdDev) {
    if (stdDev <= 0) return (int)mean;
    
    // Box-Muller Transform
    // Generate two uniform random numbers between 0 and 1
    float u1 = random(1, 10000) / 10000.0; 
    float u2 = random(1, 10000) / 10000.0;
    
    // Convert to Standard Normal (Z-Score)
    float z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);
    
    // Scale to our target mean and deviation
    return max(10, (int)(mean + (z0 * stdDev)));
}

/**
 * Generates a Log-Normal distributed random number.
 * Human reaction times are strictly Log-Normal (skewed, non-negative).
 * @param targetMean The desired average delay in milliseconds.
 * @param targetStdDev The desired spread in milliseconds.
 */
int logNormalDelay(float targetMean, float targetStdDev) {
    if (targetStdDev <= 0) return (int)targetMean;

    // 1. Convert linear mean/stdDev to Log-Space parameters (Mu and Sigma)
    // Formula: mu = ln(m^2 / sqrt(v + m^2))
    float variance = targetStdDev * targetStdDev;
    float mu = log((targetMean * targetMean) / sqrt(variance + (targetMean * targetMean)));
    float sigma = sqrt(log(1.0 + (variance / (targetMean * targetMean))));

    // 2. Generate a standard normal variable using Box-Muller
    float u1 = random(1, 10000) / 10000.0;
    float u2 = random(1, 10000) / 10000.0;
    float z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);

    // 3. Apply to Log-Normal formula: exp(mu + z * sigma)
    float val = exp(mu + (z0 * sigma));

    return max(10, (int)val);
}

bool isShiftChar(char c) {
    if (isupper(c)) return true;
    if (strchr("!@#$%^&*()_+{}|:\"<>?", c)) return true;
    return false;
}

void coopDelay(unsigned long ms) {
    unsigned long s = millis();
    while (isTyping && (millis() - s < ms)) {
        server.handleClient();
        if (stopSignal) { isTyping = false; break; }
        while (isPaused && !stopSignal) { 
            server.handleClient(); 
            delay(50); 
        }
        delay(1);
    }
}

// ===================================================================================
//  BIOMETRIC ENGINE
// ===================================================================================
void typeHuman(String raw) {
    if (!bleKeyboard.isConnected()) return;

    isTyping = true; stopSignal = false; isPaused = false; charsTyped = 0;
    
    int len = raw.length();
    int consecutiveMistakes = 0;
    bool startOfLine = true;
    
    // Flow State Variables
    int charsUntilStateChange = 0;
    float currentFlowFactor = 1.0; 

    for (int i = 0; i < len; i++) {
        if (!isTyping || stopSignal) break;

        char c = raw[i];

        // --- 1. CODE MODE: Indent Strip ---
        if (codeMode && startOfLine) { if (c == ' ' || c == '\t') continue; }
        if (c == '\n') startOfLine = true; else if (c != '\r') startOfLine = false;

        // --- 2. MULTI-BRACE LOOP (Your Logic) ---
        if (codeMode && c == '\n') {
            int peek = i + 1;
            int braces = 0;
            while (true) {
                while (peek < len && (raw[peek] == ' ' || raw[peek] == '\t')) peek++;
                if (peek < len && raw[peek] == '}') {
                    bleKeyboard.write(KEY_DOWN_ARROW);
                    // Use Log-Normal for movement delay (more human)
                    coopDelay(logNormalDelay(500, 100)); 
                    braces++;
                    peek++;
                    if (peek < len && raw[peek] == '\n') peek++;
                    continue;
                }
                break;
            }
            if (braces > 0) {
                bleKeyboard.write(KEY_RETURN);
                i = peek - 1; 
                startOfLine = true;
                continue; 
            }
        }

        // --- 3. NEWLINE HANDLING ---
        if (c == '\n') { if (newlineMode == 1) c = ' '; else if (newlineMode == 2) continue; }
        if (c == '\r') continue;

        // --- 4. STATISTICAL TIMING (Log-Normal) ---
        float baseMs = 12000.0 / targetWPM; // Average ms per char

        // Flow State Logic
        if (charsUntilStateChange <= 0) {
            charsUntilStateChange = random(10, 40);
            int state = random(0, 100);
            if (state < 30) currentFlowFactor = 0.8; 
            else if (state > 90) currentFlowFactor = 1.4;
            else currentFlowFactor = 1.0;
        }
        charsUntilStateChange--;

        // Shift Dwell
        int extraDwell = 0;
        if (isShiftChar(c)) extraDwell = shiftDwellMs;

        // Calculate Target Mean and Deviation for this keystroke
        float meanDelay = (baseMs * currentFlowFactor) + extraDwell;
        float stdDev = meanDelay * (jitterStrength / 100.0);
        
        // **KEY CHANGE: Use Log-Normal Distribution for Keystroke Latency**
        // This generates the biological "Bell Curve" (skewed) you requested.
        int finalDelay = logNormalDelay(meanDelay, stdDev);

        // --- 5. ERROR SIMULATION ---
        bool typo = false;
        if (isalnum(c) && random(0, 100) < mistakeProb) {
           if (consecutiveMistakes < 1) typo = true;
        }

        if (typo) {
            consecutiveMistakes++;
            bleKeyboard.print((char)(c + 1)); 
            
            // Reaction Lag (Log-Normal)
            bool overshoot = (i+1 < len && raw[i+1] != '\n' && random(0,100) < reactionLagChance);
            if (overshoot) {
                coopDelay(finalDelay * 0.7); 
                bleKeyboard.print(raw[i+1]);
                coopDelay(logNormalDelay(400, 100)); // Realization
                bleKeyboard.write(KEY_BACKSPACE);
                coopDelay(120);
                bleKeyboard.write(KEY_BACKSPACE);
            } else {
                coopDelay(logNormalDelay(350, 80)); 
                bleKeyboard.write(KEY_BACKSPACE);
            }
            
            coopDelay(logNormalDelay(150, 40));
            bleKeyboard.print(c); 
            
        } else {
            consecutiveMistakes = 0;
            bleKeyboard.print(c);
        }
        
        // --- 6. COGNITIVE PAUSES ---
        int thinkWait = 0;
        
        // Zone Out (Use Gaussian for longer pauses)
        if (random(0, 1000) < (zoneOutProb * 10)) {
            thinkWait = gaussianRandom(2500, 500);
        } 
        // Logic Pause
        else if (random(0,100) < thinkingPauseProb) {
            if (c == ';' || c == '{' || c == ')') thinkWait = gaussianRandom(1200, 300);
        }
        
        // --- 7. EXECUTE ---
        unsigned long totalDelay = (unsigned long)finalDelay + thinkWait;
        if(totalDelay < 5) totalDelay = 5;
        instantaneousWPM = (int)(12000.0 / totalDelay);
        charsTyped++;
        coopDelay(totalDelay);
    }
    isTyping = false; instantaneousWPM = 0;
}

// ===================================================================================
//  HANDLERS
// ===================================================================================
void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }
void handleLive() {
    if (server.hasArg("k") && server.hasArg("v")) {
        String k = server.arg("k"); int v = server.arg("v").toInt();
        if (k == "wpm") targetWPM = clamp(v, 10, 300);
        if (k == "mis") mistakeProb = clamp(v, 0, 10);
        if (k == "var") burstVariance = clamp(v, 0, 100);
        if (k == "shf") shiftDwellMs = clamp(v, 0, 500);
        if (k == "zone") zoneOutProb = clamp(v, 0, 5);
    }
    server.send(200, "text/plain", "OK");
}
void handleType() {
    if (isTyping) return server.send(400, "text/plain", "Busy");
    String body = server.arg("plain");
    server.send(200, "text/plain", "OK");
    typeHuman(body);
}
void handleStatus() {
    String json = "{";
    json += "\"state\":\"" + String(isPaused ? "PAUSED" : (isTyping ? "TYPING" : "READY")) + "\",";
    json += "\"act_wpm\":" + String(instantaneousWPM) + ",";
    json += "\"conf_wpm\":" + String(targetWPM) + ",";
    json += "\"typed\":" + String(charsTyped);
    json += "}";
    server.send(200, "application/json", json);
}

void setup() {
    Serial.begin(115200);
    bleKeyboard.begin();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/live", HTTP_GET, handleLive);
    server.on("/type", HTTP_POST, handleType);
    server.on("/stop", HTTP_GET, [](){ stopSignal=true; server.send(200); });
    server.on("/pause", HTTP_GET, [](){ isPaused=!isPaused; server.send(200); });
    server.on("/status", HTTP_GET, handleStatus);
    server.begin();
}

void loop() { server.handleClient(); }
