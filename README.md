# AppleMusicASIO

ASIO or WASAPI-exclusive audio output for Apple Music on Windows 11 x64.

The bridge runs inside `AMPLibraryAgent.exe`, the process that owns Apple Music's
audio session. It replaces the app's shared-mode WASAPI render stream with an
ASIO or WASAPI-exclusive sink, bypassing Windows shared-mode mixing, resampling
and audio processing objects (APOs). Apple's decoder and processing before the
render stream remain in the path. DRM is not bypassed.

## Features

- ASIO or WASAPI exclusive output.
- Source sample rate following: 44.1, 48, 96 and 192 kHz.
- Automatic start with the normal Apple Music shortcut; no service, scheduled
  task, watcher or launcher process.
- Small per-user installation in `%LOCALAPPDATA%\AppleMusicASIO`.
- Static C runtime and no Visual C++ redistributable.
- Simple installer with an ASIO / WASAPI-exclusive mode selector.

Tested with Windows 11 x64, Apple Music `1.1540.23042.0`, a MiniFuse ASIO driver
and both output modes. Other drivers and Apple Music versions are untested.

## Install

1. Close Apple Music.
2. Run `AMAudioBridgeSetup.exe` from the release folder.
3. Select `WASAPI exclusive` or `ASIO`, click `Install`, then start Apple Music.

`Apply` saves a mode change and closes the window. `Close` discards the change.
Restart Apple Music after changing the mode.

The installer copies the bridge, early-launch proxy and preferences helper to
`%LOCALAPPDATA%\AppleMusicASIO`, and registers two per-user COM servers under
`HKCU`. Administrator access is not required and no machine-wide files are
modified.

To uninstall, close Apple Music, run the setup program and click `Uninstall`.
The preferences junction, if created, is kept because it also fixes Apple
Music's own settings persistence.

## How it works

Two per-user COM registrations provide automatic launch:

- The early-launch proxy (`AMEarlyProxy.dll`) is registered for the Windows
  Property System class. It forwards calls to the real `propsys.dll` and, only
  inside `AppleMusic.exe`, starts `AMPLibraryAgent.exe` and injects the audio
  bridge before Apple activates its library.
- `AMAudioBridge.dll` is registered for `CLSID_MMDeviceEnumerator`. It forwards
  device enumeration to `MMDevApi.dll`, then hooks the audio client creation,
  the `IAudioClient` interface and Apple's CoreAudio format functions.
- `AMPrefsPersist.dll` is loaded into the agent and fixes settings persistence
  when Apple's MSIX LocalCache is redirected to another volume.

The bridge never resamples. ASIO opens the driver at the source rate and
channel count. WASAPI exclusive negotiates a format the endpoint accepts; when
float32 is unavailable, the bridge packs it to an integer PCM format.

## Configuration

The setup program stores the output mode here:

```text
HKCU\Software\AppleMusicASIO\Mode
```

Values: `wasapi`, `asio`, `auto`, `monitor`, `off`. The registry value
overrides `Mode` in `AMAudioBridge.ini`. Other settings remain in the INI next
to the installed DLL:

```ini
[Bridge]
Mode=auto
LogLevel=2
StrictBitPerfect=1
VolumeControl=unity
FollowSourceRate=1

[ASIO]
Driver=auto
Channels=0
BufferSize=0
SampleRate=0

[WASAPI]
BufferMs=20
```

`auto` selects ASIO when a registered ASIO driver is available, otherwise
WASAPI exclusive. `unity` leaves software gain alone; Apple applies its own
volume before handing PCM to the render stream. `monitor` keeps the normal
Windows audio path for troubleshooting.

## Limitations

- The COM overrides are per-user and affect other processes that create
  MMDeviceEnumerator or activate the Property System class. Non-target
  processes only forward the calls.
- The early-launch proxy stays loaded in processes that use that class. It only
  starts the agent and injects the bridge inside `AppleMusic.exe`.
- WASAPI exclusive holds the device until the app releases the stream. A paused
  track can prevent another application from opening the device exclusively.
- The MiniFuse WDM endpoint accepts 32-bit PCM with 24 valid bits, not float32.
  This path is transparent packing, not byte-for-byte bit-perfect output.
- Apple Music updates can change internal modules and break the hooks. Test
  after each app update.
- No warranty; use at your own risk.

## Build

From a Visual Studio x64 developer environment:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -OutputDir bin\x64\Release\v042
powershell -ExecutionPolicy Bypass -File .\release.ps1 -BuildDir bin\x64\Release\v042 -Version 0.4.2
```

The release contains `AMAudioBridgeSetup.exe`, `AMAudioBridge.dll`,
`AMEarlyProxy.dll`, `AMPrefsPersist.dll`, `AMAudioBridge.ini`, `README.md` and
`LICENSE`. The build directory also contains diagnostic tools that are not
packaged.

Binaries are x64, use the static C runtime (`/MT`), and depend only on standard
Windows system DLLs. Source and release text are ASCII-only.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
