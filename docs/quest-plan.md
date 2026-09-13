# Quest plan — native, no game engine

Decision (2026-09-04): the headset app is native C++ on the Android NDK,
OpenXR with `XR_EXT_hand_tracking`, Vulkan or GLES for the picture, Oboe for
audio. No Unity, no Unreal. The synthesizer core is used unchanged.

## What is already true

* `Core/` builds for `arm64-v8a` with NDK r27c (clang), including the OSC
  server (BSD sockets), the gesture layer, the FFT and the whole engine:
  ```
  cmake -S . -B build-android -G "Unix Makefiles" ^
    -DCMAKE_MAKE_PROGRAM=%NDK%\prebuilt\windows-x86_64\bin\make.exe ^
    -DCMAKE_TOOLCHAIN_FILE=%NDK%\build\cmake\android.toolchain.cmake ^
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 ^
    -DCMAKE_BUILD_TYPE=Release -DAMBIENT_BUILD_PLUGIN=OFF
  cmake --build build-android
  ```
  produces `libNoctuaryCore.a`, `ambient_render` and `ambient_selftest` for the
  device (the two executables can be run through `adb shell` for a device-side
  measurement pass).
* The gesture layer (`ambient/Gesture.h`) is the mapping from hand quantities
  to parameters: distance, heights, reach, palm tilt, pinch, head angles →
  parameter with range, smoothing, dead-zone and a clutch (right pinch by
  default). It is fed on the desktop by OSC and on the Quest directly from
  the hand-tracking joints. Same code, same mappings text.
* The morph is one scalar owned by the hand distance; the whole instrument
  moves with it.

## Boilerplate to start from

Meta's OpenXR mobile SDK ships native samples (`XrSamples`), in particular
the hand-tracking sample: Android `NativeActivity`, GLES/Vulkan context,
OpenXR instance/session/space handling, frame loop, and the parsing of the
26 hand joints per hand from `xrLocateHandJointsEXT`. That sample is the
skeleton; the plan is to keep its structure and replace the demo content:

1. **Audio**: an Oboe stream (low-latency, 48 kHz, 2 ch) whose callback calls
   `Engine::process(L, R, n)`. The engine's atomics are the control surface;
   nothing else touches the audio thread. Notes/presets go through the
   `EventQueue` exactly as in the plugin.
2. **Hands → gestures**: per frame, from the joints: palm position (metres,
   stage space), pinch strength (thumb tip to index tip distance, or the
   `XR_FB_hand_tracking_aim` pinch strength), palm roll from the palm
   orientation. Feed `GestureLayer::setHand(hand, x, y, z, pinch, tilt)` and
   `setHead(yaw, pitch, roll)` from the view pose; call
   `GestureLayer::update(dt, engine.setParam)` once per frame.
3. **Picture** (synaesthetic, calm — the Kaleidoscope rules apply: no camera
   shake, everything continuous): the engine's observers give sounding notes
   with their distance (`noteDistance`), the brain's root, the arc, the morph
   position; the far plane is literally far away in the scene, near notes are
   close and bright. Hands are drawn as the instrument's handles; the clutch
   state is visible. First version: point sprites / soft discs on GLES, later
   Vulkan.
4. **Presets and state**: the 148 + 32 presets are compiled in; morph slots
   A/B chosen by a menu on the wrist or by voice later.
5. **Bridge mode**: the same app can send its hand data as OSC to a PC
   (`/ambient/hand/L|R`, `/ambient/head`) so the desktop plugin can be played
   from the headset before the on-device audio is finished.

## Testing without the headset

`Tools/osc_hand_sim.py` streams a slow choreography of two hands and a
pinching clutch to the desktop synth over OSC; the header shows message count
and clutch state. The self test covers the OSC parser, the dispatch table and
the gesture mapping (clutch, dead-zone, smoothing, text round-trip).

## Status 2026-09-04: the app exists and packages

`Quest/` holds the native app (see `Quest/README.md`): `libambientquest.so`
builds with NDK r27c against the Khronos OpenXR loader 1.1.63 (prefab AAR
from Maven Central, since the Meta SDK zip no longer ships headers or
loader) and Oboe from source; `build_apk.ps1` produces a signed
`NoctuaryQuest.apk` without Gradle (aapt2 → jar → zipalign → apksigner).
Steps 1, 2, 3 and 5 of the plan above are implemented in `src/main.cpp`.
Not yet run on a headset.

Since then (same day): calibration gesture (8 s on first start, saved to
`calib.txt`), the hand menu (left pinch opens, right height chooses, right
pinch activates: morph, A/B capture, A/B preset prev/next, record,
calibrate) with a 5×7 dot font on a head-locked panel, on-device WAV
recording through the core's lock-free `WavRecorder`, per-note envelope
levels in the picture, and `ambient_render --bench` for a device
measurement (`adb push build-android/Tools/render/ambient_render
/data/local/tmp/ && adb shell /data/local/tmp/ambient_render --bench`).

PC reference for that measurement (i9-12900K, one core, 48 kHz, 256-sample
blocks, chord held plus brain): median 39× realtime with the rotating-phasor
partial bank (22× before), slowest preset 16.5× (Slow Chorus Field: six
strands). The effect chain alone is 1 % of a core. A Quest 2 core is
roughly a fifth of that, so even the heavy presets should land above 3× —
comfortable; the first on-device bench confirms it.

## Open

* First run on a device: session state flow, swapchain format, hand-tracking
  permission prompt, Oboe stream start, `adb logcat -s Noctuary`.
* Pinch thresholds (15–50 mm thumb-to-index) may need tuning per hand size.
* Visual design of the scene beyond the first soft-point version; the
  Kaleidoscope rules apply (no camera motion driven by audio, everything
  continuous).
* Passthrough option; a wrist anchor for the menu instead of head-locked.
