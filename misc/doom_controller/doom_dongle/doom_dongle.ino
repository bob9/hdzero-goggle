// DOOM dongle: EdgeTX AUX serial -> ESP-NOW -> HDZero goggle backpack.
//
// Flash to any ESP32 dev board (Arduino IDE, board "ESP32 Dev Module").
// Wire the radio's "Lua" serial TX pin to the ESP32 RX2 (GPIO16), GND-GND,
// and power the board from the radio's 5V (or its own USB).
//
// Set MY_UID below to your ELRS bind UID (the same 6 numbers shown in the
// ExpressLRS Lua under "Bind Phrase" / on the backpack web UI). The goggle
// backpack only accepts ESP-NOW packets from a sender whose MAC equals the
// bind UID, so the dongle adopts it.
//
// The button mask is tunnelled as MSP_ELRS_SET_OSD (0x00B6) subcommand 0xD0,
// which the STOCK goggle backpack forwards verbatim to the goggles.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// >>> your ELRS bind UID here <<<
static uint8_t MY_UID[6] = {0, 0, 0, 0, 0, 0};

#define SERIAL_RX_PIN 16
#define FAILSAFE_MS 600 // send all-released if the radio goes quiet

static uint32_t lastFrameMs = 0;
static uint16_t lastMask = 0;

static uint8_t crc8_dvb_s2(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : crc << 1;
    return crc;
}

static void sendMaskEspnow(uint16_t mask) {
    // MSPv2 command frame: $ X < flags func16 size16 payload crc
    uint8_t payload[3] = {0xD0, (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8)};
    uint8_t frame[8 + sizeof(payload) + 1] = {'$', 'X', '<', 0,
                                              0xB6, 0x00, // function 0x00B6 LE
                                              sizeof(payload), 0x00};
    memcpy(&frame[8], payload, sizeof(payload));
    uint8_t crc = 0;
    for (int i = 3; i < 8 + (int)sizeof(payload); i++)
        crc = crc8_dvb_s2(crc, frame[i]);
    frame[8 + sizeof(payload)] = crc;
    esp_now_send(MY_UID, frame, sizeof(frame));
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, SERIAL_RX_PIN, -1);

    WiFi.mode(WIFI_STA);
    MY_UID[0] &= ~0x01; // MAC addresses can't be multicast
    esp_wifi_set_mac(WIFI_IF_STA, MY_UID);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("esp_now_init failed");
        ESP.restart();
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MY_UID, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    Serial.println("doom dongle ready");
}

void loop() {
    // frame from the radio: '$' 'D' mask_lo mask_hi xor-checksum
    static uint8_t buf[5];
    static int pos = 0;

    while (Serial2.available()) {
        uint8_t c = Serial2.read();
        if (pos == 0 && c != '$')
            continue;
        if (pos == 1 && c != 'D') {
            pos = 0;
            continue;
        }
        buf[pos++] = c;
        if (pos == 5) {
            pos = 0;
            if ((buf[2] ^ buf[3]) == buf[4]) {
                lastMask = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
                sendMaskEspnow(lastMask);
                lastFrameMs = millis();
            }
        }
    }

    // radio gone quiet mid-hold: release everything
    if (lastMask != 0 && millis() - lastFrameMs > FAILSAFE_MS) {
        lastMask = 0;
        sendMaskEspnow(0);
    }
}
