/*
Thanks to thoses developers for their projects:
* @7h30th3r0n3 : https://github.com/7h30th3r0n3/Evil-M5Core2 and https://github.com/7h30th3r0n3/PwnGridSpam
* @viniciusbo : https://github.com/viniciusbo/m5-palnagotchi
* @sduenasg : https://github.com/sduenasg/pio_palnagotchi

Thanks to @bmorcelli (Pirata) for his help doing a better code.
20/09 - Changed from DynamicJsonDocument json[2048] to JsonDocument json, to avoid stack smashing errors
(firgers crossed)
*/
#if !defined(LITE_VERSION)
#include "pwngrid.h"
#include "../wifi/sniffer.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h"
#include <algorithm>

uint8_t pwngrid_friends_tot = 0;
std::vector<pwngrid_peer> pwngrid_peers;
String pwngrid_last_friend_name = "";

// The sniffer callback runs in the WiFi task and mutates pwngrid_peers while
// the UI task copies/iterates it.  Without this lock a push_back during a UI
// copy can corrupt the vector and reboot the device.
static SemaphoreHandle_t pwngridMutex = nullptr;

static bool lockPwngrid() {
    if (pwngridMutex == nullptr) return true;
    return xSemaphoreTake(pwngridMutex, pdMS_TO_TICKS(50)) == pdTRUE;
}
static void unlockPwngrid() {
    if (pwngridMutex != nullptr) xSemaphoreGive(pwngridMutex);
}

uint8_t getPwngridTotalPeers() { return pwngrid_friends_tot; }
uint8_t getPwngridRunTotalPeers() { return pwngrid_friends_tot; }

String getPwngridLastFriendName() {
    if (!lockPwngrid()) return "";
    String name = pwngrid_last_friend_name;
    unlockPwngrid();
    return name;
}

std::vector<pwngrid_peer> getPwngridPeers() {
    if (!lockPwngrid()) return {};
    std::vector<pwngrid_peer> peers = pwngrid_peers;
    unlockPwngrid();
    return peers;
}

// Add pwngrid peers
void add_new_peer(JsonDocument &json, signed int rssi) {
    if (!lockPwngrid()) return;

    String identity = json["identity"].as<String>();
    // Check if it exists in the list
    for (auto &peer_list : pwngrid_peers) {
        if (peer_list.identity == identity) {
            peer_list.last_ping = millis();
            peer_list.gone = false;
            peer_list.rssi = rssi;
            unlockPwngrid();
            return;
        }
    }
    // Check if doesn't exists AND there are room in RAM memory to save
    if (pwngrid_peers.size() < 50) {
        pwngrid_peers.push_back((pwngrid_peer){
            json["epoch"].as<int>(),
            json["face"].as<String>(),
            json["grid_version"].as<String>(),
            identity,
            json["name"].as<String>(),
            json["pwnd_run"].as<int>(),
            json["pwnd_tot"].as<int>(),
            json["session_id"].as<String>(),
            json["timestamp"].as<int>(),
            json["uptime"].as<int>(),
            json["version"].as<String>(),
            rssi,
            millis(),
            false,
        }); // Add the local object into the vector
        // Update last friend and increment counter
        pwngrid_last_friend_name = json["name"].as<String>();
        pwngrid_friends_tot = pwngrid_peers.size();
    }
    unlockPwngrid();
}

// Had to remove Radiotap headers, since its automatically added
// Also had to remove the last 4 bytes (frame check sequence)
const uint8_t pwngrid_beacon_raw[] = {
    0x80, 0x00,                                     // FC
    0x00, 0x00,                                     // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // DA (broadcast)
    0xde, 0xad, 0xbe, 0xef, 0xde, 0xad,             // SA
    0xa1, 0x00, 0x64, 0xe6, 0x0b, 0x8b,             // BSSID
    0x40, 0x43,                                     // Sequence number/fragment number/seq-ctl
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp
    0x64, 0x00,                                     // Beacon interval
    0x11, 0x04,                                     // Capability info
    // 0xde (AC = 222) + 1 byte payload len + payload (AC Header)
    // For each 255 bytes of the payload, a new AC header should be set
};

const int raw_beacon_len = sizeof(pwngrid_beacon_raw);

esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

esp_err_t pwngridAdvertise(uint8_t channel, String face) {
    static uint8_t _chan = 1;
    JsonDocument pal_json;
    String pal_json_str = "";

    pal_json["pal"] = true; // Also detect other Palnagotchis
    pal_json["name"] = "Bruce";
    pal_json["face"] = face;
    pal_json["epoch"] = 1;
    pal_json["grid_version"] = "1.10.3";
    pal_json["identity"] = "32e9f315e92d974342c93d0fd952a914bfb4e6838953536ea6f63d54db6b9610";
    pal_json["pwnd_run"] = 0;
    pal_json["pwnd_tot"] = 0;
    pal_json["session_id"] = "a2:00:64:e6:0b:8b";
    pal_json["timestamp"] = 0;
    pal_json["uptime"] = 0;
    pal_json["version"] = "1.8.4";
    pal_json["policy"]["advertise"] = true;
    pal_json["policy"]["bond_encounters_factor"] = 20000;
    pal_json["policy"]["bored_num_epochs"] = 0;
    pal_json["policy"]["sad_num_epochs"] = 0;
    pal_json["policy"]["excited_num_epochs"] = 9999;

    serializeJson(pal_json, pal_json_str);
    uint16_t pal_json_len = measureJson(pal_json);
    uint8_t header_len = 2 + ((uint8_t)(pal_json_len / 255) * 2);
    size_t frame_len = raw_beacon_len + pal_json_len + header_len;
    uint8_t *pwngrid_beacon_frame = (uint8_t *)heap_caps_malloc(frame_len, MALLOC_CAP_8BIT);
    if (!pwngrid_beacon_frame) {
        ESP_LOGE("pwngrid", "Fail allocating memory for pwngrid beacon frame");
        return ESP_ERR_NO_MEM;
    }
    memcpy(pwngrid_beacon_frame, pwngrid_beacon_raw, raw_beacon_len);

    // Iterate through json string and copy it to beacon frame
    int frame_byte = raw_beacon_len;
    for (int i = 0; i < pal_json_len; i++) {
        // Write AC and len tags before every 255 bytes
        if (i == 0 || i % 255 == 0) {
            pwngrid_beacon_frame[frame_byte++] = 0xde; // AC = 222
            uint8_t payload_len = 255;
            if (pal_json_len - i < 255) { payload_len = pal_json_len - i; }

            pwngrid_beacon_frame[frame_byte++] = payload_len;
        }

        // Append json byte to frame
        // If current byte is not ascii, add ? instead
        uint8_t next_byte = (uint8_t)'?';
        if (isAscii(pal_json_str[i])) { next_byte = (uint8_t)pal_json_str[i]; }

        pwngrid_beacon_frame[frame_byte++] = next_byte;
    }

    // Channel switch not working?
    // vTaskDelay(500 / portTICK_PERIOD_MS);
    if (_chan != channel) {
        esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGE("pwngrid", "Failed to set channel %d: %s", channel, esp_err_to_name(err));
        }
        vTaskDelay(102 / portTICK_PERIOD_MS);
        _chan = channel;
    }
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html#_CPPv417esp_wifi_80211_tx16wifi_interface_tPKvib
    esp_err_t result;
    for (int i = 0; i < 3; i++) {
        result = esp_wifi_80211_tx(WIFI_IF_AP, pwngrid_beacon_frame, frame_len, false);
        if (result != ESP_OK) {
            ESP_LOGE("pwngrid", "Failed sending advertising: %s", esp_err_to_name(result));
        }
        delay(2);
    }
    free(pwngrid_beacon_frame);
    return result;
}

const int away_threshold = 120000;

void checkPwngridGoneFriends() {
    if (!lockPwngrid()) return;

    bool removed = false;
    for (auto &peer_list : pwngrid_peers) {
        // millis() wraps, so use unsigned subtraction; the old expression
        // compared the wrong way around and never expired peers.
        if (millis() - peer_list.last_ping > (unsigned long)away_threshold) {
            peer_list.gone = true;
        }
        if (peer_list.gone) removed = true;
    }

    if (removed) {
        pwngrid_peers.erase(
            std::remove_if(
                pwngrid_peers.begin(),
                pwngrid_peers.end(),
                [](const pwngrid_peer &peer) { return peer.gone; }
            ),
            pwngrid_peers.end()
        );
        pwngrid_friends_tot = pwngrid_peers.size();
    }
    unlockPwngrid();
}

signed int getPwngridClosestRssi() {
    signed int closest = -1000;
    if (!lockPwngrid()) return closest;

    for (const auto &peer_list : pwngrid_peers) {
        // Check if peer is away
        if (peer_list.gone == false && peer_list.rssi > closest) { closest = peer_list.rssi; }
    }

    unlockPwngrid();
    return closest;
}

// Detect pwnagotchi adapted from Marauder
// https://github.com/justcallmekoko/ESP32Marauder/wiki/detect-pwnagotchi
// https://github.com/justcallmekoko/ESP32Marauder/blob/master/esp32_marauder/WiFiScan.cpp#L2255
typedef struct {
    int16_t fctl;
    int16_t duration;
    uint8_t da;
    uint8_t sa;
    uint8_t bssid;
    int16_t seqctl;
    unsigned char payload[];
} __attribute__((packed)) WifiMgmtHdr;

typedef struct {
    uint8_t payload[0];
    WifiMgmtHdr hdr;
} wifi_ieee80211_packet_t;

void getMAC(char *addr, uint8_t *data, uint16_t offset) {
    sprintf(
        addr,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        data[offset + 0],
        data[offset + 1],
        data[offset + 2],
        data[offset + 3],
        data[offset + 4],
        data[offset + 5]
    );
}

void pwnSnifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    sniffer(buf, type);
    wifi_promiscuous_pkt_t *snifferPacket = (wifi_promiscuous_pkt_t *)buf;
    WifiMgmtHdr *frameControl = (WifiMgmtHdr *)snifferPacket->payload;

    const uint8_t *frame = snifferPacket->payload;
    const uint16_t frameCtrl = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    const uint8_t frameType = (frameCtrl & 0x0C) >> 2;
    const uint8_t frameSubType = (frameCtrl & 0xF0) >> 4;

    if (frameType == 0x00 && frameSubType == 0x08) {
        const uint8_t *addr1 = snifferPacket->payload + 4;  // Adresse du destinataire (Adresse 1)
        const uint8_t *addr2 = snifferPacket->payload + 10; // Adresse de l'expéditeur (Adresse 2)
        const uint8_t *bssid = snifferPacket->payload + 16; // Adresse BSSID (Adresse 3)
        const uint8_t *apAddr;

        if (memcmp(addr1, bssid, 6) == 0) {
            apAddr = addr1;
        } else {
            apAddr = addr2;
        }
        BeaconList Beacon;
        memcpy(Beacon.MAC, apAddr, 6);
        Beacon.channel = ch;
        // Save a new MAC to Deauth (helper takes the beacon lock).
        sniffer_add_beacon(Beacon);
    }

    String src = "";
    String essid = "";

    if (type == WIFI_PKT_MGMT) {
        // Remove frame check sequence bytes
        int len = snifferPacket->rx_ctrl.sig_len - 4;
        int fctl = ntohs(frameControl->fctl);
        const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)snifferPacket->payload;
        const WifiMgmtHdr *hdr = &ipkt->hdr;

        // if ((snifferPacket->payload[0] == 0x80) && (buf == 0)) {
        if ((snifferPacket->payload[0] == 0x80)) {
            char addr[] = "00:00:00:00:00:00";
            getMAC(addr, snifferPacket->payload, 10);
            src.concat(addr);
            if (src == "de:ad:be:ef:de:ad") {
                // Just grab the first 255 bytes of the pwnagotchi beacon
                // because that is where the name is
                for (int i = 38; i < len; i++) {
                    if (isAscii(snifferPacket->payload[i])) { essid.concat((char)snifferPacket->payload[i]); }
                }

                JsonDocument sniffed_json; // ArduinoJson v6s
                DeserializationError result = deserializeJson(sniffed_json, essid);

                if (result == DeserializationError::Ok) {
                    // Serial.println("\nSuccessfully parsed json");
                    // serializeJson(json, Serial);  // ArduinoJson v6
                    add_new_peer(sniffed_json, snifferPacket->rx_ctrl.rssi);
                } else if (result == DeserializationError::IncompleteInput) {
                    Serial.println("Deserialization error: incomplete input");
                } else if (result == DeserializationError::NoMemory) {
                    Serial.println("Deserialization error: no memory");
                } else if (result == DeserializationError::InvalidInput) {
                    Serial.println("Deserialization error: invalid input");
                } else if (result == DeserializationError::TooDeep) {
                    Serial.println("Deserialization error: too deep");
                } else {
                    Serial.println(essid);
                    Serial.println("Deserialization error");
                }
            }
        }
    }
}

const wifi_promiscuous_filter_t filter = {
    .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
};

void initPwngrid() {
    // Reconfiguring/stopping WiFi while the WebUI is serving requests can
    // trigger AsyncTCP use-after-free reboots.
    cleanlyStopWebUiForWiFiFeature();
    ensureWifiPlatform();
    if (pwngridMutex == nullptr) { pwngridMutex = xSemaphoreCreateMutex(); }
    wifi_init_config_t WIFI_INIT_CONFIG = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&WIFI_INIT_CONFIG);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[Pwngrid] WiFi init failed: %s\n", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    // The Arduino stack may have left the radio running in STA/APSTA mode.
    // Restart it in AP mode so the sniffer has a valid interface.
    esp_wifi_stop();
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) { Serial.printf("[Pwngrid] set_mode failed: %s\n", esp_err_to_name(err)); }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        Serial.printf("[Pwngrid] start failed: %s\n", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(pwnSnifferCallback);
    // esp_wifi_set_ps(WIFI_PS_NONE);
    // Channels are 1..14; the old random(0, 14) could return 0, which is invalid.
    esp_wifi_set_channel(random(1, 15), WIFI_SECOND_CHAN_NONE);
    vTaskDelay(1 / portTICK_RATE_MS);
}
#endif
