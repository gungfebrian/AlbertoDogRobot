# Albert BLE protocol

This document is the compact contract between the ESP32 firmware and the iOS
controller. Keep the UUIDs and wire format stable when changing either side.

## Discovery and characteristics

The robot advertises as `AlbertMini` and exposes a Nordic UART-style service:

| Role | UUID | Direction |
|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Discovery |
| RX characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Phone writes commands |
| TX characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Robot notifies replies |

The phone should scan for the service UUID, discover both characteristics, and
subscribe to TX notifications before sending the first command. Commands are
plain UTF-8 text and are case-insensitive.

## Commands and replies

Send one command at a time, without assuming that a command reply is required.
Movement, pose, trick, dance, face, gaze, and autopilot commands are documented
in the main [README](../README.md). The system commands are:

| Command | Expected behavior |
|---|---|
| `STATUS` | Returns the current telemetry line without interrupting an active sequence. |
| `PET` | Applies the same interaction as the robot touch pad. |
| `AUTO` | Returns control to the robot's autonomous behavior. |
| `SAVE` | Persists the robot's state to flash and reports `saved`. |
| `RESET` | Wipes the stored brain and reports `memory wiped`. |
| `STATS` | Renders the state summary on the OLED. |

The firmware queues incoming commands. A `STATUS` poll therefore does not
overwrite a button press, and a normal command can stop an active movement or
show promptly.

## Telemetry format

`STATUS` sends a space-separated line such as:

```text
H:60 E:70 B:0 Bond:0 pets:12 age:4 act:zoomies!
```

The fields are happiness (`H`), energy (`E`), boredom (`B`), bonding (`Bond`),
lifetime pets (`pets`), boot age (`age`), and the current activity (`act`). The
activity value may contain punctuation, so consumers should split the first
six key/value fields and treat the remainder after `act:` as display text.

## Reconnection behavior

The iOS app clears its characteristic references after a disconnect, rescans
when Bluetooth is available, and requests fresh status after reconnecting.
Firmware changes should preserve the advertised name, service UUID, and
characteristic UUIDs unless the client is updated at the same time.
