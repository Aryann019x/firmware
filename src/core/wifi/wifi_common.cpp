#include "core/wifi/wifi_common.h"
#include "core/display.h"    // using displayRedStripe  and loop options
#include "core/mykeyboard.h" // usinf keyboard when calling rename
#include "core/powerSave.h"
#include "core/settings.h"
#include "core/utils.h"
#include "core/wifi/wifi_mac.h" // Set Mac Address - @IncursioHack
#include <esp_event.h>
#include <esp_netif.h>
#include <globals.h>

static volatile bool timezoneTaskRunning = false;
static portMUX_TYPE timezoneTaskMux = portMUX_INITIALIZER_UNLOCKED;
static bool wifiTransitioning = false;

// Starts the one-shot timezone task at most once.  The old implementation
// stored the FreeRTOS task handle and cleared it from inside the task, which
// could race with xTaskCreate() writing the handle and leave a stale non-NULL
// value forever.
static void startTimezoneTask() {
    portENTER_CRITICAL(&timezoneTaskMux);
    const bool shouldStart = !timezoneTaskRunning;
    if (shouldStart) { timezoneTaskRunning = true; }
    portEXIT_CRITICAL(&timezoneTaskMux);
    if (!shouldStart) return;

    if (xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&timezoneTaskMux);
        timezoneTaskRunning = false;
        portEXIT_CRITICAL(&timezoneTaskMux);
    }
}

void ensureWifiPlatform() {
    static bool netifInitialized = false;
    static bool eventLoopCreated = false;
    static portMUX_TYPE platformMux = portMUX_INITIALIZER_UNLOCKED;

    portENTER_CRITICAL(&platformMux);
    bool needNetif = !netifInitialized;
    bool needLoop = !eventLoopCreated;
    portEXIT_CRITICAL(&platformMux);

    if (needNetif) {
        // The Arduino WiFi stack may have initialized netif already.  Calling
        // esp_netif_init() twice returns ESP_ERR_INVALID_STATE, and the old
        // ESP_ERROR_CHECK() turned that into an abort()/boot loop.
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            Serial.printf("[WiFi] esp_netif_init failed: %s\n", esp_err_to_name(err));
            return;
        }
        portENTER_CRITICAL(&platformMux);
        netifInitialized = true;
        portEXIT_CRITICAL(&platformMux);
    }

    if (needLoop) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            Serial.printf("[WiFi] event loop creation failed: %s\n", esp_err_to_name(err));
            return;
        }
        portENTER_CRITICAL(&platformMux);
        eventLoopCreated = true;
        portEXIT_CRITICAL(&platformMux);
    }
}

bool _wifiConnect(const String &ssid, int encryption) {
    String password = bruceConfig.getWifiPassword(ssid);
    if (password == "" && encryption > 0) { password = keyboard(password, 63, "Network Password:", true); }
    bool connected = _connectToWifiNetwork(ssid, password);
    bool retry = false;

    while (!connected) {
        wakeUpScreen();

        options = {
            {"Retry",  [&]() { retry = true; } },
            {"Cancel", [&]() { retry = false; }},
        };
        loopOptions(options);

        if (!retry) {
            wifiDisconnect();
            return false;
        }

        password = keyboard(password, 63, "Network Password:", true);
        connected = _connectToWifiNetwork(ssid, password);
    }

    if (connected) {
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();
        bruceConfig.addWifiCredential(ssid, password);

        // Start timezone update in background if not already running
        startTimezoneTask();
    }

    delay(200);
    return connected;
}

bool _connectToWifiNetwork(const String &ssid, const String &pwd) {
    drawMainBorderWithTitle("WiFi Connect");
    padprintln("");
    padprint("Connecting to: " + ssid + ".");
    WiFi.mode(WIFI_MODE_STA);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    WiFi.begin(ssid, pwd);

    int i = 1;
    while (WiFi.status() != WL_CONNECTED) {
        if (tft.getCursorX() >= tftWidth - 12) {
            padprintln("");
            padprint("");
        }
#ifdef HAS_SCREEN
        tft.print(".");
#else
        Serial.print(".");
#endif

        if (i > 20) {
            displayError("Wifi Offline");
            vTaskDelay(500 / portTICK_RATE_MS);
            break;
        }

        vTaskDelay(500 / portTICK_RATE_MS);
        i++;
    }

    return WiFi.status() == WL_CONNECTED;
}

bool _setupAP() {
    IPAddress AP_GATEWAY(172, 0, 0, 1);
    // Callers may arrive here right after wifiDisconnect(), which leaves the
    // radio in WIFI_OFF.  softAP()/softAPConfig() are only reliable once AP
    // mode is selected.
    if (!WiFi.mode(WIFI_MODE_AP)) {
        Serial.println("[WiFi] failed to select AP mode");
        wifiConnected = false;
        return false;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    if (!WiFi.softAPConfig(AP_GATEWAY, AP_GATEWAY, IPAddress(255, 255, 255, 0))) {
        Serial.println("[WiFi] softAPConfig failed");
        wifiConnected = false;
        return false;
    }
    if (!WiFi.softAP(bruceConfig.wifiAp.ssid, bruceConfig.wifiAp.pwd, 6, 0, 4, false)) {
        Serial.println("[WiFi] softAP failed");
        wifiConnected = false;
        return false;
    }
    wifiIP = WiFi.softAPIP().toString(); // update global var
    Serial.println("IP: " + wifiIP);
    wifiConnected = true;
    return true;
}

void wifiDisconnect() {
    wifiTransitioning = true;
    
    WiFi.softAPdisconnect(true); // turn off AP mode
    vTaskDelay(10 / portTICK_PERIOD_MS);
    WiFi.disconnect(true, true); // turn off STA mode
    vTaskDelay(10 / portTICK_PERIOD_MS);
    WiFi.mode(WIFI_OFF);         // enforces WIFI_OFF mode
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    wifiConnected = false;
    wifiTransitioning = false;
}

bool wifiConnectMenu(wifi_mode_t mode) {
    if (WiFi.isConnected()) return false; // safeguard

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        displayTextLine("WiFi busy, please wait...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }

    switch (mode) {
        case WIFI_AP: // access point
            WiFi.mode(WIFI_AP);
            return _setupAP();
            break;

        case WIFI_STA: { // station mode
            int nets;
            WiFi.mode(WIFI_MODE_STA);

            // wifiMACMenu();
            applyConfiguredMAC();

            bool refresh_scan = false;
            do {
                displayTextLine("Scanning..");
                WiFi.scanDelete();
                nets = WiFi.scanNetworks();
                options = {};
                if (nets < 0) {
                    displayError("WiFi scan failed", true);
                    WiFi.scanDelete();
                    break;
                }
                for (int i = 0; i < nets; i++) {
                    if (options.size() < 250) {
                        String ssid = WiFi.SSID(i);
                        int encryptionType = WiFi.encryptionType(i);
                        int32_t rssi = WiFi.RSSI(i);
                        int32_t ch = WiFi.channel(i);
                        // Check if the network is secured
                        String encryptionPrefix = (encryptionType == WIFI_AUTH_OPEN) ? "" : "#";
                        String encryptionTypeStr;
                        switch (encryptionType) {
                            case WIFI_AUTH_OPEN: encryptionTypeStr = "Open"; break;
                            case WIFI_AUTH_WEP: encryptionTypeStr = "WEP"; break;
                            case WIFI_AUTH_WPA_PSK: encryptionTypeStr = "WPA/PSK"; break;
                            case WIFI_AUTH_WPA2_PSK: encryptionTypeStr = "WPA2/PSK"; break;
                            case WIFI_AUTH_WPA_WPA2_PSK: encryptionTypeStr = "WPA/WPA2/PSK"; break;
                            case WIFI_AUTH_WPA2_ENTERPRISE: encryptionTypeStr = "WPA2/Enterprise"; break;
                            default: encryptionTypeStr = "Unknown"; break;
                        }

                        String optionText = encryptionPrefix + ssid + "(" + String(rssi) + "|" +
                                            encryptionTypeStr + "|ch." + String(ch) + ")";

                        options.push_back({optionText.c_str(), [=]() {
                                               _wifiConnect(ssid, encryptionType);
                                           }});
                    }
                }
                options.push_back({"Hidden SSID", [=]() {
                                       String __ssid = keyboard("", 32, "Your SSID");
                                       _wifiConnect(__ssid.c_str(), 8);
                                   }});
                addOptionToMainMenu();

                // Option labels are copied into the Option objects, so the raw
                // scan results can be released before blocking in the menu.
                WiFi.scanDelete();

                loopOptions(options);
                options.clear();

                if (check(EscPress)) {
                    refresh_scan = true;
                } else {
                    refresh_scan = false;
                }
            } while (refresh_scan);
        } break;

        case WIFI_AP_STA: // repeater mode
                          // _setupRepeater();
            break;

        default: // error handling
            Serial.println("Unknown wifi mode: " + String(mode));
            break;
    }

    if (returnToMenu) {
        wifiDisconnect(); // Forced turning off the wifi module if exiting back to the menu
        return false;
    }
    return wifiConnected;
}

void wifiConnectTask(void *pvParameters) {
    // A FreeRTOS task must never fall off the end of its entry function; doing
    // so aborts the device.  Delete the task explicitly on every early exit.
    if (WiFi.status() == WL_CONNECTED) {
        vTaskDelete(NULL);
        return;
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        vTaskDelete(NULL);
        return;
    }

    WiFi.mode(WIFI_MODE_STA);
    int nets = WiFi.scanNetworks();
    if (nets < 0) {
        Serial.println("[WiFi] background scan failed");
        vTaskDelete(NULL);
        return;
    }

    // Copy credentials before connecting: WiFi.begin() invalidates the scan
    // results that are being iterated.
    std::vector<std::pair<String, String>> known;
    known.reserve(nets);
    for (int i = 0; i < nets; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        String pwd = bruceConfig.getWifiPassword(ssid);
        if (pwd == "") continue;
        known.emplace_back(ssid, pwd);
    }
    WiFi.scanDelete();

    for (const auto &net : known) {
        WiFi.begin(net.first, net.second);
        bool connectedNow = false;
        for (int i = 0; i < 50; i++) {
            if (WiFi.status() == WL_CONNECTED) {
                wifiConnected = true;
                wifiIP = WiFi.localIP().toString();

                // Start timezone update in background if not already running
                startTimezoneTask();
                drawStatusBar();
                connectedNow = true;
                break;
            }
            vTaskDelay(100 / portTICK_RATE_MS);
        }
        // Stop after the first successful connection; otherwise the next
        // WiFi.begin() would immediately tear down the working link.
        if (connectedNow) break;
    }

    vTaskDelete(NULL);
    return;
}

String checkMAC() { return String(WiFi.macAddress()); }

bool wifiConnecttoKnownNet(void) {
    if (WiFi.isConnected()) { // already connected: keep the global state in sync
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();
        return true;
    }
    
    // Check if WiFi is in transition
    if (wifiTransitioning) {
        displayTextLine("WiFi busy, please wait...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }
    
    bool result = false;
    // The scan/connect below needs STA mode.  Leaving the radio in WIFI_OFF
    // makes scanNetworks() fail with a negative result.
    if (!WiFi.mode(WIFI_MODE_STA)) {
        displayError("WiFi mode failed", true);
        return false;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    displayTextLine("Scanning Networks..");
    WiFi.disconnect(true, true);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    int nets = WiFi.scanNetworks();
    if (nets < 0) {
        displayError("WiFi scan failed", true);
        return false;
    }

    // Snapshot known networks, then release the scan buffer before connecting.
    std::vector<std::pair<String, String>> known;
    known.reserve(nets);
    for (int i = 0; i < nets; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        String password = bruceConfig.getWifiPassword(ssid);
        if (password != "") known.emplace_back(ssid, password);
    }
    WiFi.scanDelete();

    for (const auto &net : known) {
        Serial.println("Connecting to: " + net.first);
        result = _connectToWifiNetwork(net.first, net.second);
        // Maybe it finds a known network and can't connect, then try the next
        // until it gets connected (or not)
        if (result) {
            Serial.println("Connected to: " + net.first);
            break;
        }
    }
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();

        // Start timezone update in background if not already running
        startTimezoneTask();
    }
    return result;
}

void updateTimezoneTask(void *pvParameters) {
    // Wait a bit for connection to stabilize before updating timezone
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    // Only update timezone if WiFi is still connected
    if (WiFi.isConnected() && wifiConnected) { updateClockTimezone(); }

    // Let a later connection start a fresh timezone update.
    portENTER_CRITICAL(&timezoneTaskMux);
    timezoneTaskRunning = false;
    portEXIT_CRITICAL(&timezoneTaskMux);
    vTaskDelete(NULL);
}
