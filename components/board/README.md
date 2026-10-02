# board

Owns the physical pin map of the ESP32-C3 SuperMini. This is the **only** component allowed to
translate logical functions into GPIO numbers, so the rest of the firmware stays portable.

## API

```c
#include "board.h"

ESP_ERROR_CHECK(board_init());
const board_pins_t *pins = board_pins();
```

`board_init()` validates that every configured pin is a legal GPIO on this target and that no two
functions share a pin, then warns if a functional pin sits on a strapping or USB-JTAG pin.

## Configuration

All pins come from Kconfig (`menuconfig` → *Board - ESP32-C3 SuperMini pin map*):

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_BOARD_IR_RX_GPIO` | 4 | IR receiver output |
| `CONFIG_BOARD_IR_TX_GPIO` | 5 | IR emitter driver |
| `CONFIG_BOARD_HOTKEY_GPIO` | 0 | Hotkey button (active-low) |
| `CONFIG_BOARD_STATUS_LED_GPIO` | 8 | Onboard status LED |
| `CONFIG_BOARD_STATUS_LED_ACTIVE_LOW` | y | LED illuminates on a low level |

## Reserved pins

GPIO2/8/9 are strapping pins and GPIO18/19 are USB-JTAG/serial. GPIO8 legitimately carries the
onboard LED; the other reserved pins must not be used for application functions.
