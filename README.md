# ksmaxis

A library for reading knob axis values using native OS APIs, designed for [ksm-v2](https://github.com/kshootmania/ksm-v2).

## Dependencies

| Platform | Library |
|----------|---------|
| Windows  | DirectInput 8 |
| Linux    | evdev |
| macOS    | IOKit HID |

## Supported Input Modes

- `InputMode::kAnalogStick` - Analog Stick X/Y (circular)
- `InputMode::kSlider` - Slider input (circular)
- `InputMode::kMouse` - Mouse X/Y (relative)

## Build

```bash
cmake -B build
cmake --build build
```

## License

MIT License
