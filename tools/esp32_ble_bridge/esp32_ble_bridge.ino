/**
 * esp32_ble_bridge.ino
 *
 * Flipper Zero ↔ ESP32 BLE bridge firmware for the ESL Tool project.
 *
 * This sketch runs on any ESP32 module with BLE support (NOT ESP32-S2 or
 * ESP8266 — those lack BLE hardware). Recommended modules:
 *   - Seeed XIAO ESP32C3  ($5)  — GPIO20=RX, GPIO21=TX
 *   - ESP32-WROOM-32D     ($10) — GPIO16=RX, GPIO17=TX
 *   - Mayhem v2 ESP32-CAM ($95) — auto-connected, same pins as WROOM-32D
 *
 * Wiring (ESP32-WROOM-32D / generic DevKit):
 *   Flipper GPIO TX (pin 13)  ──→ ESP32 pin labeled "25" (our RX)
 *   Flipper GPIO RX (pin 14)  ──→ ESP32 pin labeled "26" (our TX)
 *   Flipper GND     (pin 18)  ──→ ESP32 GND
 *   Flipper 3.3V    (pin 9)   ──→ ESP32 3.3V (⚠ NOT 5V!)
 *
 *   ⚠ Do NOT use the pins labeled "TX" and "RX" near the USB connector —
 *   those are GPIO1/GPIO3 (the USB debug UART) and will not work.
 *   Use the pins labeled "25" and "26" by number on the header.
 *
 * Serial protocol (115200 baud, newline-terminated UTF-8):
 *
 * Flipper → ESP32:
 *   SCAN\n
 *   ATC_PRICE <mac> <price_str>\n
 *   ATC_PRICE <mac> <price_str> <label_str>\n
 *   ATC_CLEAR <mac>\n
 *   VUSION_PROVISION <mac>\n
 *   VUSION_DISPLAY <mac> <slot_0_to_15>\n
 *   VUSION_PING <mac>\n
 *   VUSION_RESET <mac>\n
 *
 * ESP32 → Flipper:
 *   DEVICE <name> <mac> <rssi> ATC|VUSION\n
 *   DONE\n
 *   PROGRESS <0-100>\n
 *   OK <message>\n
 *   ERROR <message>\n
 *
 * Dependencies (install via Arduino IDE Library Manager):
 *   - NimBLE-Arduino  (h2zero) — BLE Central + bonding
 *   - Adafruit GFX Library    — price text rendering
 *   - Adafruit BusIO          — dependency of GFX
 *
 * Board support:
 *   For XIAO ESP32C3: install "esp32 by Espressif" board package,
 *   select "XIAO_ESP32C3" in Tools → Board.
 *   For WROOM-32D: select "ESP32 Dev Module".
 */

#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEClient.h>
#include <NimBLEAdvertisedDevice.h>
#include <string.h>
#include <stdio.h>

// ── Pin configuration ─────────────────────────────────────────────────────────

// ESP32-WROOM-32D / generic ESP32 DevKit:
//   Use GPIO25 (RX from Flipper) and GPIO26 (TX to Flipper).
//   These are labeled "25" and "26" by number on the board header —
//   NOT the "TX"/"RX" labels near the USB connector (those are GPIO1/GPIO3,
//   the USB debug UART, and must NOT be used).
//   Wiring:
//     Flipper GPIO C1 (pin 15, TX) ──→ ESP32 pin "25"  (our RX)
//     Flipper GPIO C0 (pin 16, RX) ──→ ESP32 pin "26"  (our TX)
//     Flipper GND     (pin 18)     ──→ ESP32 GND
//     Flipper 3.3V    (pin 9)      ──→ ESP32 3.3V  ⚠ NOT 5V!
#define RX_PIN   25
#define TX_PIN   26
#define UART_BAUD 115200

// ── BLE UUIDs ─────────────────────────────────────────────────────────────────

// ATC_TLSR_Paper (Hanshow Stellar with custom firmware)
#define ATC_SERVICE_UUID  "13187B10-EBA9-A3BA-044E-83D3217D9A38"
#define ATC_CHAR_UUID     "4B646063-6264-F3A7-8941-E65356EA82FE"

// BT SIG ESL Service (Vusion / SES-imagotag HRD3 series)
#define ESL_SERVICE_UUID  "0000184D-0000-1000-8000-00805F9B34FB"

// ESL characteristic UUIDs (16-bit, wrapped in 128-bit base)
#define ESL_CHAR_ADDRESS       "00002BF6-0000-1000-8000-00805F9B34FB"
#define ESL_CHAR_AP_SYNC_KEY   "00002BF7-0000-1000-8000-00805F9B34FB"
#define ESL_CHAR_RESPONSE_KEY  "00002BF8-0000-1000-8000-00805F9B34FB"
#define ESL_CHAR_CURRENT_TIME  "00002BF9-0000-1000-8000-00805F9B34FB"
#define ESL_CHAR_CONTROL_POINT "00002BFE-0000-1000-8000-00805F9B34FB"

// ── ATC protocol constants ────────────────────────────────────────────────────

#define ATC_CMD_CLEAR     0x00
#define ATC_CMD_DISPLAY   0x01
#define ATC_CMD_SET_POS   0x02
#define ATC_CMD_WRITE     0x03

#define ATC_DISPLAY_W     250
#define ATC_DISPLAY_H     122
#define ATC_BUF_SIZE      4000   // (250/8)*128 rounded = 4000 bytes
#define ATC_MTU_PAYLOAD   239    // max bytes per GATT write

// ── ESL (Vusion) protocol opcodes ─────────────────────────────────────────────

#define ESL_OP_PING           0x00
#define ESL_OP_FACTORY_RESET  0x03
#define ESL_OP_DISPLAY_IMAGE  0x20
#define ESL_OP_REFRESH        0x11

// ── Serial bridge state ───────────────────────────────────────────────────────

HardwareSerial flipperSerial(1);
char           cmd_buf[256];
uint8_t        cmd_len = 0;

// ── Scan result storage ───────────────────────────────────────────────────────

struct EslScanResult {
    char  name[32];
    char  mac[18];
    int   rssi;
    bool  is_vusion;
};

static EslScanResult scan_results[32];
static uint8_t       scan_count = 0;

// ── Utility: send a line to Flipper ──────────────────────────────────────────

static void flipper_send(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    flipperSerial.println(buf);
    Serial.printf("[TX->Flipper] %s\n", buf);
}

// ── BLE Scan ──────────────────────────────────────────────────────────────────

// Semaphore signalled by onScanEnd so cmd_scan() can block until complete.
static SemaphoreHandle_t g_scan_sem = nullptr;

class ESLAdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        Serial.printf("[SCAN] onScanEnd reason=%d\n", reason);
        if(g_scan_sem) xSemaphoreGive(g_scan_sem);
    }

    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if(scan_count >= 32) return;

        bool is_atc    = false;
        bool is_vusion = false;

        // ATC tags advertise with name "ESL_XXXXXX"
        std::string name = dev->getName();
        if(name.substr(0, 4) == "ESL_") {
            is_atc = true;
        }

        // Vusion/SES-imagotag tags — primary: BT SIG ESL Service 0x184D
        if(dev->isAdvertisingService(NimBLEUUID("184D"))) {
            is_vusion = true;
        }

        // Vusion/SES-imagotag — fallback: name-based detection for devices that
        // don't include service UUID in adv packet (e.g. EDB1 series, older HRD).
        // Known advertisement name prefixes:
        //   "VUS_"      — modern Vusion tags (V100/V300/V700 series)
        //   "VUSION"    — older Vusion branding
        //   "ESL-"      — generic SES-imagotag BLE labels
        //   "EDB"       — EDB1 series (e.g. EDB1-0210-A) 2-battery Vusion tags
        //   "IMAGOTAG"  — legacy SES-imagotag branding
        //   "SES_"      — SES-imagotag store electronic systems
        if(!is_atc && !is_vusion) {
            const char* prefixes[] = { "VUS_", "VUSION", "ESL-", "EDB", "IMAGOTAG", "SES_" };
            for(auto& pfx : prefixes) {
                if(name.substr(0, strlen(pfx)) == pfx) {
                    is_vusion = true;
                    break;
                }
            }
        }

        if(!is_atc && !is_vusion) return;

        // Deduplicate by MAC
        std::string mac_str = dev->getAddress().toString();
        for(uint8_t i = 0; i < scan_count; i++) {
            if(strcmp(scan_results[i].mac, mac_str.c_str()) == 0) return;
        }

        EslScanResult& r = scan_results[scan_count++];
        strncpy(r.name, name.c_str(), sizeof(r.name) - 1);
        strncpy(r.mac,  mac_str.c_str(), sizeof(r.mac) - 1);
        r.rssi      = dev->getRSSI();
        r.is_vusion = is_vusion;

        flipper_send("DEVICE %s %s %d %s",
                     r.name[0] ? r.name : "?",
                     r.mac,
                     r.rssi,
                     is_vusion ? "VUSION" : "ATC");
    }
};

static ESLAdvertisedDeviceCallbacks g_scan_callbacks;

static void cmd_scan(void) {
    scan_count = 0;
    memset(scan_results, 0, sizeof(scan_results));

    g_scan_sem = xSemaphoreCreateBinary();

    Serial.println("[SCAN] BLE scan starting (10s)...");
    unsigned long t = millis();

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_scan_callbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    bool started = scan->start(10000, false);   // non-blocking in v2.x
    Serial.printf("[SCAN] start() returned %d — waiting for onScanEnd...\n", started);

    if(started) {
        xSemaphoreTake(g_scan_sem, pdMS_TO_TICKS(15000));   // block up to 15s
    }

    vSemaphoreDelete(g_scan_sem);
    g_scan_sem = nullptr;

    Serial.printf("[SCAN] done. elapsed=%lums found=%u\n", millis() - t, scan_count);
    flipper_send("DONE");
}

// ── ATC (Hanshow Stellar) ─────────────────────────────────────────────────────

// Render price+label text into 4000-byte monochrome bitmap.
// Layout: 250×122px, rotated 90° CW. black=0, white=1 (inverted).
// For the ESP32 bridge we use a very simple font: each character is 8×8 pixels,
// scaled 3× for the price (24px tall) and 1× for the label (8px tall).
// Characters are rendered from the built-in 8×8 IBM PC font embedded below.

// Minimal 8×8 font for printable ASCII 0x20–0x7E (95 chars)
// Each entry is 8 bytes, one byte per row (MSB = leftmost pixel).
static const uint8_t FONT_8X8[][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 0x21 '!'
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x22 '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 0x23 '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 0x24 '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 0x25 '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 0x26 '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // 0x27 '\''
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 0x28 '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 0x29 ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 0x2A '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 0x2B '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 0x2C ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 0x2D '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 0x2E '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 0x2F '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0x30 '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 0x31 '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 0x32 '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 0x33 '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 0x34 '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 0x35 '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 0x36 '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 0x37 '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 0x38 '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 0x39 '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 0x3A ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 0x3B ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 0x3C '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 0x3D '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 0x3E '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 0x3F '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 0x40 '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 0x41 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 0x42 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 0x43 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 0x44 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 0x45 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 0x46 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 0x47 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 0x48 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x49 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 0x4A 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 0x4B 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 0x4C 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 0x4D 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 0x4E 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 0x4F 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 0x50 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 0x51 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 0x52 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 0x53 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x54 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 0x55 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 0x56 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 0x57 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 0x58 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 0x59 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 0x5A 'Z'
    // (remaining glyphs omitted for brevity — treated as space)
};

static uint8_t atc_buf[ATC_BUF_SIZE];

// Set a pixel in atc_buf (rotated 90° CW: each "column" of 16 bytes).
// black=0 (pixel on), white=1 (pixel off).
static void atc_set_pixel(int x, int y, bool black) {
    if(x < 0 || x >= ATC_DISPLAY_W || y < 0 || y >= ATC_DISPLAY_H) return;
    int byte_idx = (ATC_DISPLAY_W - 1 - x) * 16 + (y >> 3);
    int bit_pos  = 0x80 >> (y & 7);
    if(byte_idx >= ATC_BUF_SIZE) return;
    if(black) {
        atc_buf[byte_idx] &= ~bit_pos;   // black = 0
    } else {
        atc_buf[byte_idx] |= bit_pos;    // white = 1
    }
}

// Draw one character at (cx, cy), scaled by `scale`. black on white.
static void atc_draw_char(char c, int cx, int cy, int scale) {
    if(c < 0x20 || c > 0x5A) c = ' ';   // clamp to available glyphs
    const uint8_t* glyph = FONT_8X8[c - 0x20];
    for(int row = 0; row < 8; row++) {
        for(int col = 0; col < 8; col++) {
            bool on = (glyph[row] >> (7 - col)) & 1;
            if(on) {
                for(int sy = 0; sy < scale; sy++)
                    for(int sx = 0; sx < scale; sx++)
                        atc_set_pixel(cx + col*scale + sx, cy + row*scale + sy, true);
            }
        }
    }
}

// Render price_str (large, 3× scale) and label_str (small, 1× scale) into atc_buf.
static void atc_render_price(const char* price_str, const char* label_str) {
    // White background
    memset(atc_buf, 0xFF, ATC_BUF_SIZE);

    // Draw price centered horizontally, 3× scale
    int price_len = strlen(price_str);
    int price_w   = price_len * 8 * 3;
    int price_x   = (ATC_DISPLAY_W - price_w) / 2;
    if(price_x < 2) price_x = 2;
    for(int i = 0; i < price_len; i++) {
        atc_draw_char(price_str[i], price_x + i * 8 * 3, 16, 3);
    }

    // Draw label centered, 1× scale
    if(label_str && label_str[0]) {
        int label_len = strlen(label_str);
        int label_w   = label_len * 8;
        int label_x   = (ATC_DISPLAY_W - label_w) / 2;
        if(label_x < 2) label_x = 2;
        for(int i = 0; i < label_len; i++) {
            atc_draw_char(label_str[i], label_x + i * 8, 92, 1);
        }
    }
}

// Upload atc_buf to an ATC tag over BLE GATT.
static bool atc_upload(const char* mac) {
    NimBLEAddress addr(mac, BLE_ADDR_RANDOM);
    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(10);

    flipper_send("PROGRESS 0");

    if(!client->connect(addr)) {
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR BLE connect failed: %s", mac);
        return false;
    }

    NimBLERemoteService*        svc  = client->getService(ATC_SERVICE_UUID);
    NimBLERemoteCharacteristic* chr  = svc ? svc->getCharacteristic(ATC_CHAR_UUID) : nullptr;

    if(!svc || !chr) {
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR ATC service/char not found");
        return false;
    }

    // 1. Clear display (fill white)
    {
        uint8_t clear[2] = { ATC_CMD_CLEAR, 0xFF };
        chr->writeValue(clear, 2, true);
    }
    delay(200);

    // 2. Set write position to 0
    {
        uint8_t setpos[3] = { ATC_CMD_SET_POS, 0x00, 0x00 };
        chr->writeValue(setpos, 3, true);
    }
    delay(100);

    // 3. Write buffer in chunks
    int total = ATC_BUF_SIZE;
    int sent  = 0;
    while(sent < total) {
        int chunk = total - sent;
        if(chunk > ATC_MTU_PAYLOAD) chunk = ATC_MTU_PAYLOAD;

        uint8_t pkt[ATC_MTU_PAYLOAD + 1];
        pkt[0] = ATC_CMD_WRITE;
        memcpy(pkt + 1, atc_buf + sent, chunk);
        chr->writeValue(pkt, chunk + 1, true);

        sent += chunk;
        flipper_send("PROGRESS %d", (int)(sent * 95 / total));
        delay(20);
    }

    // 4. Display (flush buffer to e-ink)
    {
        uint8_t disp[1] = { ATC_CMD_DISPLAY };
        chr->writeValue(disp, 1, true);
    }

    flipper_send("PROGRESS 100");
    delay(200);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return true;
}

// Clear an ATC display (fill with white, then refresh)
static bool atc_clear(const char* mac) {
    NimBLEAddress addr(mac, BLE_ADDR_RANDOM);
    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(10);

    if(!client->connect(addr)) {
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR BLE connect failed: %s", mac);
        return false;
    }

    NimBLERemoteService*        svc = client->getService(ATC_SERVICE_UUID);
    NimBLERemoteCharacteristic* chr = svc ? svc->getCharacteristic(ATC_CHAR_UUID) : nullptr;

    if(!svc || !chr) {
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR ATC service/char not found");
        return false;
    }

    uint8_t clear[2]  = { ATC_CMD_CLEAR, 0xFF };
    uint8_t disp[1]   = { ATC_CMD_DISPLAY };
    chr->writeValue(clear, 2, true);
    delay(200);
    chr->writeValue(disp, 1, true);
    delay(200);

    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return true;
}

// ── Vusion (BT SIG ESL) ───────────────────────────────────────────────────────

static bool vusion_connect_bonded(const char* mac, NimBLEClient** out_client) {
    NimBLEAddress addr(mac, BLE_ADDR_RANDOM);
    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(15);

    // "Just works" pairing with bonding — no passkey needed
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    if(!client->connect(addr)) {
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR BLE connect failed: %s", mac);
        return false;
    }

    // Initiate pairing/bonding
    if(!client->secureConnection()) {
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR BLE bonding failed");
        return false;
    }

    *out_client = client;
    return true;
}

static NimBLERemoteCharacteristic* vusion_get_char(NimBLEClient* client, const char* uuid) {
    NimBLERemoteService* svc = client->getService(ESL_SERVICE_UUID);
    if(!svc) return nullptr;
    return svc->getCharacteristic(uuid);
}

// Provision: write ESL address + generate sync/response keys + set time
static bool vusion_provision(const char* mac) {
    NimBLEClient* client = nullptr;
    if(!vusion_connect_bonded(mac, &client)) return false;

    // ESL Address (2 bytes, little-endian) — assign 0x0001
    NimBLERemoteCharacteristic* ch_addr = vusion_get_char(client, ESL_CHAR_ADDRESS);
    if(ch_addr) {
        uint8_t addr_val[2] = { 0x01, 0x00 };
        ch_addr->writeValue(addr_val, 2, true);
        delay(200);
    }

    // AP Sync Key (16 random bytes)
    NimBLERemoteCharacteristic* ch_sync = vusion_get_char(client, ESL_CHAR_AP_SYNC_KEY);
    if(ch_sync) {
        uint8_t key[16];
        esp_fill_random(key, sizeof(key));
        ch_sync->writeValue(key, 16, true);
        delay(200);
    }

    // Response Key (16 random bytes)
    NimBLERemoteCharacteristic* ch_resp = vusion_get_char(client, ESL_CHAR_RESPONSE_KEY);
    if(ch_resp) {
        uint8_t key[16];
        esp_fill_random(key, sizeof(key));
        ch_resp->writeValue(key, 16, true);
        delay(200);
    }

    // Current Time (4 bytes, little-endian Unix timestamp)
    NimBLERemoteCharacteristic* ch_time = vusion_get_char(client, ESL_CHAR_CURRENT_TIME);
    if(ch_time) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        uint8_t tv[4] = {
            (uint8_t)(t),
            (uint8_t)(t >> 8),
            (uint8_t)(t >> 16),
            (uint8_t)(t >> 24)
        };
        ch_time->writeValue(tv, 4, true);
        delay(200);
    }

    flipper_send("PROGRESS 100");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return true;
}

// Send a Control Point TLV command
static bool vusion_control_point(const char* mac, uint8_t opcode, const uint8_t* payload, uint8_t payload_len) {
    NimBLEClient* client = nullptr;
    if(!vusion_connect_bonded(mac, &client)) return false;

    NimBLERemoteCharacteristic* cp = vusion_get_char(client, ESL_CHAR_CONTROL_POINT);
    if(!cp) {
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        flipper_send("ERROR ESL Control Point char not found");
        return false;
    }

    // Subscribe to notifications for the response
    cp->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
        (void)data; (void)length;
        // Response received — could parse ESL response TLV here if needed
    });

    // Build TLV: [opcode][length][payload...]
    uint8_t tlv[32];
    tlv[0] = opcode;
    tlv[1] = payload_len;
    if(payload && payload_len > 0) {
        memcpy(tlv + 2, payload, payload_len);
    }
    cp->writeValue(tlv, 2 + payload_len, true);
    delay(500);  // Wait for e-ink refresh

    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return true;
}

static bool vusion_display(const char* mac, uint8_t slot) {
    // Display Image opcode 0x20: [display_idx 1B][image_idx 1B]
    uint8_t payload[2] = { 0x00, slot };
    return vusion_control_point(mac, ESL_OP_DISPLAY_IMAGE, payload, 2);
}

static bool vusion_ping(const char* mac) {
    // Ping opcode 0x00: no payload
    return vusion_control_point(mac, ESL_OP_PING, nullptr, 0);
}

static bool vusion_reset(const char* mac) {
    // Factory Reset opcode 0x03: no payload
    return vusion_control_point(mac, ESL_OP_FACTORY_RESET, nullptr, 0);
}

// ── Command parser ────────────────────────────────────────────────────────────

static void process_command(char* line) {
    char* token = strtok(line, " \r\n");
    if(!token) return;

    if(strcmp(token, "SCAN") == 0) {
        cmd_scan();

    } else if(strcmp(token, "ATC_PRICE") == 0) {
        char* mac   = strtok(nullptr, " \r\n");
        char* price = strtok(nullptr, " \r\n");
        char* label = strtok(nullptr, "\r\n");
        if(!mac || !price) { flipper_send("ERROR missing args"); return; }
        atc_render_price(price, label ? label : "");
        bool ok = atc_upload(mac);
        if(ok) flipper_send("OK Price set");
        else   flipper_send("ERROR Upload failed");

    } else if(strcmp(token, "ATC_CLEAR") == 0) {
        char* mac = strtok(nullptr, " \r\n");
        if(!mac) { flipper_send("ERROR missing MAC"); return; }
        bool ok = atc_clear(mac);
        if(ok) flipper_send("OK Display cleared");
        else   flipper_send("ERROR Clear failed");

    } else if(strcmp(token, "VUSION_PROVISION") == 0) {
        char* mac = strtok(nullptr, " \r\n");
        if(!mac) { flipper_send("ERROR missing MAC"); return; }
        bool ok = vusion_provision(mac);
        if(ok) flipper_send("OK Provisioned");
        else   flipper_send("ERROR Provision failed");

    } else if(strcmp(token, "VUSION_DISPLAY") == 0) {
        char* mac  = strtok(nullptr, " \r\n");
        char* slot = strtok(nullptr, " \r\n");
        if(!mac) { flipper_send("ERROR missing MAC"); return; }
        uint8_t idx = slot ? (uint8_t)atoi(slot) : 0;
        bool ok = vusion_display(mac, idx);
        if(ok) flipper_send("OK Display image %u", idx);
        else   flipper_send("ERROR Display failed");

    } else if(strcmp(token, "VUSION_PING") == 0) {
        char* mac = strtok(nullptr, " \r\n");
        if(!mac) { flipper_send("ERROR missing MAC"); return; }
        bool ok = vusion_ping(mac);
        if(ok) flipper_send("OK Pong");
        else   flipper_send("ERROR Ping failed");

    } else if(strcmp(token, "VUSION_RESET") == 0) {
        char* mac = strtok(nullptr, " \r\n");
        if(!mac) { flipper_send("ERROR missing MAC"); return; }
        bool ok = vusion_reset(mac);
        if(ok) flipper_send("OK Reset");
        else   flipper_send("ERROR Reset failed");

    } else {
        flipper_send("ERROR Unknown command: %s", token);
    }
}

// ── Arduino setup / loop ──────────────────────────────────────────────────────

void setup() {
    // Debug serial (USB) — optional
    Serial.begin(115200);

    // Flipper serial on UART1
    flipperSerial.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
    flipperSerial.setTimeout(100);

    // Initialize NimBLE (v2.x handles BT controller state internally)
    NimBLEDevice::init("ESL-Bridge");
    NimBLEDevice::setPower(3);   // max TX power (v2.x uses dBm integer, 3 = +3 dBm max)

    Serial.println("[ESL Bridge] Ready. Waiting for Flipper commands.");
}

void loop() {
    while(flipperSerial.available()) {
        char c = flipperSerial.read();
        if(c == '\n' || c == '\r') {
            if(cmd_len > 0) {
                cmd_buf[cmd_len] = '\0';
                Serial.printf("[CMD] %s\n", cmd_buf);
                process_command(cmd_buf);
                cmd_len = 0;
            }
        } else if(cmd_len < (sizeof(cmd_buf) - 1)) {
            cmd_buf[cmd_len++] = c;
        }
    }
    delay(5);
}
