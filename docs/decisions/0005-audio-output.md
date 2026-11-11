# 0005 — Audio output: miniaudio, a synthesizer of our own, a null output

**Status:** accepted on 2026-11-10, when the maintainer agreed to miniaudio "as long as it serves the work". To be revisited if it stops building on a platform resend supports, or if a device it opens misbehaves in a way its options do not fix.

**Decision:** resend makes its sounds itself, as numbers (`src/audio/synth.cpp`), and hands them to [miniaudio](https://miniaud.io) 0.11.25 by David Reid, which plays them on the default output device. miniaudio is one header, in the public domain or under MIT No Attribution, at the user's choice. It is downloaded at the first configure and checked against its SHA-256, as the Slint sources are. When there is no device, no miniaudio, or `--silent`, the same samples go nowhere and resend behaves the same. This closes G5.1 of the [roadmap](../roadmap.md#g5--sound-3555-h).

## The problem

Slint plays no sound. resend needs very little: one mono stream of short synthesized notes, started within a frame of the event they mark, on Linux and Windows, from a program that must still start on a machine with no sound card (a CI container, a lab computer with audio disabled, a remote desktop). It must not change what is simulated, and the tests must be able to check what would be heard without a device.

## What was considered

- **The platforms' own APIs:** WASAPI on Windows; ALSA, PulseAudio or PipeWire on Linux. No dependency, but three back ends to write and keep, and the choice on Linux depends on the desktop.
- **SDL 3's audio:** good and portable, but a large library to build or ship for one stream, alongside a toolkit that already owns the window and the events.
- **PortAudio, RtAudio, cubeb:** each a library to build and link. RtAudio and PortAudio link to the back ends at build time, so the packages would depend on `libasound` or `libpulse` being installed; cubeb brings Mozilla's build and more than resend uses.
- **miniaudio:** one C file, built into the program. It finds ALSA, PulseAudio or JACK on Linux and WASAPI, DirectSound or WinMM on Windows at run time, so neither the build nor the packages depend on them, and it reports a missing device as an error, not a crash. Its decoders, encoders, mixer graph and engine are not needed and are compiled out (`src/audio/miniaudio.cpp`).

## What was decided

1. **miniaudio 0.11.25, pinned.** Version 0.11.26 came out on 2026-10-31, ten days before this decision; 0.11.25 (2026-03-03) had been out long enough to trust. `CMakeLists.txt` downloads `miniaudio.h` and its `LICENSE` from the release's tag into `.deps/` when they are missing, and stops if either file's SHA-256 is not the one recorded. Offline at the first configure, the build carries on without sound and says so; `-DRESEND_AUDIO=OFF` does the same on purpose.
2. **Our own synthesizer.** Square, triangle and noise voices with an 8 ms attack, an exponential decay and a slide, eight at once, the oldest giving way; 48 kHz, mono. A sound is a few rows of numbers (`src/audio/sounds.cpp`): there are no audio files to ship, license or load, and the sounds can be read and tested.
3. **The mix never clips.** Voices are summed at a fixed level and pass through a limiter that is linear up to 0.8 and bends softly towards 1 beyond it, so eight voices at full volume stay below full scale.
4. **The cues know nothing of audio.** `src/app/cues.cpp` turns records into cues and merges them; it is part of `resend_app` and is tested like the effects. Only `src/app_sound.cpp` meets the synthesizer.
5. **A null output.** The tests, the snapshots, `--silent` and a machine without a device get an output that plays nothing; the interface still logs what it would have played, which is what the GUI tests check. The About panel says which output is in use.
6. **The device runs on its own thread.** miniaudio calls back from its thread for samples; the synthesizer takes a lock for the few microseconds a block takes. The interface never waits for the device.

## What it costs

- About 95 000 lines of C compiled into the program, without warnings checked (it is not resend's code), and about 350 KB more in the binary.
- The first configure needs the network, once, as the Slint sources do with `RESEND_FETCH_SLINT`.
- The packages carry miniaudio's license text in `share/doc/resend/miniaudio/`, and THIRD-PARTY-NOTICES.md names it.
