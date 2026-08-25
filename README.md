# Albert — a pocket robot with feelings

Albert is a four-legged desk robot built on an ESP32-C3. He has a face on a
little OLED screen, a touch pad on his head, a memory that survives power-off,
and a personality that drifts between happy, bored, sleepy and properly grumpy
depending on how you treat him. You drive him from an iPhone app over
Bluetooth, or just leave him alone and let him live his own life in auto mode.

This repository holds both halves of him: the firmware that runs on the robot
and the SwiftUI app that talks to it.

| Path | What it is |
|---|---|
| `AlbertMicro_ESP32C3_v5/AlbertMicro_ESP32C3_v5.ino` | The full firmware — one sketch, organised into classes |
| `INTERFACEALBERT/ContentView.swift` | App entry point, Bluetooth stack and telemetry parsing |
| `INTERFACEALBERT/INTERFACEALBERTApp.swift` | The whole user interface: animated face, dashboard, controller page |
| `INTERFACEALBERT.xcodeproj` | Xcode project for the iOS app |

Heads up: the two Swift file names are historically swapped — the *App* file
holds the UI and the *ContentView* file holds the app entry and BLE code.
It works fine; just don't let the names mislead you.

## Hardware

| Part | Details |
|---|---|
| Brain | ESP32-C3 (any dev board with USB) |
| Screen | SSD1306 OLED, 128 x 64, I2C address `0x3C` |
| Legs | 4 hobby servos (SG90 class), one per leg |
| Touch | TTP223 capacitive pad, active high |
| Power | A supply that can take four servos stalling at once — this matters, see the power section |

### Wiring

| Signal | GPIO |
|---|---|
| OLED SDA | 6 |
| OLED SCL | 4 |
| Front-right servo (FR) | 0 |
| Back-right servo (BR) | 1 |
| Front-left servo (FL) | 2 |
| Back-left servo (BL) | 3 |
| TTP223 out | 10 |

Two things about the legs that will save you a debugging afternoon:

1. **The left and right servos are mirrored.** The FR horn faces forward, the
   BR horn faces backward, and the left side mirrors the right. Because of the
   mirror, the two sides can never receive the same angle for the same motion —
   the code always gives left legs `CENTER - x` where right legs get
   `CENTER + x`. If a leg curls the wrong way, check the pin mapping first,
   not the math.
2. **Poses never press against the servo hard stop.** Fully stretched poses go
   to ±80° from center, not ±90°. A servo pushed into its physical stop stalls,
   draws a current spike, and the whole robot brown-out resets.

## Getting started

### Flash the firmware

Libraries you need installed in Arduino: `ESP32Servo`, `Adafruit GFX`,
`Adafruit SSD1306`, and `FluxGarage RoboEyes`. Then:

```
arduino-cli compile --fqbn esp32:esp32:esp32c3 AlbertMicro_ESP32C3_v5
arduino-cli upload  --fqbn esp32:esp32:esp32c3 -p /dev/cu.usbmodemXXXX AlbertMicro_ESP32C3_v5
```

(Or just open the sketch in the Arduino IDE and hit upload.)

### Build the app

Open `INTERFACEALBERT.xcodeproj` in Xcode, pick your iPhone, run. The app needs
Bluetooth permission — accept the prompt on first launch. Tap **Scan & Connect**
and Albert (advertising as `AlbertMini`) will be found within a second or two.

## The app

The app has three pages, switched with the tab bar at the bottom. If the
Bluetooth link drops (robot rebooted, walked out of range), the app quietly
rescans and reconnects by itself.

For the cross-platform wire contract, see [docs/ble-protocol.md](docs/ble-protocol.md).

### Albert tab — the dashboard

This is the "living with Albert" page. The big card is his face, mirrored live
from the robot: it blinks, breathes, glances around, and switches expression
based on the mood telemetry the robot reports every three seconds. When he is
grumpy you get the anger mark in the top-left corner of the card, same as on
his OLED. Tap the face to pet him. The header shows his mood and what he is
currently doing ("Happy · ZOOMIES!"), and the mood card shows the last thing
he said over Bluetooth.

Below the face:

| Section | What you get |
|---|---|
| Mood bars | Live happiness, energy and bonding, plus lifetime pets, age, and his last message |
| Autopilot | Free Roam hands control to his brain; or pick one of seven activity chips (Chill, Explore, Play, Cuddle, Groom, Zoomies, Show off) to lock him into that activity. The active one is highlighted live. |
| Shows | The four big performances: INTRO, 5G GACOR, TEAM, PARTY — the words flash on the app face in sync with his OLED |
| Move | A d-pad for walking, turning, spinning, and stop |

### Actions tab — the command library

Everything else lives here, organised by a category switch: Poses, Tricks,
Dances, Feelings, System. Each category is colour-coded, and a compact live
face sits at the top so Feelings buttons give instant visual feedback. System
holds auto, sleep, stats, save, reset (with a confirmation — it wipes his
memory) and disconnect.

### Control tab — the controller

This page turns your phone into a game controller, PlayStation style, for
driving Albert by hand.

| Control | How it works |
|---|---|
| Analog stick (left) | Drag in any direction. Up walks forward, down walks backward, left and right turn. Release and it snaps back to center and sends STOP. There is a dead zone in the middle so a resting thumb doesn't twitch him around. |
| Emotion diamond (right) | Four round buttons in the classic diamond layout. Each press fires an expression instantly. |
| Emotion switch | The segmented control under the deck swaps the diamond between three banks — see below. |
| SPIN L / SPIN R | Shoulder buttons. Hold to spin in place, release to stop. |
| STAND / SIT / STOP | Utility row at the bottom for quick recovery. |

The three emotion banks:

| Bank | Top | Right | Bottom | Left |
|---|---|---|---|---|
| Sunny | HAPPY | LOVE | CUTE | LAUGH |
| Grumpy | GRUMPY | ANGRY | FURIOUS | POUT |
| Drama | SURPRISED | SAD | SLEEPY | SUS |

The stick only sends a command when you cross into a new direction zone, not
continuously — so the Bluetooth link stays quiet and the robot stays responsive.

## Talking to Albert directly

Everything the app does goes through plain-text commands, so you can also drive
him from the Arduino serial monitor or any BLE UART app. Commands are
case-insensitive.

### Movement

| Command | Action |
|---|---|
| `WALK` | Walk forward (continuous) |
| `BACK` | Walk backward (continuous, and genuinely backward — the gait wave runs in reverse) |
| `LEFT` / `RIGHT` | Turn while walking |
| `SL` / `SR` | Spin left / right in place |
| `STOP` | Stop and stand |

### Poses

| Command | Action |
|---|---|
| `UP` | Stand at neutral |
| `SIT` | Sit back |
| `LIE` | Lie down (moderate stretch) |
| `LAYDOWN` | Fully flat — front legs all the way forward, back legs all the way back, staged pair by pair to keep current draw sane |
| `REST` | Sploot and relax |
| `SLEEP` | Nap mode — he recovers energy while asleep |

### Tricks

`PUSHUPS` `SWING` `GALLOP` `BOUNCE` `STRETCH` `WAVE` `SHAKE` `WIGGLE` `BOW`
`BEG` `SPINPOSE` `YES` `NO` `SAD` `SCARED` `PLAYDEAD` `SNEEZE` `KISS`
`ZOOMIES` `TANTRUM` `SULK` `SHY`

A few favourites:

| Command | What he does |
|---|---|
| `GALLOP` | Checks left and right, then three rounds of bounds that get faster and bigger, finishing with a spring-loaded leap |
| `PLAYDEAD` | Wobbles dramatically, flops completely flat, lies still, then pops back up delighted with himself |
| `TANTRUM` | Furious stomping, then throws himself on the floor in a huff |
| `SULK` | Turns his back on you and side-eyes over his shoulder until you feel bad |
| `ZOOMIES` | The cat thing. Sprints, spins, sprints again, ends up confused |

### Dances

`DANCE` `MOONWALK` `CHACHA` `HEADBANG` `STOMP` `SALUTE` `PEEKABOO` `POUNCE`
`TWIST` `BREAKDANCE` `ROBOT` `DISCO` `BALLET` `PARTY`

`PARTY` is the medley: it announces each dance by name on the OLED, runs all
of them back to back, and closes with a flashing YEAY.

### Text shows

| Command | The performance |
|---|---|
| `INTRO` | HI! — I'M — ALBERTO on screen, a bow of words, then a wave |
| `GACOR` | Eye intro, a spin, then 5G / GACOR! flashing while he bounces |
| `TEAM` | Roll call — GUNG, MAYUN, GABBY, ABUI, TIO — three times with a cha-cha step per name |
| `DEMO` | A guided tour of every facial expression |

While text is on screen the eyes are switched off completely (a dedicated text
state), so the words never fight with the face. The face comes back when the
show ends.

### Faces

`HAPPY` `ANGRY` `GRUMPY` `FURIOUS` `POUT` `TIRED` `SLEEPY` `SURPRISED` `SUS`
`CUTE` `LOVE` `NERVOUS` `CYCLOPS` `LAUGH` `CONFUSED` `SCAN` `WINK` `FACE`

GRUMPY gives the half-lidded sideways glare with the anger mark in the top-left
of the screen. FURIOUS is the escalation: wide angry stare, anger marks
throbbing in both corners.

### Gaze and system

| Command | Action |
|---|---|
| `LOOKUP` `LOOKDOWN` `LOOKLEFT` `LOOKRIGHT` | Point the eyes |
| `AUTO` | Hand control back to his own brain (free roam) |
| `AUTO CHILL/EXPLORE/PLAY/SEEK/GROOM/ZOOM/SHOW` | Auto mode locked to one activity |
| `PET` | Same as petting the touch pad |
| `STATUS` | One-line telemetry: happiness, energy, boredom, bonding, pets, age, current activity |
| `STATS` | Draw the stats screen on his OLED |
| `SAVE` | Save his brain to flash right now |
| `RESET` | Wipe the brain: happiness, bonding, pet counts — everything |

`STATUS` is special: the app polls it every three seconds, so the firmware
answers it inline without interrupting whatever Albert is doing. Every other
command aborts the current move instantly — that's what makes him feel
responsive.

## How his brain works

Four numbers, saved to flash every 30 seconds and across reboots:

| Stat | Goes up when | Goes down when |
|---|---|---|
| Happiness | Petting, tricks, attention | Poking, boredom, neglect |
| Energy | Sleeping | Everything else, especially dancing |
| Boredom | Time passing with no interaction | Any interaction |
| Bonding | Petting, long petting especially | Never — he doesn't forget you |

His resting face follows a mood ladder, checked top to bottom:

| Condition | Face |
|---|---|
| Energy below 25 | Sleepy |
| Happiness above 75 | Happy |
| Happiness below 18 | Grumpy, anger mark and all |
| Happiness below 32 | Pouting, with a tear |
| Boredom above 70 | Suspicious |
| Otherwise | Calm and normal |

Poking him when he is already unhappy escalates: first a head-shake, then
sulking, and below 18 happiness you get a full tantrum. You were warned.

### Auto mode

In `AUTO` he picks an activity, lives in it for 8 to 30 seconds, then picks
again. Selection is weighted by mood, but 30% of the time he ignores the
weights entirely and does something random — so he never falls into a
predictable loop.

| Activity | Flavour |
|---|---|
| Chill | Scanning around, slow blinks, occasional sneeze or shy moment |
| Explore | Wanders and looks around |
| Play | Anything from a twist to a full breakdance — sixteen options |
| Seek attention | Walks up, waves, begs, blows a kiss, or guilt-trips you with a sulk |
| Groom | Stretches and shakes |
| Zoomies | Sudden energy bursts, sprints and gallops |
| Show-off | Performs: disco, ballet, robot dance, pushups, a bow |

Between actions he micro-idles — weight shifts, glances, blinks, and brief
random mood flashes — so he never freezes into a statue.

### The touch pad

| Gesture | Reaction |
|---|---|
| 1 tap | Perks up and pays attention |
| 2 taps | A random trick from a pool of thirteen |
| 3 taps | Nap time |
| Hold about a second | A pet: hearts, happiness up, bonding up |
| Long hold | Full happy dance |

## Power and reliability notes

Servos are the power hogs here, and a sagging rail resets the ESP32 mid-move.
The firmware defends itself in layers:

| Defence | Detail |
|---|---|
| Soft stretch limit | Extreme poses stop at ±80°, off the mechanical stop |
| Staged big moves | LAYDOWN moves the front pair first, then the back pair |
| Brown-out safe mode | If the chip detects the last reset was a brown-out, it boots quietly: short settle, nervous face, no auto mode, and it sends `brownout: safe mode` over BLE. Any command wakes him back to normal. |
| Display budgeting | Screen pushes are capped at 30 fps (15 fps when an emote overlay is active) and the I2C bus runs at 1 MHz, so the display never starves the servo loop |
| Command queue | BLE commands land in a FIFO — nothing gets lost or overwritten, even if the STATUS poll arrives in the same instant as your button press |

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| A leg moves the wrong way or curls inward | Pin mapping. The wiring order is FR=0, BR=1, FL=2, BL=3 — check `SERVO_PINS` before touching the pose math |
| Robot resets when doing big poses | Power supply can't handle the stall current. Check the battery, and confirm poses use `STRETCH`, not raw ±90 |
| Shows play on the app but not the OLED | You are running old firmware — re-flash. The current build queues commands and repaints banners continuously |
| Everything feels sluggish | Also old firmware: the display throttle and the STATUS special-case are the fix |
| He walks forward when told to go back | Old firmware again — reversing was rebuilt to run the gait wave backward in time |
| He is grumpy all the time | That one is on you. Pet him. |

## A note on the name

He introduces himself as ALBERT, does a roll call for GUNG, MAYUN, GABBY, ABUI
and TIO, and firmly believes 5G is gacor. None of this is configurable without
editing the sketch, which is exactly how it should be.
