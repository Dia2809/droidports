# LSWTCS port for droidports

Loader for **Lego Star Wars: The Complete Saga** (Android `libTTapp.so`,
version 2.0.2.02 / 20202), adapted from
[lswtcs-vita](https://github.com/gm666q/lswtcs-vita) into the droidports
framework. Targets `linux-armhf`.

## Layout

| File                  | Role |
|-----------------------|------|
| `main.c`              | Entry point: parses args, brings up SDL2 + GLES2, loads `libTTapp.so` (either standalone or from APK), wires patches + activity, runs `invoke_app()` |
| `libtt.{c,h}`         | TTActivity native-callback table; Android Activity lifecycle driver; main event loop |
| `TTActivity.c`        | JNI class `com/tt/tech/TTActivity` (host-implemented Java side: `FlurryEvent`, `IsMusicActive`, `OpenPrivacyPolicy`, `OpenTermsOfServices`, `getCountryCode`, plus `SDK_INT` / `WINDOW_SERVICE`) |
| `patch.c`             | Game-specific hooks (controller button remap; CPU-affinity hooks intentionally dropped — Linux scheduler handles it) |
| `sdl2_media.c`        | SDL2 window + GLES2 context; pumps input → `nativeOnKeyDown/Up`, touch via mouse, gamepad axes |
| `asset_manager.c`     | Minimal `AAssetManager_*` and `ANativeWindow_*` stubs |
| `io_remap.c`          | Rewrites Android assetpack paths to `$LSWTCS_DATA_PATH/*.dat` |
| `symtable_lswtcs.c`   | Port-local symtable: full GLES2 (auto-generated), EGL stubs, OpenSL ES stubs, ANativeWindow / AAsset wiring, locale / wchar / pthread / misc libc passthroughs |
| `keycodes.h`          | Subset of Android `KeyEvent` codes + internal gamepad slot enum |
| `media.h`             | Display / input interface |
| `port.cmake`          | Bridge selection (`openal_bridge`, `zip_bridge`) |

## Building

Requires an armhf chroot or real device with:

```
libsdl2-dev libopenal-dev libzip-dev libbz2-dev zlib1g-dev libfreetype-dev
```

```sh
mkdir build && cd build
cmake -DPORT=lswtcs -DPLATFORM=linux -DARMHF=ON -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

## Running

The four asset packs (`Audio.dat`, `Levels.dat`, `Others.dat`,
`Textures.dat`) must be extracted from a legally-obtained APK and placed
in `$LSWTCS_DATA_PATH` (default `./lswtcs_data/`). The shared library
itself may be passed directly:

```sh
LSWTCS_DATA_PATH=./lswtcs_data ./lswtcs ./lswtcs_data/libTTapp.so
# or, from an APK:
./lswtcs path/to/lswtcs.apk
```

Optional env vars:

- `LSWTCS_LEGACY_CONTROLS=1` — skip the `NuInputDevicePS::GetGamePadButtonIndex`
  remap (use the stock Android-spec button mapping).

## Known limitations

- **Audio is silent.** `slCreateEngine` returns `FEATURE_UNSUPPORTED`;
  the game falls back to no audio. Wiring real audio means writing an
  OpenSL ES → OpenAL bridge (or pulling the gm666q OpenSL ES fork).
- **qemu-user is not supported.** The loader runs cleanly through the
  full Activity lifecycle under `qemu-arm-static`, but the engine's
  worker threads trip glibc stack-canary checks (likely qemu-user
  translation bug or softfp/hardfp ABI mismatch with the prebuilt
  Android `.so`). Test on real ARMv7 hardware (Raspberry Pi, Pinephone,
  RK3399, any armhf SBC).
- **Asset Manager** wraps `fopen()` directly — no real APK streaming.
  All `.dat` files must be pre-extracted to disk.
- **EGL** is stubbed; the real GL context is provided by SDL2. If
  libTTapp ever inspects EGL display attributes, the dummies in
  `symtable_lswtcs.c` may need to return more realistic values.

## Credits

- [TheFloW](https://github.com/TheOfficialFloW) — original Android `.so` loader for Vita
- [v-atamanenko](https://github.com/v-atamanenko) — `soloader-boilerplate`, FalsoJNI
- [gm666q](https://github.com/gm666q) — `lswtcs-vita` (the reference port this is derived from)
- [JohnnyonFlame](https://github.com/JohnnyonFlame) — droidports framework

## License

GPLv3, matching droidports.
