# 0003 — Visual direction: a pixel world, crisp data, three themes

**Status:** accepted on 2026-11-10, on a still mock-up of the lab screen and on the prototype in `design/motion-prototype/`. To be revisited if the first screens rebuilt in Slint (G2) do not read and move as the prototype does.

**Decision:** resend becomes a pixel-art world with a HUD around it, in three themes, with motion and sound for everything the engine reports. Whatever is read at length stays in smooth type. This closes G1.1 of the [roadmap](../roadmap.md#look-feel-and-play-g1g7); the rules it must keep are listed there.

## What was shown

- **A still mock-up** of one screen: the first network as a lab in the middle of a ping, with the goals, the build bar, the action bar, the inspector with its tables and the timeline.
- **A prototype that moves,** one HTML page built by `build.py` from `template.html`, `palettes.json` and the sprites that `sprites.py` draws. It plays one ping across a router from a list of records, 10 ms a hop, as a function of the instant shown: the timeline can be dragged both ways and shows the same frame for the same instant. It has the effects at three intensities, the cues from a small synthesizer, the candidate palettes side by side, and a terminal in the inspector.

The prototype is a sketch of the look and the feel. None of its code goes into the application.

## What was decided

1. **Pixels for the world, smooth type for data.** The canvas, the frames of the HUD, the icons and the short labels are pixel art at one scale (one art pixel is three screen pixels in the prototype). Tables, addresses, the terminal and the explanations use a smooth font at 12 px or more. The mock-ups use Pixelify Sans, Inter and JetBrains Mono, all under the OFL; G1.3 decides what ships.
2. **Three themes, one set of drawings.** Blueberry is the default, Bubblegum and Picnic (light) can be chosen. Today's colours are retired. A sprite is a grid of roles, not of colours (28 roles for the 21 sprites of the prototype), so a theme is a table of colours and nothing is drawn twice.
3. **Blueberry uses the colours of Sweetie 16 by GrafxKid,** fifteen of the sixteen. Its page on Lospec names the author and states no terms of use; resend credits it in the README and in the About panel.
4. **Motion at "full" is the feel to match:** frames ease along the cables and leave a trail; an Echo bounces, an ARP request carries its ring; a device hops and flashes when a frame arrives; a port sparks when it learns; markers slide in; the reply that reaches its source brings confetti and stamps the goal. Sprites change size in whole steps (1, 2, 4, 3 screen pixels to an art pixel), never smoothly, so pixels stay square; positions are continuous. "Reduced" drops particles, hops and trails; "off" keeps markers and texts only.
5. **Sound is synthesized:** a rising pair of notes for an ARP request and a falling one for its reply, the next note of a pentatonic scale for each hop of the ping, a held low note when a datagram has to wait, a resolving arpeggio when the reply comes home. No audio files and no music.
6. **The terminal speaks the Linux tools** (`ip`, `ping`, `bridge`) and reads the same state as the tables. Commands can be typed, with a history, or picked from a menu for each family of commands that says what each one does.
7. **No points, levels, streaks or timers.** Progress is what was seen and solved.

## What the prototype does not prove

- It draws on an HTML canvas, not in Slint. Its effects use only moves, whole-number scales, opacity and rectangles, and the Slint 1.18.1 features the plan relies on were checked by compiling a test file (`image-rendering: pixelated`, `source-clip-*`, nine-slice images, `animation-tick()`, `animate` with easing, imported fonts). Frame rates in Slint are not measured.
- It plays one scripted ping. The terminal knows six commands, and only PC-A can ping.
- Sound in the application needs an output library (miniaudio, G5.1), not yet tried.
- The themes hold the colours the prototype needs: the roles for errors and for the simulator stepping in are still to be added, within 32 colours each (48 since G1, below). Every pair of text and surface the prototype uses has a contrast of at least 4.5:1; one pair it does not use, the switch's blue as text on a Blueberry panel, has 4.2:1.
- Not tried: a screen reader, the themes under colour-blindness, a second person.

## Changed while building G1 and G2

- **48 colours a theme, not 32.** The tokens became thirteen surfaces, four inks and eight families of four tones (metal, accent, ok, link, ask, route, drop, guard): 49 names, which share colours but need more than 32 distinct ones. The look tests hold each theme to 48 and check the contrast of every pair of text and surface in use.
- **Blueberry is based on Sweetie 16,** no longer made of it only: it uses all sixteen of its colours, and since Sweetie has no four-tone ramp for the link, drop and guard families nor a tone for the grid and the shadow, it adds ten (`#10111c`, `#29304a`, `#3b4f99`, `#62b3ff`, `#c9e6ff`, `#ff8f99`, `#ffe0e3`, `#8e4fb8`, `#d6a6ff`, `#f3e6ff`), 26 in all. The credit says "based on".
- **No image files.** Sprites are written as text grids of roles and painted for the theme when it is chosen; frames are rectangles with notched corners instead of nine-slice images (roadmap G1.4).
- **Square pixels on the canvas** come in whole steps of physical pixels; below one, the 16 px small set is drawn (G1.5).
- **The pixel font for titles only** (2026-11-11). Decision 1 put the short labels in Pixelify Sans. Rendered by Slint at every size from 11 to 22 px, regular and semibold, its C reads as an O, its 7 as a 1 and its 2 as an 8: "Check" as "Oheck", "ICMP" as "IOMP", "PC-A" as "PO-A", port 2 as port 8. It is the drawing, not the size: the font's pixel is 1/11 em, but its side bearings and its semibold strokes are off that grid, so no size is sharp. Titles keep it, at 22 px (`Type.pixel-title`); buttons, tabs, chips and captions are in Inter semibold (`Type.ui`), and whatever the network holds, a device's name, a port's number, an event, is in Inter or JetBrains Mono. A look test holds the pixel font to the title size. A pixel font drawn wholly on its grid, with letters that differ, could take the labels back: one token.
- **Fonts on a bare machine.** The fonts are built into the program and the snapshot is the same with no font installed, but fontique 0.11.1, under Slint 1.18.1, stops the program when fontconfig knows no font at all; resend therefore adds the directory of its installed fonts to fontconfig's configuration at start (`src/main.cpp`).
