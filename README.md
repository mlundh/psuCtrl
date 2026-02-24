# psuCtrl

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Command-line controller for Siglent SPD series bench PSUs over SCPI/TCP.

## Supported models

| Model | Channels | Max voltage | Max current |
|---|---|---|---|
| SPD3303X-E | CH1, CH2 | 30 V | 3 A |
| SPD1305X | CH1, CH2 | 30 V | 5 A |

The model is detected automatically from `*IDN?` on each connection.

## Requirements

- GCC with C11 support
- POSIX sockets (Linux / macOS)
- PSU reachable over TCP, port 5025 (default)

## Build

```sh
make
```

Binary is placed at `./psuCtrl`.

```sh
make install   # installs to ~/.local/bin/psuCtrl
make clean
```

## Usage

```
psuCtrl <host> [port] <command> [args...] [-n]
```

`-n` (dry-run) may appear anywhere on the command line. It prints the SCPI
commands that would be sent without making a network connection.

### Commands

| Command | Arguments | Description |
|---|---|---|
| `idn` | | Query instrument identity |
| `status` | | Print set-points, measurements and output state for all channels |
| `meas` | `<ch>` | Print measured voltage and current for one channel |
| `setvolt` | `<ch> <volts>` | Set programmed voltage |
| `setcurr` | `<ch> <amps>` | Set current limit |
| `set` | `<ch> <volts> <amps>` | Set voltage and current in one call |
| `on` | `<ch>` | Enable channel output |
| `off` | `<ch>` | Disable channel output |

Channel: `1` = CH1, `2` = CH2.

### Examples

```sh
# Read current state
psuCtrl 192.168.1.100 status

# Set CH1 to 12 V / 500 mA and enable
psuCtrl 192.168.1.100 set 1 12.0 0.5
psuCtrl 192.168.1.100 on 1

# Disable CH2
psuCtrl 192.168.1.100 off 2

# Check what commands would be sent (no network)
psuCtrl 192.168.1.100 set 1 3.3 1.0 -n

# Non-default port
psuCtrl 192.168.1.100 5025 status
```

## Architecture

```
main.c              Argument parsing, command dispatch, model detection
scpi_transport.c/h  Raw TCP transport (auto-reconnect, dry-run, timeouts)
spd_driver.c/h      SCPI command set for SPD1305X / SPD3303X-E
```

The instruments close the TCP connection after each response. The transport
layer reconnects automatically. For queries that require a preceding channel
selection (`INSTrument CHx`) both lines are packed into a single TCP write
via `scpi_select_and_query()` so the selection is in effect when the
instrument evaluates the query.

### SYSTem:STATus? decoding

The two models differ in how their status word encodes per-channel output state:

- **SPD3303X-E** — bit 4 (0x10) = CH1 output ON, bit 5 (0x20) = CH2 output ON.
  A single query covers all channels; no channel selection needed.
- **SPD1305X** — channel must be selected first with `INSTrument CHx`;
  bit 4 (0x10) then reflects only the selected channel.
