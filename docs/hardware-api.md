# Hardware API

The host talks to the Pico over USB-CDC at **115200 baud** using a framed serial protocol.
Payloads are JSON (except the ROM binary upload). This is designed for scripted bring-up
and CI.

## Framing

Every transaction follows the same byte sequence. The **receiver** always sends ACK (or
NACK on error) after EOT — whether the host or the Pico is sending:

```
Sender                         Receiver
  ENQ (0x05)          ────────►
  STX (0x02)          ────────►
                      ◄────────  ACK (0x06)     ← receiver ready for payload
  payload bytes       ────────►
  EOT (0x04)          ────────►
                      ◄────────  ACK (0x06) or NACK (0x15)   ← accepted/rejected
```

| Byte | Value | Meaning |
|---|---|---|
| ENQ | `0x05` | Start frame |
| STX | `0x02` | Start payload |
| ACK | `0x06` | Ready / accepted |
| EOT | `0x04` | End payload |
| NACK | `0x15` | Rejected (bad frame, unknown command, or payload too large) |

```{important}
Do not open a plain serial monitor on the port while using the Hardware API — unstructured
output corrupts framing. Only one process may hold the port at a time. The **`monitor`**
command also prints unstructured ASCII, and that state **persists on the Pico** until you
disable it or start a **`read`** capture (which auto-disables it).
```

## Commands

All commands are JSON sent in a framed payload (host → Pico). The Pico responds with a
framed JSON payload (Pico → host).

| Command | Request | Response |
|---|---|---|
| **reset** | `{"cmd":"reset","assert":true}` or `"assert":false` | `{"ok":true,"cmd":"reset","asserted":true}` |
| **upload_rom** | `{"cmd":"upload_rom","size":32768}` then a **binary frame** (32768 raw bytes) | `{"ok":true,"cmd":"upload_rom","bytes":32768,"reset_vector":"8000"}` |
| **read** | `{"cmd":"read","until":"stp","max_cycles":10000}` | streams `{"type":"cycle",...}` then `{"type":"done","reason":"stp",...}` |
| **request_addr** | `{"cmd":"request_addr"}` | `{"ok":true,"cmd":"request_addr","addr":"4000","phi2_hz":0.2}` |
| **monitor** | `{"cmd":"monitor","enable":true}` | enables/disables the ASCII bus table (off by default) |

### reset

Assert or release the 6502 RESET line (GP27). Use `"assert":true` to hold the CPU in
reset, `"assert":false` to let it run.

### upload_rom

A two-step transfer:

1. Send the JSON command frame; the Pico replies `{"ok":true,"awaiting":32768}`.
2. Send a binary frame containing exactly 32768 bytes.

RESET is asserted for the duration of the upload so the CPU cannot fetch half-written ROM
data, then released automatically.

### read

Captures bus activity as JSON. Streams one frame per PHI2 rising edge until the CPU
**fetches STP** (`0xDB` on a read cycle) or `max_cycles` is reached. Each cycle frame:

```json
{"type":"cycle","seq":1,"addr":"8000","data":"18","rw":0}
```

Final frame:

```json
{"type":"done","ok":true,"reason":"stp","cycles":14,"addr":"800D"}
```

To use this in automated tests, end your ROM with a **`STP` (`0xDB`)** instruction (not
`BRK` — that opcode is `0x00`).

At the default **0.2 Hz** clock, cycle frames arrive about **once every 5 seconds**. The
host waits up to **12 s** per frame (`READ_FRAME_TIMEOUT` in `hardware_api.py`). A full
capture from reset through STP typically takes **10–120 s** depending on program length.

### request_addr

Returns the last address sampled on the bus (updated every PHI2 rising edge).

### monitor

Toggles the human-readable ASCII bus table (disabled by default). **Mutually exclusive
with scripted capture:** table rows contain `|` (`0x7C`) and other bytes that collide with
framed protocol traffic. Disable it before upload/read:

```python
api.monitor(enable=False)
```

Example output:

```
| 01 |  18  |  8000   |  0 |  0.2  |
```

Use **`read`** (JSON cycle stream) for automated tests; reserve **`monitor`** for manual
breadboard observation.
