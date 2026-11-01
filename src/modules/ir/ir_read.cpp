/**
 * @file ir_read.cpp
 * @author @im.nix (https://github.com/Niximkk)
 * @author Rennan Cockles (https://github.com/rennancockles)
 * @brief Read IR signals
 * @version 0.2
 * @date 2024-08-03
 */

#include "ir_read.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/settings.h"
#include "ir_utils.h"
#include <IRrecv.h>
#include <IRutils.h>
#include <globals.h>

/* Dont touch this */
// #define MAX_RAWBUF_SIZE 300
#define IR_FREQUENCY 38000
#define DUTY_CYCLE 0.330000

String uint32ToString(uint32_t value) {
    char buffer[12] = {0}; // 8 hex digits + 3 spaces + 1 null terminator
    snprintf(
        buffer,
        sizeof(buffer),
        "%02lX %02lX %02lX %02lX",
        value & 0xFF,
        (value >> 8) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 24) & 0xFF
    );
    return String(buffer);
}

String uint32ToStringInverted(uint32_t value) {
    char buffer[12] = {0}; // 8 hex digits + 3 spaces + 1 null terminator
    snprintf(
        buffer,
        sizeof(buffer),
        "%02lX %02lX %02lX %02lX",
        (value >> 24) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 8) & 0xFF,
        value & 0xFF
    );
    return String(buffer);
}

IrRead::IrRead(bool headless_mode, bool raw_mode) {
    headless = headless_mode;
    raw = raw_mode;
    setup();
}

IrRead::~IrRead() {
    if (rawcode != nullptr) {
        delete[] rawcode;
        rawcode = nullptr;
    }
    if (irrecv != nullptr) {
        irrecv->disableIRIn();
        delete irrecv;
        irrecv = nullptr;
    }
}

bool quickloop = false;

void IrRead::setup() {
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.enableOTG();
#endif

    // Validate the configured RX pin *before* creating the receiver.  The old
    // code enabled IRrecv first and then opened the pin picker, so a new pin
    // only took effect after a reboot.
    bool pinValid = false;
    const std::vector<std::pair<String, int>> pins = IR_RX_PINS;
    for (const auto &pin : pins) {
        if (pin.second == bruceConfigPins.irRx) {
            pinValid = true;
            break;
        }
    }
    if (!pinValid) {
        if (headless) {
            displayError("No valid IR RX pin", true);
            return;
        }
        gsetIrRxPin(true); // Open dialog to choose irRx pin
        // The dialog may have been cancelled: re-validate before listening.
        pinValid = false;
        for (const auto &pin : pins) {
            if (pin.second == bruceConfigPins.irRx) {
                pinValid = true;
                break;
            }
        }
        if (!pinValid) {
            displayError("No valid IR RX pin", true);
            return;
        }
    }

    if (bruceConfigPins.irRx < 0) {
        displayError("No IR RX pin configured", true);
        return;
    }

    setup_ir_pin(bruceConfigPins.irRx, INPUT_PULLUP);

    // Recreate the receiver with the final pin.  This also makes a pin change
    // from the settings menu take effect immediately.
    if (irrecv != nullptr) {
        irrecv->disableIRIn();
        delete irrecv;
        irrecv = nullptr;
    }
    irrecv = new IRrecv(bruceConfigPins.irRx, SAFE_STACK_BUFFER_SIZE / 2, 50);
    if (irrecv == nullptr) {
        displayError("IR receiver init failed", true);
        return;
    }
    // This IRremoteESP8266 fork returns void; a failed start shows up as
    // decode() never producing results, which read_signal() handles.
    irrecv->enableIRIn();

    if (headless) return;
    // else
    returnToMenu = true; // make sure menu is redrawn when quitting in any point
    std::vector<Option> quickRemoteOptions = {
        {"TV",
         [&]() {
             quickButtons = quickButtonsTV;
             begin();
             return loop();
         }                     },
        {"AC",
         [&]() {
             quickButtons = quickButtonsAC;
             begin();
             return loop();
         }                     },
        {"FAN",
         [&]() {
             quickButtons = quickButtonsFAN;
             begin();
             return loop();
         }                     },
        {"SOUND",
         [&]() {
             quickButtons = quickButtonsSOUND;
             begin();
             return loop();
         }                     },
        {"LED STRIP", [&]() {
             quickButtons = quickButtonsLED;
             begin();
             return loop();
         }},
    };
    options = {
        {"Custom Read",
         [&]() {
             begin();
             return loop();
         }                            },
        {"Quick Remote Setup  ",
         [&]() {
             quickloop = true;
             loopOptions(quickRemoteOptions);
         }                            },
        {"Menu",                 yield},
    };
    loopOptions(options);
}

void IrRead::loop() {
    // begin() can return early when no valid RX pin is configured.  Never
    // dereference a receiver that was not created.
    if (irrecv == nullptr) { return; }

    while (1) {
        if (check(EscPress)) {
            returnToMenu = true;
            button_pos = 0;
            quickloop = false;
#ifdef USE_BOOST /// DISABLE 5V OUTPUT
            PPM.disableOTG();
#endif
            break;
        }
        if (check(NextPress)) save_signal();
        if (quickloop && button_pos == quickButtons.size()) save_device();
        if (check(SelPress)) save_device();
        if (check(PrevPress)) discard_signal();

        read_signal();

        // decode() polls with a 50 ms timeout; still yield once per pass so
        // the idle task can feed the watchdog during long capture sessions.
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

void IrRead::begin() {
    _read_signal = false;

    display_banner();
    if (quickloop && button_pos < (int)quickButtons.size()) {
        padprintln("Waiting for signal of button: " + String(quickButtons[button_pos]));
    } else {
        padprintln("Waiting for signal...");
    }

    tft.println("");
    display_btn_options();

    delay(300);
}

void IrRead::cls() {
    drawMainBorder();
    tft.setCursor(10, 28);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
}

void IrRead::display_banner() {
    cls();
    tft.setTextSize(FM);
    padprintln("IR Read");

    tft.setTextSize(FP);
    padprintln("--------------");
    padprintln("Signals captured: " + String(signals_read));
    tft.println("");
}

void IrRead::display_btn_options() {
    tft.println("");
    tft.println("");
    if (_read_signal) {
        padprintln("Press [PREV] to discard signal");
        padprintln("Press [NEXT] to save signal");
    }
    if (signals_read > 0) { padprintln("Press [OK]   to save device"); }
    padprintln("Press [ESC]  to exit");
}

void IrRead::read_signal() {
    if (_read_signal || irrecv == nullptr || !irrecv->decode(&results)) return;

    _read_signal = true;

    // Always switches to RAW data, regardless of the decoding result
    raw = true;

    display_banner();

    // Dump of signal details
    padprint("RAW Data Captured:");
    String raw_signal = parse_raw_signal();
    tft.println(
        raw_signal.substring(0, 45) + (raw_signal.length() > 45 ? "..." : "")
    ); // Shows the RAW signal on the display

    display_btn_options();
    delay(500);
}

void IrRead::discard_signal() {
    if (!_read_signal) return;
    if (irrecv != nullptr) irrecv->resume();
    begin();
}

void IrRead::save_signal() {
    if (!_read_signal) return;
    if (!quickloop) {
        String btn_name = keyboard("Btn" + String(signals_read), 30, "Btn name:");
        append_to_file_str(btn_name);
    } else {
        if (button_pos < (int)quickButtons.size()) {
            append_to_file_str(quickButtons[button_pos]);
        } else {
            return;
        }
    }
    signals_read++;
    if (quickloop) button_pos++;
    discard_signal();
    delay(100);
}

String IrRead::parse_state_signal() {
    String r = "";
    uint16_t state_len = (results.bits) / 8;
    for (uint16_t i = 0; i < state_len; i++) {
        // r += uint64ToString(results.state[i], 16) + " ";
        r += ((results.state[i] < 0x10) ? "0" : ""); // adds 0 padding if necessary
        r += String(results.state[i], HEX) + " ";
    }
    r.toUpperCase();
    return r;
}

String IrRead::parse_raw_signal() {

    rawcode = resultToRawArray(&results);
    raw_data_len = getCorrectedRawLength(&results);

    // resultToRawArray() returns nullptr when the capture is empty or the
    // allocation fails.  Dereferencing it here used to crash IR Read.
    if (rawcode == nullptr || raw_data_len == 0) {
        if (rawcode != nullptr) {
            delete[] rawcode;
            rawcode = nullptr;
        }
        raw_data_len = 0;
        return "";
    }

    String signal_code = "";

    for (uint16_t i = 0; i < raw_data_len; i++) { signal_code += String(rawcode[i]) + " "; }

    delete[] rawcode;
    rawcode = nullptr;
    signal_code.trim();

    return signal_code;
}

void IrRead::append_to_file_str(String btn_name) {
    strDeviceContent += "name: " + btn_name + "\n";

    if (raw) {
        strDeviceContent += "type: raw\n";
        strDeviceContent += "frequency: " + String(IR_FREQUENCY) + "\n";
        strDeviceContent += "duty_cycle: " + String(DUTY_CYCLE) + "\n";
        strDeviceContent += "data: " + parse_raw_signal() + "\n";
    } else {
        // parsed signal  https://github.com/jamisonderek/flipper-zero-tutorials/wiki/Infrared
        strDeviceContent += "type: parsed\n";
        switch (results.decode_type) {
            case decode_type_t::RC5: {
                if (results.command > 0x3F) strDeviceContent += "protocol: RC5X\n";
                else strDeviceContent += "protocol: RC5\n";
                break;
            }
            case decode_type_t::RC6: {
                strDeviceContent += "protocol: RC6\n";
                break;
            }
            case decode_type_t::SAMSUNG: {
                strDeviceContent += "protocol: Samsung32\n";
                break;
            }
            case decode_type_t::SONY: {
                // check address and command ranges to find the exact protocol
                if (results.address > 0xFF) strDeviceContent += "protocol: SIRC20\n";
                else if (results.address > 0x1F) strDeviceContent += "protocol: SIRC15\n";
                else strDeviceContent += "protocol: SIRC\n";
                break;
            }
            case decode_type_t::NEC: {
                // check address and command ranges to find the exact protocol
                if (results.address > 0xFFFF) strDeviceContent += "protocol: NEC42ext\n";
                else if (results.address > 0xFF1F) strDeviceContent += "protocol: NECext\n";
                else if (results.address > 0xFF) strDeviceContent += "protocol: NEC42\n";
                else strDeviceContent += "protocol: NEC\n";
                break;
            }
            case decode_type_t::UNKNOWN: {
                Serial.print("unknown protocol, try raw mode");
                return;
            }
            default: {
                strDeviceContent += "protocol: " + typeToString(results.decode_type, results.repeat) + "\n";
                break;
            }
        }

        strDeviceContent += "address: " + uint32ToString(results.address) + "\n";
        strDeviceContent += "command: " + uint32ToString(results.command) + "\n";

        // extra fields not supported on flipper
        strDeviceContent += "bits: " + String(results.bits) + "\n";
        if (hasACState(results.decode_type)) strDeviceContent += "state: " + parse_state_signal() + "\n";
        else if (results.bits > 32)
            strDeviceContent += "value: " + uint32ToString(results.value) + " " +
                                uint32ToString(results.value >> 32) + "\n"; // MEMO: from uint64_t
        else strDeviceContent += "value: " + uint32ToStringInverted(results.value) + "\n";

        /*
        Serial.println(results.bits);
        Serial.println(results.address);
        Serial.println(results.command);
        Serial.println(results.overflow);
        Serial.println(results.repeat);
        Serial.println("value:");
        serialPrintUint64(results.address, HEX);
        serialPrintUint64(results.command, HEX);
        Serial.print("resultToHexidecimal: ");
        Serial.println(resultToHexidecimal(&results));
        Serial.println(results.value);
        String value = uint32ToString(results.value ) + " " + uint32ToString(results.value>> 32);
        value.replace(" ", "");
        uint64_t value_int = strtoull(value.c_str(), nullptr, 16);
        Serial.println(value_int);
        */
    }
    strDeviceContent += "#\n";
}

void IrRead::save_device() {
    if (signals_read == 0) return;

    String filename = keyboard("MyDevice", 30, "File name:");

    display_banner();

    FS *fs = nullptr;

    bool sdCardAvailable = setupSdCard();
    bool littleFsAvailable = checkLittleFsSize();

    if (sdCardAvailable && littleFsAvailable) {
        // ask to choose one
        options = {
            {"SD Card",  [&]() { fs = &SD; }      },
            {"LittleFS", [&]() { fs = &LittleFS; }},
        };

        loopOptions(options);
    } else if (sdCardAvailable) {
        fs = &SD;
    } else if (littleFsAvailable) {
        fs = &LittleFS;
    };

    if (fs && write_file(filename, fs)) {
        displaySuccess("File saved to " + String((fs == &SD) ? "SD Card" : "LittleFS") + ".", true);
        signals_read = 0;
        strDeviceContent = "";
        if (quickloop) {
            // The quick-remote sequence is complete: stop indexing the button
            // list, otherwise begin() reads past the end of the vector.
            quickloop = false;
            button_pos = 0;
        }
    } else displayError(fs ? "Error writing file." : "No storage available.", true);

    delay(1000);

    if (irrecv != nullptr) irrecv->resume();
    begin();
}

String IrRead::loop_headless(int max_loops) {

    if (irrecv == nullptr) return "";

    while (!irrecv->decode(&results)) { // MEMO: default timeout is 15ms
        max_loops -= 1;
        if (max_loops <= 0) {
            Serial.println("timeout");
            return ""; // nothing received
        }
        delay(1000);
        // delay(50);
    }

    irrecv->disableIRIn();

    if (!raw && results.decode_type == decode_type_t::UNKNOWN) {
        Serial.println("# decoding failed, try raw mode");
        return "";
    }

    if (results.overflow) displayWarning("buffer overflow, data may be truncated", true);
    // TODO: check results.repeat

    String r = "Filetype: IR signals file\n";
    r += "Version: 1\n";
    r += "#\n";
    r += "#\n";

    strDeviceContent = "";
    append_to_file_str("Unknown"); // writes on strDeviceContent
    r += strDeviceContent;

    return r;
}

bool IrRead::write_file(String filename, FS *fs) {
    if (fs == nullptr) return false;

    if (!(*fs).exists("/BruceIR")) (*fs).mkdir("/BruceIR");

    while ((*fs).exists("/BruceIR/" + filename + ".ir")) {
        int ch = 1;
        int i = 1;

        displayWarning("File \"" + String(filename) + "\" already exists", true);
        display_banner();

        // ask to choose one
        options = {
            {"Append number", [&]() { ch = 1; }},
            {"Overwrite ",    [&]() { ch = 2; }},
            {"Change name",   [&]() { ch = 3; }},
        };

        loopOptions(options);

        switch (ch) {
            case 1:
                filename += "_";
                while ((*fs).exists("/BruceIR/" + filename + String(i) + ".ir")) i++;
                filename += String(i);
                break;
            case 2: (*fs).remove("/BruceIR/" + filename + ".ir"); break;
            case 3:
                filename = keyboard(filename, 30, "File name:");
                display_banner();
                break;
        }
    }

    /*
    /Old "Add num index" solution

    if ((*fs).exists("/BruceIR/" + filename + ".ir")) {
        int i = 1;
        filename += "_";
        while((*fs).exists("/BruceIR/" + filename + String(i) + ".ir")) i++;
        filename += String(i);
    }
    */

    File file = (*fs).open("/BruceIR/" + filename + ".ir", FILE_WRITE);

    if (!file) { return false; }

    file.println("Filetype: Bruce IR File");
    file.println("Version: 1");
    file.println("#");
    file.println("# " + filename);
    file.print(strDeviceContent);

    file.close();
    delay(100);
    return true;
}
