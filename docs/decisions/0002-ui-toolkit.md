# 0002 — Slint as the UI toolkit: hands-on evaluation

**Status:** accepted on 2026-11-07, with the design rules at the end. To be revisited if manual testing or the VM without a GPU contradicts the measurements.

**Decision:** keep Slint 1.18. Every criterion of milestone M1 that can be automated is met with the GPU renderers; the feel of mouse and trackpad, IME input and screen readers still have to be tested by hand.

## What was tested

The prototype lives in `spikes/canvas/` and is built with `-DRESEND_BUILD_SPIKES=ON`:

- `canvas_controller.*`: geometry and interaction in plain C++, without Slint (transforms, ports, cable selection, dragging, port-to-port linking, keyboard, fitting the view);
- `spike.slint` + `spike_app.*`: Slint draws from the models and forwards input; the glue updates only the rows that changed;
- `spike_tests.cpp`: 49 checks, on both the controller and the interface (testing backend, real pointer and keyboard events);
- `bench.sh`: benchmarks with real window events, measuring FPS with `SLINT_DEBUG_PERFORMANCE`.

Machine: Intel i7-9700K, NVIDIA RTX 2070 (proprietary driver), Arch Linux, Wayland with niri, prebuilt Slint 1.18.1. Stress scene: 150 devices (60 eight-port switches, 90 hosts), 250 cables, 20,000 events.

## Results

| M1 criterion | Result |
| --- | --- |
| Smooth dragging with 150 nodes | **Yes with Skia and FemtoVG**: 55–60 fps, i.e. the vsync limit, while dragging, panning, zooming and scrolling. **No with the software renderer**: 15–19 fps (panning and dragging), 31–35 (zooming). |
| A click within ~6 px selects the right cable | Yes: 5 px selects, 9 px does not, even at 30% zoom (the tolerance is in screen pixels). |
| 20,000 events scroll without stutter | Yes: the `ListView` instantiates 26 rows out of 20,000; 2.8 µs per scroll step. |
| Correct hit areas at 1.5× | Yes: input arrives in logical pixels and the controller does not depend on the scale. At 2.0× text and lines are sharp. |
| Accented letters | Yes for keyboard events (`è`, `ü`, `ñ`). IME and dead keys not tested. |
| Context menu | Yes, opened with `ContextMenuArea.show()` (see below); the chosen entry deletes the node with the real backend. |
| Wayland and X11 | Both work; X11 (XWayland) and the software renderer differ by 642 antialiasing pixels out of 967,000. |
| UI rebuild ≤ 15 s | Yes in Debug with `RESEND_SLINT_COMPILATION_UNITS=4` (the default): **10 s**; 23 s in Release. |

Average C++ cost per input event (Slint dispatch plus model synchronization), always below a millisecond:

| Scenario | All cables in one `Path` | One `Path` per cable |
| --- | --- | --- |
| Dragging | 22–34 µs | 22–30 µs |
| Panning | 320–410 µs | 100–140 µs |
| Zooming | 450–770 µs | 75–330 µs |

With a single `Path`, panning rebuilds the string of 250 segments; with one `Path` per cable the cost moves to rendering, and the software renderer drops to **6–7 fps**.

Rebuild times after a real change to `ui/app.slint` (changes to comments alone rebuild nothing: `slint-compiler` does not rewrite unchanged output):

| Units | Debug | Release |
| --- | --- | --- |
| 1 | 15.9 s | 34.6 s |
| 2 | 11.9 s | 26.5 s |
| 4 | 10.3 s | 23.4 s |
| 8 | 9.1 s | 20.2 s |

### Caveats on the measurements

- FPS are capped at 60 by vsync: they show whether the frame rate is sustained, not the headroom.
- niri stops sending frames to windows that are out of view. Some runs dropped to 1 fps in a way that could not be reproduced (the same configuration, repeated, stayed at 55–60). Reliable comparisons need a session where the window stays visible, or a non-tiling compositor.
- One machine only, with a dedicated GPU.

## Problems found and solutions

1. **`TouchArea` and `ContextMenuArea`.** The `TouchArea` accepts the right click, so the menu does not open by itself. Solution: on a right click the controller selects the object and Slint calls `menu-area.show(position)`.
2. **Scrolling the `ListView` to the last row.** Right after a row is added, the content height is not updated yet and scrolling is clamped. Solution in the app: scroll again in `changed content-height`. The testing backend does not reproduce the problem; it needs the real renderer.
3. **Fluent `LineEdit`.** A click in the inner padding near the right edge does not focus the field. To be confirmed by hand; if needed, a `TouchArea` that forwards focus.
4. **Large networks are unreadable.** At 25% zoom the cards of 150 devices cannot be read: semantic zoom is needed (simplified cards below a zoom threshold), which also reduces the work of the software renderer. Done in M4: zoomed out, cards show only their icon and name.
5. **Fixed-width panels** (340 logical px) leave little room for the canvas at a 2.0 scale: relative widths or collapsible panels are needed. Done in M4: the three panels fold away and the window has no fixed minimum size.
6. **A `FocusScope` over a `TouchArea`** (found in M4). A focus scope that does not have the focus takes the press that gives it the focus, so the touch area below never sees it: the first click on the canvas did nothing. Solution: the canvas's focus scope lies under its touch area, which gives it the focus itself.

## Limits of display-less tests

The `slint::testing` backend suits behaviour tests in CI, but:

- it does not produce snapshots;
- it lays out the `ListView` synchronously, so it does not reproduce problem 2;
- it puts menus and popups in a separate window with its own coordinates: a test can check that a menu opens, not pick an entry with the pointer;
- `changed` handlers run with the event loop: tests call `slint::platform::update_timers_and_animations()` to run them, and again after a handler scrolled a `ListView`, whose new rows appear only once it is laid out;
- it needs `SLINT_FEATURE_EXPERIMENTAL` and element debug information (see `CMakeLists.txt`);
- a button and its text share the same accessible label: tests have to filter by role.

## Not verified

The real feel of mouse and trackpad (latency, pinch-zoom), IME and dead keys, screen readers through AT-SPI, DPI changes between monitors, Windows, machines without a dedicated GPU or with Mesa drivers.

## Design rules that follow

1. Geometry, hit testing and interaction state live in a C++ controller with no dependency on Slint, tested on its own; Slint has a single input layer over the canvas.
2. Cables are drawn in a single `Path` in screen coordinates, plus one `Path` for the selection and one for the cable being created.
3. Every input returns the set of things that changed; the glue updates only those model rows.
4. Long lists always use `ListView`.
5. Before version 1.0, test on a VM without a GPU: if the software renderer stays below 30 fps with 150 devices even with semantic zoom, the choice must be reconsidered (Qt 6 remains the alternative).

## How to reproduce

```sh
cmake -S . -B build-spike -DRESEND_BUILD_SPIKES=ON -DSlint_DIR="$(bash scripts/fetch-slint.sh)"
cmake --build build-spike
ctest --test-dir build-spike -R canvas-spike --output-on-failure
bash spikes/canvas/bench.sh build-spike/spikes/canvas/canvas-spike 4
./build-spike/spikes/canvas/canvas-spike            # interactive test
./build-spike/spikes/canvas/canvas-spike --small --menu-pick
```

In the interactive test: drag from a port (the circle below a card) to another one to create a cable, click a cable to select it, Delete to remove, Esc to cancel, the wheel or `+`/`-` to zoom, `f` to fit the view, right click for the menu.
