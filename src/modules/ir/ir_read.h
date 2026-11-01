/**
 * @file ir_read.h
 * @author Rennan Cockles (https://github.com/rennancockles)
 * @brief Read Infrared signals
 * @version 0.1
 * @date 2024-07-17
 */

#include <IRrecv.h>
#include <globals.h>

class IrRead {
public:
    // The receiver must be created *after* the RX pin is chosen in setup().
    // Creating it here binds it to the boot-time pin and ignores later changes.
    IRrecv *irrecv = nullptr;

    /////////////////////////////////////////////////////////////////////////////////////
    // Constructor
    /////////////////////////////////////////////////////////////////////////////////////
    IrRead(bool headless_mode = false, bool raw_mode = false);
    ~IrRead();

    // The receiver and raw buffer are owned pointers: copying the object would
    // leave two instances sharing them and double-free on destruction.
    IrRead(const IrRead &) = delete;
    IrRead &operator=(const IrRead &) = delete;

    ///////////////////////////////////////////////////////////////////////////////////
    // Arduino Life Cycle
    ///////////////////////////////////////////////////////////////////////////////////
    void setup();
    void loop();
    void quickLoop();

    String loop_headless(int max_loops);

private:
    bool _read_signal = false;
    decode_results results;
    uint16_t *rawcode = nullptr;
    uint16_t raw_data_len = 0;
    int signals_read = 0;
    int button_pos = 0;
    String strDeviceContent = "";
    bool headless = false;
    bool raw = false;

    /////////////////////////////////////////////////////////////////////////////////////
    // Display functions
    /////////////////////////////////////////////////////////////////////////////////////
    void cls();
    void display_banner();
    void display_btn_options();

    /////////////////////////////////////////////////////////////////////////////////////
    // Operations
    /////////////////////////////////////////////////////////////////////////////////////
    void begin();
    void read_signal();
    void save_device();
    void save_signal();
    void discard_signal();
    void append_to_file_str(String btn_name);
    bool write_file(String filename, FS *fs);
    String parse_raw_signal();
    String parse_state_signal();
    /////////////////////////////////////////////////////////////////////////////////////
    // Quick Remotes
    /////////////////////////////////////////////////////////////////////////////////////
    std::vector<String> quickButtonsTV = {"POWER", "UP",   "DOWN", "LEFT",  "RIGHT", "OK",       "SOURCES",
                                          "VOL+",  "VOL-", "CHA+", "CHA-",  "MUTE",  "SETTINGS", "NETFLIX",
                                          "HOME",  "BACK", "EXIT", "SMART", "1",     "2",        "3",
                                          "4",     "5",    "6",    "7",     "8",     "9",        "0"};
    std::vector<String> quickButtonsAC = {
        "POWER", "TEMP+", "TEMP-", "SPEED", "SWING", "SWING+", "SWING-", "JET", "UP", "DOWN", "MODE"
    };
    std::vector<String> quickButtonsFAN = {
        "POWER", "SPEED+", "SPEED-", "MODE", "TIMER", "SWING", "OSCILLATE", "UP", "DOWN", "LIGHT", "ION", "SLEEP"
    };
    std::vector<String> quickButtonsSOUND = {"POWER",    "UP",      "DOWN", "LEFT",    "RIGHT",
                                             "OK",       "SOURCES", "VOL+", "VOL-",    "MUTE",
                                             "SETTINGS", "BACK",    "EQ",   "REC",     "PLAY/PAUSE",
                                             "STOP",     "NEXT",    "PREV", "SHUFFLE", "REPEAT"};
    std::vector<String> quickButtonsLED = {
    "ON", "OFF", "BRIGHTNESS+", "BRIGHTNESS-",
    "RED", "GREEN", "BLUE", "WHITE",           
    "ORANGE", "PEA_GREEN", "DARK_BLUE",
    "DARK_YELLOW", "CYAN", "PURPLE",        
    "YELLOW", "LIGHT_BLUE", "MAGENTA",    
    "LIGHT_YELLOW", "SKY_BLUE", "ROSE",       
    "MODE_FLASH", "MODE_STROBE", "MODE_FADE", "MODE_SMOOTH" 
};
    std::vector<String> &quickButtons = quickButtonsTV;
};
