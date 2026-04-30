/**
 * TriggerLab ESP8266 Bridge Firmware
 *
 * Acts as a transparent TCP ↔ UART bridge between the Android app
 * and the STM32F722 mainboard.
 *
 * Fixes vs original:
 *  1. VLA stack buffers replaced with static 256-byte buffers (crash fix)
 *  2. processString() off-by-one null terminator overflow fixed
 *  3. delay(5) in inner loop replaced with yield()/delay(1) (latency fix)
 *  4. delay(100) in outer loop reduced to delay(10) (reconnect fix)
 *  5. SignalReadyPin release uses explicit pin, not LED_BUILTIN (ambiguity fix)
 *  6. UART RX buffer increased 128 → 512 bytes (overflow fix)
 *  7. EEPROM size increased 32 → 64 bytes (margin fix)
 *  8. WiFi channel fixed to ch6 instead of random (stability fix)
 *  9. Inner loop drains ALL available UART data per iteration (throughput fix)
 * 10. UART baud rate increased 115200 → 921600 (8× speed improvement)
 * 11. RSSI reporting added — sent to Android every 5 seconds
 * 12. Factory reset command (%%) now also reboots ESP cleanly
 *
 * Packet protocol (matches Remote.h and Cmd.kt):
 *  Android → STM32:  [<][TYPE][LO][HI][CS][>]  6 bytes
 *  STM32 → Android:  [<][TYPE][LO][HI][CS][>]  6 bytes
 *  GVI handshake:    "GVI!\n"                    5 bytes (on connect)
 *
 * Special ESP configuration commands (intercepted, NOT forwarded to STM32):
 *  !! + IP string   → update stored IP address
 *  ## + SSID string → update stored SSID
 *  $$ + PASS string → update stored password
 *  %% (any)         → factory reset + reboot
 */

#include <EEPROM.h>
#include <ESP8266WiFi.h>

// ── EEPROM layout ─────────────────────────────────────────────────────────
#define EEPROM_SIZE     64   // bytes (was 32 — added margin)
#define IP_Start        0
#define IP_Len          4
#define SSID_Start      IP_Len
#define SSID_Len        11
#define Pass_Start      (SSID_Start + SSID_Len)
#define Pass_Len        17   // min 8, max 16 + null terminator

// ── Network config ────────────────────────────────────────────────────────
#define SrvPort         80
#define MAX_SRV_CLIENTS 1
#define WIFI_CHANNEL    6    // Fixed ch6 (non-overlapping: 1, 6, 11)
                             // was random(1,13) — caused variable interference

// ── Timing ────────────────────────────────────────────────────────────────
#define RECONNECT_DELAY_MS  10   // outer loop idle delay (was 100ms)
#define RSSI_INTERVAL_MS    5000 // how often to send RSSI to Android

// ── UART ──────────────────────────────────────────────────────────────────
#define UART_BAUD       921600   // was 115200 — 8× speed improvement
                                 // STM32 USART3 must also be set to 921600

// ── Buffer sizes ─────────────────────────────────────────────────────────
#define BUFF_SIZE       256  // static buffer — no VLA stack allocation

// ── Hardware pins ─────────────────────────────────────────────────────────
#define SIGNAL_READY_PIN  2  // HIGH = STM32 on hold, LOW = STM32 running
                             // On ESP-01 and NodeMCU: GPIO2 = LED_BUILTIN

// ── Packet constants (must match Remote.h) ────────────────────────────────
#define PKT_START       0x3C  // '<'
#define PKT_END         0x3E  // '>'
#define SEND_RSSI       0x51  // 'Q' — new: RSSI packet type

// ── Default credentials ───────────────────────────────────────────────────
#define StrConnOK       "GVI!\n"
#define StrDefaultSSID  "RemoteSen"
#define StrDefaultPASS  "01234567"
#define ZERO_CHAR       48

// ── Static buffers — no VLA, no stack overflow ────────────────────────────
static uint8_t rx_buff[BUFF_SIZE];
static uint8_t tx_buff[BUFF_SIZE];

// ── State ─────────────────────────────────────────────────────────────────
uint8_t  myIP[4]       = {10, 0, 0, 1};
const uint8_t mySubNet[4] = {255, 255, 255, 0};
bool     softAP_ready  = false;
String   mySSID;
String   myPASSWORD;
uint32_t lastRssiTime  = 0;

WiFiServer server(SrvPort);
WiFiClient serverClient;

// ─────────────────────────────────────────────────────────────────────────
// EEPROM helpers
// ─────────────────────────────────────────────────────────────────────────

void get_IP() {
    for (uint8_t i = 0; i < IP_Len; i++) {
        myIP[i] = EEPROM.read(IP_Start + i);
        // Sanity check — unwritten EEPROM reads 0xFF
        if (myIP[i] == 0xFF) myIP[i] = (i == 0) ? 10 : (i == 3) ? 1 : 0;
    }
}

void setIP() {
    for (uint8_t i = 0; i < IP_Len; i++) {
        EEPROM.write(IP_Start + i, myIP[i]);
    }
    EEPROM.commit();
}

String get_MemStr(uint8_t startPos, uint8_t maxLen) {
    char data[maxLen + 1];
    uint8_t i;
    for (i = 0; i < maxLen; i++) {
        uint8_t c = EEPROM.read(startPos + i);
        if (c == '\0' || c == 0xFF) break;
        data[i] = (char)c;
    }
    data[i] = '\0';
    return String(data);
}

void set_MemStr(uint8_t type, String myData) {
    uint8_t pos   = type ? Pass_Start : SSID_Start;
    uint8_t limit = type ? Pass_Len   : SSID_Len;
    uint8_t len   = (uint8_t)min((int)myData.length(), (int)(limit - 1));
    uint8_t i;
    for (i = 0; i < len; i++) {
        EEPROM.write(pos + i, (uint8_t)myData[i]);
    }
    EEPROM.write(pos + i, '\0');
    EEPROM.commit();
}

void resetData() {
    myIP[0] = 10; myIP[1] = 0; myIP[2] = 0; myIP[3] = 1;
    setIP();
    set_MemStr(0, StrDefaultSSID);
    set_MemStr(1, StrDefaultPASS);
}

// ─────────────────────────────────────────────────────────────────────────
// IP parsing
// ─────────────────────────────────────────────────────────────────────────

void arrayToIP(uint8_t arr[], uint8_t len, uint8_t pos) {
    uint8_t val = 0;
    for (uint8_t i = 0; i < len; i++) {
        val = val * 10 + (arr[i] - ZERO_CHAR);
    }
    myIP[pos] = val;
}

void processIP(uint8_t* data, uint8_t len) {
    uint8_t j = 0;
    uint8_t idx = 0;
    uint8_t tmpArr[3];
    // Skip the leading '!!' marker (bytes 0 and 1)
    for (uint8_t i = 2; i < len; i++) {
        uint8_t c = data[i];
        if (c == '.' || i == len - 1) {
            if (i == len - 1 && c != '.') {
                tmpArr[idx++] = c;
            }
            arrayToIP(tmpArr, idx, j++);
            idx = 0;
        } else {
            if (idx < 3) tmpArr[idx++] = c;
        }
    }
    setIP();
}

// ─────────────────────────────────────────────────────────────────────────
// String command processing (## = SSID, $$ = password)
// FIX: buffer now sized len-1 to accommodate null terminator safely
// ─────────────────────────────────────────────────────────────────────────

void processString(uint8_t* data, uint8_t len, uint8_t id) {
    if (len <= 2) return;
    uint8_t dataLen = len - 2;
    // FIX: allocate dataLen + 1 so null terminator fits within bounds
    char myStr[dataLen + 1];
    for (uint8_t i = 0; i < dataLen; i++) {
        myStr[i] = (char)data[i + 2];
    }
    myStr[dataLen] = '\0';
    set_MemStr(id, String(myStr));
}

// ─────────────────────────────────────────────────────────────────────────
// RSSI packet builder
// Sends signal strength to Android as a standard 6-byte packet
// ─────────────────────────────────────────────────────────────────────────

void sendRSSI() {
    if (!serverClient || !serverClient.connected()) return;

    // RSSI is negative dBm (-30 to -100).
    // Encode as (rssi + 128) to make it an unsigned byte (28 to 98 range).
    int8_t  rssi    = (int8_t)WiFi.RSSI();
    uint8_t encoded = (uint8_t)(rssi + 128);
    uint8_t cs      = (uint8_t)(SEND_RSSI + encoded);

    uint8_t pkt[6] = {
        PKT_START,
        SEND_RSSI,
        encoded,
        0,           // hi byte unused
        cs,
        PKT_END
    };
    serverClient.write(pkt, 6);
}

// ─────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────

void setup() {
    // Hold STM32 in reset until WiFi is ready
    pinMode(SIGNAL_READY_PIN, OUTPUT);
    digitalWrite(SIGNAL_READY_PIN, HIGH);

    // Increase UART RX buffer BEFORE Serial.begin()
    // FIX: was 128 — too small when STM sends multiple packets quickly
    Serial.setRxBufferSize(512);
    Serial.begin(UART_BAUD);

    EEPROM.begin(EEPROM_SIZE);

    // Uncomment ONCE to write defaults to blank EEPROM:
    // resetData();

    get_IP();
    mySSID     = get_MemStr(SSID_Start, SSID_Len);
    myPASSWORD = get_MemStr(Pass_Start, Pass_Len);

    // Validate SSID and password (EEPROM might be blank on first run)
    if (mySSID.length() < 1)     mySSID     = StrDefaultSSID;
    if (myPASSWORD.length() < 8) myPASSWORD = StrDefaultPASS;

    WiFi.mode(WIFI_AP);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);  // disable sleep for minimum latency

    // FIX: fixed channel 6 instead of random(1,13)
    // Channels 1, 6, 11 are non-overlapping — ch6 is commonly least congested
    while (!softAP_ready) {
        softAP_ready = WiFi.softAP(
            mySSID, myPASSWORD,
            WIFI_CHANNEL,
            false,               // not hidden
            MAX_SRV_CLIENTS      // max 1 client
        );
        if (!softAP_ready) delay(100);
    }

    IPAddress ip(myIP[0], myIP[1], myIP[2], myIP[3]);
    IPAddress gw(myIP[0], myIP[1], myIP[2], 1);
    IPAddress nm(mySubNet[0], mySubNet[1], mySubNet[2], mySubNet[3]);
    WiFi.softAPConfig(ip, gw, nm);

    server.begin();
    server.setNoDelay(true);

    // FIX: explicitly release STM32 using the correct pin
    // (was LED_BUILTIN which may differ from SIGNAL_READY_PIN on some boards)
    digitalWrite(SIGNAL_READY_PIN, LOW);
}

// ─────────────────────────────────────────────────────────────────────────
// Main loop
// ─────────────────────────────────────────────────────────────────────────

void loop() {
    serverClient = server.available();

    if (serverClient) {
        if (serverClient.connected()) {
            serverClient.setNoDelay(true);
            // Send GVI handshake so Android knows connection is live
            serverClient.write(StrConnOK);
        }

        while (serverClient.connected()) {

            bool activity = false;

            // ── TCP → UART (Android → STM32) ──────────────────────────────
            size_t rxAvail = serverClient.available();
            if (rxAvail > 0) {
                activity = true;
                size_t toRead = min(rxAvail, (size_t)BUFF_SIZE);
                serverClient.readBytes(rx_buff, toRead);

                // Intercept ESP configuration commands
                if (toRead >= 2 && rx_buff[0] == '!' && rx_buff[1] == '!') {
                    processIP(rx_buff, (uint8_t)toRead);

                } else if (toRead >= 2 && rx_buff[0] == '#' && rx_buff[1] == '#') {
                    processString(rx_buff, (uint8_t)toRead, 0);  // SSID

                } else if (toRead >= 2 && rx_buff[0] == '$' && rx_buff[1] == '$') {
                    processString(rx_buff, (uint8_t)toRead, 1);  // Password

                } else if (toRead >= 2 && rx_buff[0] == '%' && rx_buff[1] == '%') {
                    // Factory reset — restore defaults then reboot cleanly
                    resetData();
                    delay(200);
                    ESP.restart();

                } else {
                    // Forward directly to STM32
                    Serial.write(rx_buff, toRead);
                }
            }

            // ── UART → TCP (STM32 → Android) ──────────────────────────────
            // FIX: drain ALL available bytes in one pass (was single read)
            // This ensures rapid bursts from STM32 (sensor + battery packets
            // arriving together) are forwarded immediately, not split across
            // multiple loop iterations each adding delay(5).
            while (Serial.available() > 0) {
                activity = true;
                size_t txAvail = (size_t)Serial.available();
                size_t toSend  = min(txAvail, (size_t)BUFF_SIZE);
                Serial.readBytes(tx_buff, toSend);
                serverClient.write(tx_buff, toSend);
            }

            // ── Periodic RSSI report ───────────────────────────────────────
            if (millis() - lastRssiTime >= RSSI_INTERVAL_MS) {
                sendRSSI();
                lastRssiTime = millis();
            }

            // ── Idle management ────────────────────────────────────────────
            // FIX: only delay when truly idle — no mandatory 5ms per iteration
            // yield() feeds the ESP8266 watchdog and WiFi stack without blocking
            if (!activity) {
                delay(1);
            } else {
                yield();
            }
        }

        // Client disconnected — clean up
        serverClient.stop();
    }

    // FIX: outer loop delay reduced 100ms → 10ms for faster reconnect
    delay(RECONNECT_DELAY_MS);
}
