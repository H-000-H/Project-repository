# Multi-State-Button

A generic C++ button library for embedded systems. Supports debounce, single/double/repeat click, short press, long press, and long hold — all driven by a single state machine per button.

## Features

- **State machine per button**: press down → short/long/hold detection → multi-click judgment → event settlement.
- **Automatic linked-list management**: constructing a `Button` object auto-allocates an ID and appends to the scan list; destroying removes it.
- **Up to 32 buttons** (bitmap ID allocation, 1–32).
- **Configurable timing**: scan period, debounce, short press, long press, long hold, multi-click interval, repeat threshold — all tunable per-instance at runtime or via compile-time defaults.
- **Debounce**: software debounce by default; set `debounce_ms = 0` to disable (e.g. when using RC hardware debounce).
- **Callback modes** (compile-time auto-switch, two mutually exclusive API sets):
  - **ETL delegate** (type-safe): auto-enabled when integrated with mini_tree (`DTC_GEN_COUNT_MINI_BUTTON > 0`) or when `CONFIG_BUTTON_USE_ETL` is defined and `<etl/delegate.h>` is available.
  - **Plain `void*` function pointer**: fallback when neither of the above; no third-party dependency, maximum portability.

## Quick Start

```cpp
#include "button.hpp"

// Create button objects (auto-registered)
static button::Button btn1;
static button::Button btn2;

// Callback (plain void* mode)
bool on_button_event(void* user, button::Button& self)
{
    auto evt = self.event_read();
    int id = self.read_id();
    // handle event...
    return true;
}

int main()
{
    btn1.callback_register(on_button_event);
    btn2.callback_register(on_button_event);

    while (1)
    {
        // Read GPIO and feed to buttons
        btn1.set_preesed(read_gpio_pin(BTN1_PIN));
        btn2.set_preesed(read_gpio_pin(BTN2_PIN));

        // Drive all state machines
        button::Button::scan();

        delay_ms(button::kScan_Freq_Ms);
    }
}
```

### Time Base

`button::get_tick()` is **already implemented in the library** — it returns `mini_os_get_tick()`. You do not need to define it yourself; defining it again in the same translation unit is a redefinition error.

If your project must source the tick elsewhere, edit the definition at the bottom of `button.hpp` instead of adding a second one.

### ETL Delegate Mode

```cpp
// Plain function callback
bool my_on_event(button::Button& self)
{
    auto evt = self.event_read();
    // handle event...
    return true;
}

// Register
btn1.callback_register(button::Button::Button_Callback::create<my_on_event>());

// Or with lvalue lambda capturing state
auto on_evt = [&data](button::Button& self) -> bool { /* ... */ };
btn1.callback_register(on_evt);
```

## Events

| Event | Description |
|---|---|
| `kPressed_Down` | First press detected |
| `kSignal_Pressed_Click` | Single click completed |
| `kDouble_Pressed_Click` | Double click completed |
| `kPressed_Repeat_Click` | Repeat click (≥ threshold) |
| `kPressed_Short_Start` | Short press start (triggered once) |
| `kPressed_Short_Over` | Short press ended |
| `kPressed_Long_Start` | Long press start (triggered once) |
| `kPressed_Long_Over` | Long press ended (triggered once) |
| `kPressed_Long_Hold` | Long press hold (triggered once) |
| `kPressed_Long_Hold_Over` | Long press hold ended |
| `kPressed_None` | No event |

## Compile-Time Defaults

| Constant | Default | Description |
|---|---|---|
| `kScan_Freq_Ms` | 50 | Scan period (ms) |
| `kDebounce_Ms` | 12 | Debounce duration (ms) |
| `kShort_Trigger_Ms` | 50 | Short press threshold (ms) |
| `kLong_Trigger_Ms` | 1000 | Long press threshold (ms) |
| `kLong_Hold_Trigger_Ms` | 3000 | Long hold threshold (ms) |
| `kMultiple_Click_Interval_Ms` | 500 | Multi-click interval (ms) |
| `kRepeat_Click_Threshold` | 3 | Repeat click threshold (≥ N) |

All timing parameters are also settable per-instance at runtime via public member variables.

## API Reference

### Namespace-level

| Function | Description |
|---|---|
| `Button::scan()` | Drive all button state machines; returns active count |
| `read_num()` | Number of allocated buttons |
| `get_tick()` | Returns system ms tick; already implemented, see *Time Base* |

### Button class

| Method | Description |
|---|---|
| `Button()` | Constructor: allocates ID, appends to scan list |
| `~Button()` | Destructor: frees ID, removes from list |
| `callback_register(cb[, user])` | Register event callback |
| `event_read()` | Get current event |
| `read_id()` | Get button ID (1–32) |
| `read_status()` | Get state machine state |
| `read_pressed()` | Get pressed level (0/1) |
| `set_preesed(level)` | Feed GPIO level (call every scan cycle) |
| `update()` | Advance state machine one step |

## Requirements

- C++17 or later
- Compiler with `__builtin_ctz` / `__builtin_popcount` support (GCC, Clang)
- Optional: [ETL](https://www.etlcpp.com/) for type-safe delegate callbacks

## License

Apache-2.0
