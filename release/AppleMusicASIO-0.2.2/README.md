# AppleMusicASIO

ASIO / WASAPI-exclusive audio bridge for the Apple Music desktop app on Windows 11 x64.

The bridge is injected into `AMPLibraryAgent.exe` (the process that actually owns
Apple Music's audio session) and replaces the app's WASAPI render stream with an
ASIO or WASAPI-exclusive sink. Windows shared-mode mixing, the Windows resampler
and audio processing objects (APOs) are bypassed.

## Status

Tested with:

- Windows 11 x64
- Apple Music `1.1540.23042.0` (MSIX, full-trust)
- MiniFuse ASIO driver 1.091
- 44.1, 48, 96 and 192 kHz content, including automatic rate switching
- ASIO and WASAPI exclusive (MiniFuse WDM endpoint: 32-bit container, 24 valid bits)

Current version: `0.2.2`.

## Features

- Injects into the audio process, not the UI process.
- Hooks `IMMDevice::Activate` and the `IAudioClient` vtable; the app sees a normal
  event-driven render client while PCM is handed to ASIO.
- **Source-rate following**: detects Apple's decoded/streaming rate from
  `AudioQueueNewOutput` / `AudioConverterNew` and reports that rate through
  `GetMixFormat`, so Apple does not resample 44.1 kHz content to the endpoint rate.
- ASIO and WASAPI exclusive open at the exact rate the app uses. The bridge never
  resamples.
- Float32 app output is packed to the device's integer format without a rate
  conversion; for 16/24-bit ALAC content this reconstructs the original integer
  samples (the app's float32 decoder output is exactly scaled).
- WASAPI exclusive negotiates the endpoint's supported exclusive format. The
  MiniFuse WDM endpoint accepts 32-bit PCM with 24 valid bits (not float32);
  the bridge converts float32 to that format and uses a dedicated exclusive
  client so a new playback can replace the previous stream without
  `AUDCLNT_E_DEVICE_IN_USE`.
- Hot-reloadable INI configuration.
- Background watcher injects the bridge whenever `AMPLibraryAgent.exe` starts.

## Requirements

- Windows 11 x64.
- Apple Music for Windows (MSIX).
- A working ASIO driver (recommended), or an endpoint that supports WASAPI
  exclusive mode with an integer PCM format at the source rates you play.
  Float32 exclusive support is not required.
- The watcher runs in the current user session; no service installation needed.

## Quick start

1. Build with `build.ps1` (see below), or use a release folder.
2. Double-click `Apple Music ASIO.cmd`.
   It starts the watcher (hidden) and launches Apple Music. The bridge is injected
   as soon as `AMPLibraryAgent.exe` appears.
3. Play a track. Check `AMAudioBridge.log` next to `AMAudioBridge.dll`.

If Apple Music is already running, the watcher will inject into the existing
agent; playback started afterwards is bridged. The watcher keeps running and
handles future agent restarts.

To start the watcher automatically at login, run `install-autostart.ps1` from the
release folder once. Remove the shortcut from `shell:startup` to undo it.

## Configuration

`AMAudioBridge.ini` next to the DLL. Changes are picked up while the bridge is
loaded and apply to the next stream.

```ini
[Bridge]
Mode=auto                 ; auto | asio | exclusive | monitor | off
LogLevel=2                ; 0 error .. 4 trace
StrictBitPerfect=1
VolumeControl=unity       ; unity | app
FollowSourceRate=1        ; report Apple's source rate to the app

[ASIO]
Driver=auto               ; first driver, name, description or CLSID
Channels=0                ; 0 = stereo / driver native
BufferSize=0              ; 0 = driver preferred
SampleRate=0              ; 0 = follow the app

[WASAPI]
BufferMs=20
```

- `Mode=auto` selects ASIO when a registered driver is available, otherwise
  WASAPI exclusive. Set `Mode=exclusive` to force the WASAPI path.
- `FollowSourceRate=1` is what makes 44.1 kHz content stay 44.1 kHz for both
  ASIO and WASAPI exclusive. Set it to `0` if a future Apple Music build
  misbehaves; the app will then use the Windows endpoint mix rate and the bridge
  still bypasses Windows processing.
- `VolumeControl=unity` adds no software gain. Apple applies its own volume
  before handing PCM to the render client, so the Apple Music volume slider still
  works. The device format may still require float32-to-integer packing.
  `VolumeControl=app` additionally applies the Windows stream volume to the
  bridge's float path.
- The bridge never resamples. `StrictBitPerfect` is advisory; integer packing is
  still used when the endpoint has no float32 exclusive format.

## How it works

```
Apple Music UI (AppleMusic.exe)
        |
        v
AMPLibraryAgent.exe
  CoreMedia / CoreAudioToolbox
        |  IAudioClient (hooked)
        v
AMAudioBridge.dll
  fake IAudioRenderClient / IAudioClock / session objects
        |  SPSC ring buffer, byte-exact frames
        v
ASIO driver or WASAPI-exclusive endpoint
```

- `IMMDevice::Activate` installs a patched `IAudioClient` vtable for every new
  client. The app creates one client per playback.
- `Initialize` is the takeover point. The bridge creates a fake stream table,
  opens ASIO or a dedicated WASAPI-exclusive client at the app's exact sample
  rate/channel count, and returns success.
- `GetMixFormat` is deliberately not used to open a device. Opening the device
  from `GetMixFormat` makes Apple's short-lived probe clients retry and
  destabilises playback.
- `FollowSourceRate` hooks `AudioQueueNewOutput` and `AudioConverterNew` in
  `CoreMedia.dll` / `CoreAudioToolbox.dll` to learn the decoded source rate.
  `GetMixFormat` then returns the real endpoint format with that sample rate and
  float32 preserved. The app initializes at that rate; ASIO or WASAPI exclusive
  follows.
- ASIO callbacks and the WASAPI feeder consume the ring, convert/pack to the
  device's sample type, and signal the app's event. No Windows mixer or APO is in
  the path. WASAPI exclusive uses an output client owned by the bridge, so the
  old device handle can be released when a new playback stream replaces it.

## Settings persistence fix

Newer Apple Music builds store preferences through Apple's CFPreferences layer.
When the MSIX package data is redirected to another volume (for example
`D:\WpSystem\...`), the app writes a temporary
`AppleMusic.exe.plist.Xa...` / `AMPLibraryAgent.exe.plist.Xa...` file on that
volume and then tries to commit it to
`%APPDATA%\Apple Computer\Preferences` on `C:`. The cross-volume move fails and
the app deletes the temporary file; the audio-quality setting reverts to AAC on
the next launch.

Two small pieces fix this:

- `fix-preferences-junction.ps1` makes
  `%APPDATA%\Apple Computer\Preferences` a junction to the package's real
  preferences directory, so reads and writes resolve to the same volume.
  `watch-bridge.ps1` runs it automatically at startup.
- `AMPrefsPersist.dll` is injected into `AMPLibraryAgent.exe`. When the app writes
  a temporary `.plist.Xa...` file, the helper copies it to the final `.plist`
  before Apple Music deletes it. This keeps the saved quality even if
  CFPreferences' own rename still fails.

If the quality setting still resets after an Apple Music update or a package
reset, run `fix-preferences-junction.ps1` manually and restart Apple Music once.

## Limitations

- Apple Music's own decoder and any processing before `IAudioClient` remain in
  the path. The bridge removes Windows mixing/resampling, not Apple's decoder.
  With `FollowSourceRate=1`, rate conversion by Apple is avoided for the tested
  paths; the remaining float32-to-integer packing is lossless for 16/24-bit
  content.
- WASAPI exclusive depends on the endpoint accepting the source rate. The
  MiniFuse WDM endpoint accepts integer PCM, not float32, so the bridge performs
  a transparent float32-to-integer packing. This is not a float32 bit-perfect
  path; ASIO is the only path that can expose the driver's native format.
- WASAPI exclusive is held from stream start until the app releases that client.
  While a track is paused, other applications may not be able to take the device
  in exclusive mode; close or switch the Apple Music stream to release it.
- The app's audio-quality setting (AAC vs Lossless/Hi-Res) is controlled by
  Apple Music. The junction plus `AMPrefsPersist.dll` fix described above keeps
  it across launches on the tested layout. The watcher does not kill the app.
- Apple Music updates can change internal modules and break the hooks. Test
  after each app update.
- This project does not bypass DRM or content protection.

## Build

From a Visual Studio x64 developer environment (the script locates VS itself):

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -OutputDir bin\x64\Release
```

Artifacts:

- `AMAudioBridge.dll` – injected bridge
- `AMAudioBridgeInjector.exe` – remote LoadLibrary injector / watcher target
- `AMAudioBridge.ini` – configuration
- `AMAudioProbe.exe` – standalone synthetic render client
- `AMAsioTool.exe` – ASIO enumeration / diagnostics
- `AMCoreAudioProbe.dll` – optional CoreAudio format logger

A loaded DLL is locked, so build into a new directory (for example
`bin\x64\Release\dev7`) while testing.

## Troubleshooting

- `AMAudioBridge.log` – bridge log, next to the DLL.
- `AMAudioBridgeWatch.log` – watcher/injector log.
- Only one watcher can run at a time (named mutex). If you are switching from an older build, stop the old PowerShell watcher first.
- If the bridge is not injected, run the injector manually:
  `AMAudioBridgeInjector.exe --dll AMAudioBridge.dll --pid <agent pid>`.
- If playback fails after an Apple Music update, set `Mode=monitor` (or
  `FollowSourceRate=0`) and restart playback to confirm the normal path.
- Do not force-kill `AMPLibraryAgent.exe` while changing Apple Music settings;
  allow the app to close normally so it can persist its preferences.

## Uninstall

1. Close Apple Music.
2. Remove the `AppleMusicASIO Watcher` shortcut from `shell:startup` if created.
3. Stop the watcher PowerShell process.
4. Delete the folder.

The bridge only lives inside the Apple Music process; closing Apple Music
unloads it. No system audio settings are changed.
