#include "nrf_common.h"
#include "../../core/mykeyboard.h"

// Pins are assigned in nrf_start() via NRFradio.begin(...); using -1 here avoids
// reading bruceConfigPins during static initialization (order is undefined).
RF24 NRFradio(-1, -1);
HardwareSerial NRFSerial = HardwareSerial(2); // Uses UART2 for External NRF's
SPIClass *NRFSPI = nullptr;

void nrf_info() {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextSize(FM);
    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
    tft.drawCentreString("_Disclaimer_", tftWidth / 2, 10, 1);
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.setCursor(15, 33);
    padprintln("These functions were made to be used in a controlled environment for STUDY only.");
    padprintln("");
    padprintln("DO NOT use these functions to harm people or companies, you can go to jail!");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    padprintln("");
    padprintln(
        "This device is VERY sensible to noise, so long wires or passing near VCC line can make "
        "things go wrong."
    );
    delay(1000);
    while (!check(AnyKeyPress)) { vTaskDelay(20 / portTICK_PERIOD_MS); }
}

bool nrf_start(NRF24_MODE mode) {
    bool result = false;
    if (mode == NRF_MODE_DISABLED) return false;

    if (CHECK_NRF_UART(mode)) {
        if (USBserial.getSerialOutput() == &Serial1) {
            displayError("(E) UART already in use", true);
            return false;
        }
        NRFSerial.begin(115200, SERIAL_8N1, bruceConfigPins.uart_bus.rx, bruceConfigPins.uart_bus.tx);
        Serial.println("NRF24 on Serial Started");
        result = true;
    };

    if (!CHECK_NRF_SPI(mode)) return result;

    // Without CE/CS there is no point in touching the SPI bus: RF24::begin()
    // would fail and the caller would show a misleading error.
    if (bruceConfigPins.NRF24_bus.cs == GPIO_NUM_NC || bruceConfigPins.NRF24_bus.io0 == GPIO_NUM_NC) {
        Serial.println("[NRF24] CE/CS pins are not configured for this board");
        return false;
    }

    pinMode(bruceConfigPins.NRF24_bus.cs, OUTPUT);
    digitalWrite(bruceConfigPins.NRF24_bus.cs, HIGH);
    pinMode(bruceConfigPins.NRF24_bus.io0, OUTPUT);
    digitalWrite(bruceConfigPins.NRF24_bus.io0, LOW);

    if (bruceConfigPins.NRF24_bus.mosi == (gpio_num_t)TFT_MOSI &&
        bruceConfigPins.NRF24_bus.mosi != GPIO_NUM_NC) { // (T_EMBED), CORE2 and others
#if TFT_MOSI > 0 // condition for Headless and 8bit displays (no SPI bus)
        NRFSPI = &tft.getSPIinstance();
#else
        NRFSPI = &SPI;
#endif

    } else if (bruceConfigPins.NRF24_bus.mosi == bruceConfigPins.SDCARD_bus.mosi) {
        // CC1101 shares SPI with SDCard (Cardputer and CYDs)

        NRFSPI = &sdcardSPI;
    } else if (bruceConfigPins.NRF24_bus.mosi == bruceConfigPins.CC1101_bus.mosi &&
               bruceConfigPins.NRF24_bus.mosi != bruceConfigPins.SDCARD_bus.mosi) {
        // Smoochie board shares CC1101 and NRF24 SPI bus with different CS pins at
        // the same time, different from StickCs that uses the same Bus, but one at a
        // time (same CS Pin)
        NRFSPI = &CC_NRF_SPI;
    } else {
        NRFSPI = &SPI;
    }
    NRFSPI->begin(
        (int8_t)bruceConfigPins.NRF24_bus.sck,
        (int8_t)bruceConfigPins.NRF24_bus.miso,
        (int8_t)bruceConfigPins.NRF24_bus.mosi
    );
    delay(10);

    if (!NRFradio.begin(
            NRFSPI,
            rf24_gpio_pin_t(bruceConfigPins.NRF24_bus.io0),
            rf24_gpio_pin_t(bruceConfigPins.NRF24_bus.cs)
        )) {
        Serial.println("[NRF24] begin() failed - check SPI pins and wiring");
        return false;
    }

    // RF24::begin() only validates the pins/SPI object.  It does not verify that
    // a chip is really answering, so a missing module used to look like success
    // and every feature silently did nothing.  Retry once, then report honestly.
    bool chipFound = NRFradio.isChipConnected();
    if (!chipFound) {
        delay(50);
        chipFound = NRFradio.isChipConnected();
    }
    if (!chipFound) {
        Serial.println(
            "[NRF24] chip not detected - check 3.3V, GND, CE/CS and SPI wiring "
            "(MISO may be floating)"
        );
        return false;
    }

    // Sane, explicit defaults.  Individual features override what they need.
    NRFradio.setChannel(76);
    NRFradio.setDataRate(RF24_1MBPS);
    NRFradio.setPALevel(RF24_PA_MAX);
    NRFradio.setAutoAck(true);
    NRFradio.setCRCLength(RF24_CRC_16);

    Serial.println("[NRF24] radio detected and initialized");
    return true;
}

NRF24_MODE nrf_setMode() {
    NRF24_MODE mode = NRF_MODE_DISABLED;
    options = {
        {"SPI Mode",   [&]() { mode = NRF_MODE_SPI; } },
        {"UART Mode",  [&]() { mode = NRF_MODE_UART; }},
        {"SPI + UART", [&]() { mode = NRF_MODE_BOTH; }},
        {"Main Menu", [=]() { returnToMenu = true; } }
    };
    loopOptions(options);
    return mode;
}
