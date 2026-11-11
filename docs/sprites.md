# Sprites

Everything resend draws in pixels — devices, port lights, frames, the HUD's icons and the app icon — is a sprite in `assets/sprites/`, written as text. This page says how they are written, how they become images, and what a new protocol or device must draw. The reasons for the look are in [decision 0003](decisions/0003-visual-direction.md).

## Sheets

A sheet is a text file of sprites. Each starts with a line `sprite NAME WIDTH HEIGHT`, then has one row of characters per pixel row. Lines that start with `#` between sprites are comments.

```
sprite port 8 8
########
#oooooo#
#oooooo#
#oooooo#
#,,,,,,#
#,#,,#,#
########
........
```

Sizes are multiples of 8 up to 64. A character names a **role**, never a colour: `#` is the outline, `:` a screen, `,` a well (an empty socket), `*` ink, `o` a status light, and the eight families of four tones (darkest to lightest) are `1 2 3 4` metal, `a b c d` ok, `e f g h` link, `i j k l` ask, `m n p q` route, `r s t u` accent, `v w x y` drop and `A B C D` guard. `.` is transparent. The full table is `assets/sprites/legend.txt`; the code (`look::role_of`) is the authority and a test checks that the two agree.

| Sheet | Holds |
|---|---|
| `devices.txt` | host, switch and router at 32 px, the same at 16 px (`-small`), and the port light |
| `packets.txt` | one sprite for each kind of frame |
| `icons.txt` | the HUD's icons at 16 px (some at 8), the logo and the 32 px app icon |

The sheets and `assets/palettes.json` are built into the program (`cmake/embed_assets.cmake`).

## From text to pixels

Each theme gives every role its colour, so one drawing serves the three themes. When a theme is chosen, `src/app_look.cpp` paints each sprite into an image of the `UiSprites` global (`ui/sprites.slint`): one property per icon, and arrays for what has states. Icons drawn in ink are tinted by the interface with `colorize`, to the colour of the text beside them. Images are drawn with `image-rendering: pixelated` at a whole number of physical pixels per art pixel (`look::art_pixel`, `look::canvas_art`), so pixels stay square at every zoom and scale factor.

## States

A sprite with states is drawn once: in each state some roles become others (`look::in_state`).

**Devices** (`look::DeviceState`), in this order:

| State | When | What changes |
|---|---|---|
| off | the device is powered off | every family becomes metal: no colour left |
| on | otherwise | as drawn; the status light is ok d |
| selected | it is the selection | the outline takes the accent |
| sending | it sent a frame in the last 4 ms of simulated time | the light is ok l, a screen ok dd |
| receiving | it received one | the light is link l, a screen link dd |
| error | a frame or a datagram stopped there for a reason that is a problem, or its ping failed, in the last 8 ms | the light is drop base, a screen drop dd |

Off wins over everything, then what the device does, then the selection. Frames that were simply for someone else (not my MAC, not my IP) are no error. The scene computes it (`Scene::device_activity` in `src/app/scene.cpp`); `UiSprites.devices` holds the eighteen images at index kind × 6 + state, `devices-small` the small set.

**Port lights** (`look::PortState`): free (an empty well), connected (link base), down (ask base: the cable or a port is disabled) and active (ok l, while a frame passes).

## Frames

Every kind of frame has a sprite of its own, different in shape as well as in colour: no two are the same once reduced to one colour.

| Kind (`netsim::PduKind`) | Sprite | Shape |
|---|---|---|
| ARP request | `arp-request` | an envelope with a question mark, its tail to the left |
| ARP reply | `arp-reply` | an envelope with an exclamation mark, its tail to the right |
| Echo Request | `echo-request` | a ball |
| Echo Reply | `echo-reply` | a ring |
| Time Exceeded | `time-exceeded` | an hourglass run out |
| Destination Unreachable | `unreachable` | a stop sign |
| other ICMP | `other-icmp` | a diamond |

`resend::packet_sprite` names the sprite of a kind, and `resend::pdu_kinds` lists every kind. Broadcast copies carry a ring; the selected frame shows its label above it.

## What a new protocol or device must draw

- **A new kind of frame:** a value in `netsim::PduKind`, its entry in `resend::pdu_kinds` and `packet_sprite`, and a 16 px sprite in `packets.txt` whose shape no other frame has, in the family of its protocol. The look tests fail until all of this is there.
- **A new kind of device:** a 32 px and a 16 px sprite in `devices.txt` with at least one status light (`o`), its name in the lists of `src/app_look.cpp`, and its index on the canvas. Its six states come from the drawing.
- **A new role of a device**, such as a server: the same states. If it needs one more (a service that is listening, say), add it to `look::DeviceState` and to `look::in_state`; the tests check that every state differs from the others in every theme.
- **A new family of colours:** a name in `look::family_names`, four tones in each theme of `assets/palettes.json` within 48 colours, and four characters in the legend.

## Checks

`tests/look_tests.cpp` reads every sheet and checks sizes, rows, roles and duplicates; that every state of every device and port is a different image in every theme; that every kind of frame has its sprite and that their shapes differ; and that the app icon's files read back. `tests/gui_tests.cpp` checks that every sprite reaches `UiSprites`.
