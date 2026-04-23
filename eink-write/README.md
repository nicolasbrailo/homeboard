# eink-write

CLI tool to write text or patterns to a Waveshare 2.13" e-Paper display using libeink.

## Usage

```
eink-write [options] [text...]
```

### Modes

| Flag | Description |
|---|---|
| (none) | Render the given text, auto-sized to fill the display |
| `-w` | Clear the display to white |
| `-b` | Fill the display black |
| `-d` | Demo: show build timestamp with a border |
| `-h` | Print usage |

### Examples

```bash
# Display a message
eink-write Hello World

# Clear to white
eink-write -w

# Run the demo pattern
eink-write -d
```

## Building

```bash
make
```

## Requirements

- SPI must be enabled (`sudo raspi-config nonint do_spi 0`, reboot required)
- Sufficient permissions to access `/dev/gpiochip*` and `/dev/spidev0.0`
