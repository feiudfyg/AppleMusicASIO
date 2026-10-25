# AppleMusicASIO

ASIO / WASAPI-exclusive audio bridge for the Apple Music desktop app on Windows 11 x64.

The bridge is loaded into `AMPLibraryAgent.exe`, the process that owns Apple Music's
audio session. It replaces the app's WASAPI render stream with an ASIO or
WASAPI-exclusive sink. Windows shared-mode mixing, the Windows resampler and audio
processing objects (APOs) are bypassed.

## Status

Tested with:

- Windows 11 x64
- Apple Music `1.1540.23042.0` (MSIX, full-trust)
- MiniFuse ASIO driver 1.091
- 44.1, 48, 96 and 192 kHz content, including automatic rate switching
- ASIO and WASAPI exclusive
- Per-user installation without a service, scheduled task, watcher or launcher

Current version: `0.3.0`.

## How it is loaded

`AMAudioBridge.dll` is registered as a per-user in-process COM server for
`CLSID_MMDeviceEnumerator`:

```
HKCU\Software\Classes\CLSID\{BCDE0395-E52F-467C-8E3D-C4579291692E}\InprocServer32
```

Apple Music creates the MMDeviceEnumerator when it opens audio. COM loads the
bridge in-process. `DllGetClassObject` forwards to the real `MMDevApi.dll`, so
device enumeration behaves normally. The bridge then hooks:
`IMMDevice::Activate`, the `IAudioClient` vtable, and Apple's CoreAudio format
functions.

This is not a DLL search-path hijack. It does not change `WindowsApps`, system
DLLs or the machine-wide COM registration. It creates no separate process. In
processes other than the audio agent, the bridge forwards COM calls and asks COM
to unload it as soon as it has no objects (`DllCanUnloadNow`).

## Install

Close Apple Music before installing or uninstalling so the bridge files are not
locked. Then run `AMAudioBridgeSetup.exe` from the release folder.

1. Select `WASAPI exclusive` or `ASIO`.
2. Click `Install DLL`.
3. Click `Confirm`.
4. Start or restart Apple Music.

The installer:

- copies the bridge, preference helper and INI to
  `%LOCALAPPDATA%\AppleMusicASIO`;
- registers the per-user COM server;
- stores the selected mode in
  `HKCU\Software\AppleMusicASIO\Mode`;
- automatically detects whether Apple's MSIX data is redirected to another
  volume and creates the preferences junction only when it is actually needed.

`Uninstall DLL` removes the COM registration and schedules loaded files for
deletion at the next reboot. Close Apple Music before uninstalling to remove the
files immediately.

The preferences junction is intentionally kept after uninstalling the audio
bridge: it also fixes Apple Music's own settings persistence.

## Configuration

The GUI writes the output mode to the registry. The bridge reads:

```
HKCU\Software\AppleMusicASIO
    Mode = wasapi | asio | auto | monitor | off
```

Registry mode overrides `Mode` in `AMAudioBridge.ini`. Other settings remain in
the INI next to the installed DLL:

```ini
[Bridge]
Mode=auto                 ; overridden by the registry value when present
LogLevel=2
StrictBitPerfect=1        ; advisory; the bridge never resamples
VolumeControl=unity       ; unity | app
FollowSourceRate=1

[ASIO]
Driver=auto
Channels=0
BufferSize=0
SampleRate=0

[WASAPI]
BufferMs=20
```

- `Mode=auto` selects ASIO when a registered driver is available, otherwise
  WASAPI exclusive.
- `FollowSourceRate=1` makes 44.1/48/96/192 kHz content stay at its source rate
  for both ASIO and WASAPI exclusive.
- `VolumeControl=unity` adds no software gain. Apple applies its own volume
  before handing PCM to the render client; the endpoint may still require
  float32-to-integer packing.
- `StrictBitPerfect` is advisory. The bridge never resamples, but integer
  packing is used when the endpoint has no float32 exclusive format.

## Audio paths

- `GetMixFormat` reports the source rate while preserving the endpoint's float32
  device format. The app initializes at that rate.
- ASIO opens at the exact source rate and channel count. Float32 is packed to the
  driver's native integer type without resampling.
- WASAPI exclusive opens at the exact source rate. The endpoint format is
  negotiated from the formats it actually accepts. The MiniFuse WDM endpoint
  accepts 32-bit PCM with 24 valid bits, not float32; the bridge packs float32 to
  PCM24 and uses a dedicated exclusive client.
- The WASAPI client is owned by the bridge so a new playback can release the
  previous device handle instead of failing with
  `AUDCLNT_E_DEVICE_IN_USE`.

## Preferences persistence

Apple Music writes CFPreferences temporary files under the package LocalCache.
When the package data is redirected to another volume, the final rename to
`%APPDATA%\Apple Computer\Preferences` crosses volumes and fails.

Two mechanisms handle this:

- `Setup_EnsurePreferencesJunction` resolves the real LocalCache target with
  `FSCTL_GET_REPARSE_POINT`, compares volumes, and creates a junction at
  `%APPDATA%\Apple Computer\Preferences` only when needed. It runs at install;
  if the agent has already re-created a real preferences directory at startup,
  it defers repair instead of racing Apple's file handles. No drive letters,
  SIDs or user names are hard-coded.
- `AMPrefsPersist.dll` is loaded by the bridge into the agent. It copies a
  temporary `.plist.Xa...` write to the final plist before Apple deletes the
  temporary file. This is the write-through guarantee when Apple has already
  replaced the junction.

## Limitations

- Apple Music's decoder and processing before `IAudioClient` remain in the path.
  The bridge removes Windows mixing/resampling and APOs, not Apple's decoder.
- The MiniFuse WDM endpoint does not expose float32 in exclusive mode. The
  WASAPI path therefore performs a transparent float32-to-PCM24 packing; ASIO is
  the only path that can use the driver's native sample type.
- WASAPI exclusive holds the device until the app releases the stream. A paused
  track can prevent other applications from taking the device exclusively.
- The COM override is per-user and affects any process that creates
  MMDeviceEnumerator while installed. Non-target processes only forward the
  call and do not install hooks.
- Apple Music updates can change internal modules and break the hooks. Test after
  each app update.
- This project does not bypass DRM or content protection.

## Build

From a Visual Studio x64 developer environment:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -OutputDir bin\x64\Release
```

Artifacts:

- `AMAudioBridge.dll` - COM proxy and audio bridge
- `AMAudioBridgeSetup.exe` - Win32 installer/configuration window
- `AMPrefsPersist.dll` - preference write-through helper
- `AMAudioBridge.ini` - fallback configuration
- `AMAudioBridgeInjector.exe` - manual diagnostic injector
- `AMAudioProbe.exe` - synthetic render client
- `AMAsioTool.exe` - ASIO enumeration
- `AMCoreAudioProbe.dll` - optional CoreAudio format logger

A loaded DLL is locked, so build into a new directory while testing.

## Troubleshooting

- `AMAudioBridge.log` in `%LOCALAPPDATA%\AppleMusicASIO`.
- The installer status shows whether the per-user COM registration points at the
  installed bridge.
- If audio is not bridged, confirm `HKCU\Software\AppleMusicASIO\Mode` and
  restart Apple Music so the COM server is loaded in a new agent process.
- Set `Mode=monitor` in the INI (and remove the registry `Mode` value) to verify
  the normal Windows path.
- `AMAudioBridgeInjector.exe --dll AMAudioBridge.dll --pid <agent pid>` remains
  available for diagnosis.
