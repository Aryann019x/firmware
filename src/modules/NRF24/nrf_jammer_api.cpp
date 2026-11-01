#if !defined(LITE_VERSION)
#include "nrf_jammer_api.h"
#include "core/display.h"
#include "modules/ble/BLE_Suite.h"
#include "nrf_jammer.h"
#include <RF24.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static bool nrf24Initialized = false;
static NRF24_MODE nrf24Mode = NRF_MODE_DISABLED;
static bool bleJammingActive = false;
static BLEJamMode currentMode = BLE_JAM_ADV_CHANNELS;
static rf24_pa_dbm_e currentPowerLevel = RF24_PA_MAX;
static unsigned long lastChannelHop = 0;
static int currentChannelIndex = 0;
static int targetChannel = 0;
static bool isHopping = false;
static unsigned long jamStartTime = 0;

// Hopping is driven by a small background task.  The BLE Suite UI blocks in
// showDeviceInfoScreen() while the jammer runs, so the previously exposed
// updateBLEJammer() was never called and "hopping" modes sat on one channel.
static TaskHandle_t jammerTaskHandle = nullptr;
static SemaphoreHandle_t jammerTaskDone = nullptr;
static volatile bool jammerTaskRun = false;
static const uint8_t *activeHopChannels = nullptr;
static size_t activeHopCount = 0;
static uint16_t hopIntervalMs = 10;

// BLE channel -> nRF24 channel (2400 MHz + nRF channel).
// BLE data channels 0..36 are 2404..2438 MHz (4..76).
// BLE advertising channels are 37=2402, 38=2426, 39=2480 (2, 26, 80).
static inline uint8_t bleToNrfChannel(uint8_t bleChannel) {
    if (bleChannel >= 37) {
        static const uint8_t advMap[3] = {2, 26, 80};
        return advMap[bleChannel - 37];
    }
    return (uint8_t)(4 + 2 * bleChannel);
}

static const uint8_t bleAdvertisingChannels[] = {37, 38, 39};
// All 40 BLE channels, data first then advertising.
static const uint8_t bleAllChannels[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39
};

static void applyJammerRadioConfig() {
    NRFradio.setPALevel(currentPowerLevel);
    NRFradio.setAddressWidth(3);
    NRFradio.setPayloadSize(2);
    NRFradio.setDataRate(RF24_2MBPS);
    NRFradio.setAutoAck(false);
    NRFradio.setRetries(0, 0);
    NRFradio.disableCRC();
}

static void hopJammerChannel() {
    if (!bleJammingActive || !isHopping) return;
    if (activeHopChannels == nullptr || activeHopCount == 0) return;
    if (millis() - lastChannelHop < hopIntervalMs) return;

    currentChannelIndex = (currentChannelIndex + 1) % (int)activeHopCount;
    NRFradio.setChannel(bleToNrfChannel(activeHopChannels[currentChannelIndex]));
    lastChannelHop = millis();
}

static void bleJammerTask(void *arg) {
    (void)arg;
    while (jammerTaskRun) {
        hopJammerChannel();
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    if (jammerTaskDone != nullptr) { xSemaphoreGive(jammerTaskDone); }
    jammerTaskHandle = nullptr;
    vTaskDelete(NULL);
}

static void stopJammerTask() {
    jammerTaskRun = false;
    if (jammerTaskDone != nullptr) {
        // The task checks the flag at least once per tick and then signals.
        if (xSemaphoreTake(jammerTaskDone, pdMS_TO_TICKS(2000)) != pdTRUE) {
            Serial.println("[NRF24] jammer task did not stop in time");
        }
        vSemaphoreDelete(jammerTaskDone);
        jammerTaskDone = nullptr;
    }
    jammerTaskHandle = nullptr;
}

static bool startJammerTask() {
    stopJammerTask();

    jammerTaskDone = xSemaphoreCreateBinary();
    if (jammerTaskDone == nullptr) return false;

    jammerTaskRun = true;
    if (xTaskCreate(bleJammerTask, "nrf_ble_jam", 3072, nullptr, 1, &jammerTaskHandle) != pdPASS) {
        jammerTaskRun = false;
        vSemaphoreDelete(jammerTaskDone);
        jammerTaskDone = nullptr;
        jammerTaskHandle = nullptr;
        return false;
    }
    return true;
}

bool isNRF24Available() {
    if (nrf24Initialized) return true;

    // Ask once which transport to use.  The selected mode is cached so that
    // start/stop never prompt the user again (the old code opened a menu from
    // stopBLEJammer(), which could leave the carrier running forever).
    NRF24_MODE mode = nrf_setMode();
    if (returnToMenu || mode == NRF_MODE_DISABLED) return false;

    if (!nrf_start(mode)) return false;

    nrf24Mode = mode;
    if (CHECK_NRF_SPI(mode)) applyJammerRadioConfig();
    nrf24Initialized = true;
    return true;
}

bool startBLEJammer(BLEJamMode mode, int param) {
    if (mode == BLE_JAM_TARGET_CHANNEL && (param < 0 || param > 39)) return false;
    if (!isNRF24Available()) return false;
    if (!CHECK_NRF_SPI(nrf24Mode)) {
        // UART-only transport cannot generate a constant carrier.
        return false;
    }

    // Stop any previous carrier/task before reconfiguring.
    stopBLEJammer();

    currentMode = mode;
    targetChannel = param;
    applyJammerRadioConfig();

    // stopConstCarrier() calls powerDown(), and startConstCarrier() does not
    // restore PWR_UP.  Without this the second and later jams are silent.
    NRFradio.powerUp();
    delay(5);

    activeHopChannels = nullptr;
    activeHopCount = 0;
    isHopping = false;
    currentChannelIndex = 0;
    hopIntervalMs = 10;

    switch (mode) {
        case BLE_JAM_ADV_CHANNELS:
        case BLE_JAM_HOP_ADV:
            activeHopChannels = bleAdvertisingChannels;
            activeHopCount = sizeof(bleAdvertisingChannels);
            isHopping = true;
            hopIntervalMs = 10; // 3 channels -> 30 ms full cycle
            break;
        case BLE_JAM_ALL_CHANNELS:
        case BLE_JAM_HOP_ALL:
            activeHopChannels = bleAllChannels;
            activeHopCount = sizeof(bleAllChannels);
            isHopping = true;
            hopIntervalMs = 3; // 40 channels -> 120 ms full cycle
            break;
        case BLE_JAM_TARGET_CHANNEL:
            targetChannel = param;
            break;
        case BLE_JAM_CONNECT_ATTACK:
            // No carrier for this mode; the caller coordinates with the
            // connection attempt.  Keep the radio initialized.
            break;
    }

    if (mode != BLE_JAM_CONNECT_ATTACK) {
        uint8_t firstChannel = (activeHopChannels != nullptr)
                                   ? bleToNrfChannel(activeHopChannels[0])
                                   : bleToNrfChannel((uint8_t)targetChannel);
        NRFradio.startConstCarrier(currentPowerLevel, firstChannel);
    }

    bleJammingActive = true;
    jamStartTime = millis();
    lastChannelHop = millis();

    if (isHopping && !startJammerTask()) {
        NRFradio.stopConstCarrier();
        NRFradio.flush_tx();
        bleJammingActive = false;
        isHopping = false;
        activeHopChannels = nullptr;
        activeHopCount = 0;
        return false;
    }

    return true;
}

void updateBLEJammer() {
    // When the background task is running it owns the hop timing.  Keep this
    // function for external callers that drive the radio themselves.
    if (jammerTaskHandle != nullptr) return;
    hopJammerChannel();
}

void stopBLEJammer() {
    stopJammerTask();

    if (!bleJammingActive) {
        isHopping = false;
        activeHopChannels = nullptr;
        activeHopCount = 0;
        currentChannelIndex = 0;
        return;
    }

    // Never prompt for a mode here: stopping must always work.
    if (CHECK_NRF_SPI(nrf24Mode)) {
        NRFradio.stopConstCarrier();
        NRFradio.flush_tx();
    }
    bleJammingActive = false;
    isHopping = false;
    activeHopChannels = nullptr;
    activeHopCount = 0;
    currentChannelIndex = 0;
}

bool isBLEJammingActive() { return bleJammingActive; }

int getCurrentBLEChannel() {
    if (!bleJammingActive) return -1;
    if (currentMode == BLE_JAM_TARGET_CHANNEL) return targetChannel;
    if (!isHopping || activeHopChannels == nullptr || activeHopCount == 0) return -1;

    int idx = currentChannelIndex % (int)activeHopCount;
    if (idx < 0) idx = 0;
    return activeHopChannels[idx];
}

void setBLEJammingPower(int powerLevel) {
    rf24_pa_dbm_e paLevel = RF24_PA_MIN;
    if (powerLevel == 0) paLevel = RF24_PA_MIN;
    else if (powerLevel == 1) paLevel = RF24_PA_LOW;
    else if (powerLevel == 2) paLevel = RF24_PA_HIGH;
    else if (powerLevel == 3) paLevel = RF24_PA_MAX;
    else return;

    currentPowerLevel = paLevel;
    if (bleJammingActive) {
        BLEJamMode mode = currentMode;
        int param = targetChannel;
        stopBLEJammer();
        startBLEJammer(mode, param);
    }
}

bool jamBLEChannel(int channel) {
    if (channel < 0 || channel > 39) return false;
    return startBLEJammer(BLE_JAM_TARGET_CHANNEL, channel);
}

bool jamBLEAdvertisingChannels() { return startBLEJammer(BLE_JAM_ADV_CHANNELS); }

bool jamBLEConnectionChannel(NimBLEAddress target) { return startBLEJammer(BLE_JAM_ADV_CHANNELS); }

bool jamDuringConnect(NimBLEAddress target) {
    if (!isNRF24Available()) return false;
    startBLEJammer(BLE_JAM_ADV_CHANNELS);
    String connectionMethod = "";
    NimBLEClient *pClient = attemptConnectionWithStrategies(target, connectionMethod);
    stopBLEJammer();
    if (pClient) {
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return true;
    }
    return false;
}
#endif
