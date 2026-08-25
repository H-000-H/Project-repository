# button — a generic key/button library

A single-header C++11 button library. You feed it the raw button level; it handles debounce, click counting and timing, then hands the result back to you through a callback. No third-party dependencies.

## Integration

Copy `button.hpp` into your project and add two definitions in any one `.cpp`:

```cpp
#include "button.hpp"
namespace button { uint32_t bit = 0; Button* Button::s_head = nullptr; }
```

`get_tick()` returns `-ENOSYS` by default. Edit its body in the header to return the system tick in milliseconds, e.g.:

```cpp
static inline int get_tick() { return static_cast<int>(HAL_GetTick()); }
```

## Usage

Create a button object. Use `static` or a global so it survives across scan cycles:

```cpp
button::Button btn;
```

In your periodic scan loop (50ms by default), feed the level and drive the state machine:

```cpp
btn.set_preesed(gpio_read());
button::Button::scan();   // drives all buttons in one call
```

Register a callback and handle events there:

```cpp
static bool on_event(void* user, button::Button& self)
{
    switch (self.event_read())
    {
    case button::Button_Event::kPressed_Down:         // first press down
    case button::Button_Event::kSignal_Pressed_Click: // single click
    case button::Button_Event::kDouble_Pressed_Click: // double click
    case button::Button_Event::kPressed_Repeat_Click: // repeat click (>=3)
    case button::Button_Event::kPressed_Long_Start:   // long press start
    case button::Button_Event::kPressed_Long_Over:    // long press end
    case button::Button_Event::kPressed_Long_Hold:    // long press hold
    default: break;
    }
    return true;
}

btn.callback_register(on_event, &your_context);
```

`user` in the callback is the context pointer passed at registration; its type is up to you. Pass `nullptr` if you don't need it.

## Parameters

Timing parameters are public members; just set them on the object:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `debounce_ms` | 12 | Debounce time; set 0 with hardware debounce |
| `short_press_trigger_ms` | 50 | Short press threshold |
| `long_press_trigger_ms` | 1000 | Long press threshold |
| `long_hold_trigger_ms` | 3000 | Long press hold time |
| `max_multiple_clicks_interval_ms` | 500 | Multi-click settle interval |

## Notes

A more feature-complete version lives in the mini-tree project — take a look there if you need it.
