#include "ir_utils.h"

void setup_ir_pin(int pin, uint8_t mode) {
    // gpio_reset_pin() aborts on a negative/invalid GPIO.  Config validation
    // should prevent this, but every IR caller goes through here.
    if (pin < 0 || pin >= GPIO_PIN_COUNT) return;
    if (bruceConfigPins.SDCARD_bus.checkConflict(pin)) sdcardSPI.end();
    gpio_reset_pin((gpio_num_t)pin);
    pinMode(pin, mode);
}
