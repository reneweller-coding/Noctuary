# Noctuary for Meta Quest

Native OpenXR app, no game engine: `NativeActivity` + `android_native_app_glue`,
EGL/GLES 3, the Khronos OpenXR loader, `XR_EXT_hand_tracking`, Oboe for audio,
and the unchanged synthesizer core from `../Core`.

```
Quest/
  CMakeLists.txt        NDK build of libambientquest.so (links NoctuaryCore, oboe, openxr_loader)
  AndroidManifest.xml   NativeActivity, hasCode=false, hand-tracking permission/features, VR category
  src/main.cpp          the app: OpenXR session, hands -> GestureLayer -> Engine, Oboe, GLES scene, OSC bridge
  fetch_thirdparty.ps1  downloads the OpenXR loader (prefab AAR) and Oboe into ../ThirdParty
  build_apk.ps1         CMake/NDK -> aapt2 -> jar -> zipalign -> apksigner (debug key)
```

## Build

```
powershell -File Quest\fetch_thirdparty.ps1
powershell -File Quest\build_apk.ps1
adb install -r build-quest\NoctuaryQuest.apk
```

Needs: NDK r27 (`C:\Android-Buildtools\sdk\ndk\27.2.12479018`), build-tools 34,
platform android-34, JDK 17 — see the parameters at the top of `build_apk.ps1`.

## Config (optional)

`adb push ambient.cfg /sdcard/Android/data/com.reneweller.noctuary.quest/files/ambient.cfg`

```
osc_host=192.168.1.20     # bridge mode: stream hands/head as OSC to the desktop plugin
osc_port=9000
audio=1                   # 0 = no on-device audio (pure bridge)
preset=Sleep Concert      # a full preset by name
```

## Preset packs (optional)

`*.ambientpack` files in a `Packs` subfolder of the app's data folder are loaded
at start and added to the preset list, each pack as its own family. A pack
preset may name its own sample and wavetable; those are loaded when it is
applied and take precedence over the folder's `texture.wav` and `wavetable.wav`.

```
adb push Library/Packs      /sdcard/Android/data/com.reneweller.noctuary.quest/files/Packs
adb push Library/Textures   /sdcard/Android/data/com.reneweller.noctuary.quest/files/Textures
adb push Library/Wavetables /sdcard/Android/data/com.reneweller.noctuary.quest/files/Wavetables
```

The whole library is about 8 GB, so pushing one or two packs and only the
samples they name is usually the better idea.

## What it does

* Every frame: hand joints → palm position, pinch (thumb tip to index tip),
  palm roll → `GestureLayer::setHand`; head pose → `setHead`; then
  `GestureLayer::update` writes parameters into the engine. Right pinch is
  the clutch, hand distance is the morph, heights are depth and brightness,
  tilts are Cosmos send and far level (defaults from the core).
* Audio: Oboe low-latency float stream, `Engine::process` in the callback.
* Picture: soft points. Sounding notes sit around the listener by pitch
  class, octave as height, distance as radius (near warm, far blue); the
  brain's root is orange on the floor; hands are green, red while pinching,
  with a dotted bridge between them that fills up with the morph position.
* Bridge: with `osc_host` set, the same hand/head data goes out as OSC so the
  desktop plugin can be played from the headset.
* Hand menu (`ambient/Menu.h`): hold the **left** pinch to open a head-locked
  panel (text drawn as dots with a 5×7 font), choose with the **right** hand's
  height, activate with the **right** pinch. Items: morph on/off, A = now,
  B = now, A/B previous/next preset (the names show on the panel), record
  start/stop, calibrate. While the menu is open the clutched mappings hold.
* Calibration: on first start (no `calib.txt`) and from the menu, 8 seconds of
  "hands together and apart, low and high, near and far"; the ranges are
  saved to `calib.txt` in the app's external data folder.
* Recording: `rec-YYYYMMDD-HHMMSS.wav` (32-bit float, the stream's rate) in the
  same folder, written by a background thread from a lock-free ring.
* Notes glow with their envelope level (`Engine::noteLevel`).

## Checks on the headset

The convolution room has a NEON path that the desktop tests only run through an
x86 stand-in for the intrinsics (`Tests/neonshim`). The arm64 build of
`ambient_convtest` runs the real one; `ambient_convbench` measures what a Room
impulse of 4 to 60 s costs on the XR2, which is what `setRoomMaxSeconds(4)` in
`src/main.cpp` should be set from:

```
cmake -S . -B build-android
cmake --build build-android --target ambient_convtest ambient_convbench -- -j6
adb push build-android\Tests\ambient_convtest build-android\Tests\ambient_convbench /data/local/tmp/
adb shell chmod +x /data/local/tmp/ambient_convtest /data/local/tmp/ambient_convbench
adb shell /data/local/tmp/ambient_convtest
adb shell /data/local/tmp/ambient_convbench 256 20 60
```

`ambient_convtest` must print `convolver path: neon` and `convtest: all checks passed`.
(`cmake -S . -B build-android` again after test targets were added, or the build stops
at the first one it does not know.)

## Status

Compiles and packages; not yet run on a device (no headset attached to the
build machine). First on-device checks: session state flow, swapchain format,
hand-tracking permission prompt, Oboe stream start, pinch calibration, and the
two convolver runs above.
