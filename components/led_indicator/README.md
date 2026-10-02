# led_indicator

Status indication for the onboard **single blue LED** on GPIO8 (active-low) using an LEDC PWM
backend. The LED is not an RGB part, so states are encoded as brightness and blink patterns.

## API

```c
#include "led_indicator.h"

ESP_ERROR_CHECK(led_indicator_init());
ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_ADVERTISING));
ESP_ERROR_CHECK(led_indicator_pulse(LED_PULSE_CAPTURE_OK));
```

| Function | Purpose |
|---|---|
| `led_indicator_init()` | Configure LEDC and start the pattern engine |
| `led_indicator_set_state()` | Switch to a steady state |
| `led_indicator_pulse()` | Play a one-shot indication, then return to the steady state |
| `led_indicator_deinit()` | Stop the engine and release the channel |

## States

| State | Pattern |
|---|---|
| `LED_STATE_OFF` | Dark |
| `LED_STATE_BOOT` | 3 × 120 ms flashes, then back to the base state |
| `LED_STATE_ADVERTISING` | Breathing, 2 s period, ~12.5 % peak |
| `LED_STATE_CONNECTED` | Solid dim (25 %) |
| `LED_STATE_LEARNING` | 5 Hz blink |
| `LED_STATE_ERROR` | 8 Hz blink for 5 s, then reverts to the previous state |
| `LED_STATE_FACTORY_RESET` | Fast strobe |

Pulses: `LED_PULSE_CAPTURE_OK` (250 ms flash), `LED_PULSE_IR_FRAME` (30 ms flash),
`LED_PULSE_HOTKEY_STEP` (double 80 ms pulse).

## Design notes

- **Active-low handled by hardware**: `ledc_channel_config_t.flags.output_invert` is set from
  `board_pins()->status_led_active_low`, so the pattern tables only deal with brightness.
- **Non-blocking**: a single 10 ms `esp_timer` advances a static step table; no task, no
  `vTaskDelay`, no busy-wait.
- **Static allocation**: pattern tables and engine state are all `static`; nothing is allocated per
  indication.
- **Layer compliance**: the pin number comes from `board`, never hardcoded here.
