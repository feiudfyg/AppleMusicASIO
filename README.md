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
- Minimal verifier shim for early agent startup; the audio bridge stays outside
  System32 and loads through `MMDeviceEnumerator`.
- Small per-user bridge installation in `%LOCALAPPDATA%\AppleMusicASIO`.
- Static C runtime and no Visual C++ redistributable.
- Simple installer with an ASIO / WASAPI-exclusive mode selector.

Tested with Windows 11 x64, Apple Music `1.1540.23042.0`, a MiniFuse ASIO driver
and both output modes. Other drivers and Apple Music versions are untested.

## Install

The verifier shim is machine-wide and needs administrator approval. The audio
bridge is per-user.

1. Close Apple Music.
2. Open an elevated Command Prompt or PowerShell in the release folder and run:

   ```text
   AMVerifierSetup.exe install
   ```

   This copies `AMVerifierShim.dll` to `%ProgramFiles%\AppleMusicASIO`, creates a
   hard link in `System32`, and configures the Apple Music IFEO entry. It
   refuses to overwrite another verifier provider.
3. Run `AMAudioBridgeSetup.exe`, select `WASAPI exclusive` or `ASIO`, click
   `Install`, then start Apple Music.

`Apply` saves a mode change and closes the window. `Close` discards the change.
Restart Apple Music after changing the mode.

The audio setup copies the bridge and preferences helper to
`%LOCALAPPDATA%\AppleMusicASIO` and registers the per-user `MMDeviceEnumerator`
COM server. It removes the old per-user early-launch proxy during migration.
Administrator access is not required for this step.

To uninstall, close Apple Music, run `AMAudioBridgeSetup.exe` and click
`Uninstall`, then run `AMVerifierSetup.exe uninstall` from an elevated prompt.
The preferences junction, if created, is kept because it also fixes Apple
Music's own settings persistence.

## Security model

`AMVerifierShim.dll` is intentionally minimal:

- It only calls `CoCreateInstance` for Apple's library class. It does not load
  an audio DLL, read configuration, use the network, or inject into a process by
  name.
- It verifies the current package family and does nothing outside Apple Music.
  The IFEO key is name-based, so a fake `AppleMusic.exe` is rejected by the shim.
- The System32 entry is a hard link to an administrator-protected Program Files
  file. The directory ACL grants users read/execute only; users cannot replace
  the shim.
- The verifier shim starts `AMPLibraryAgent.exe`; the agent then loads the audio
  bridge normally through `MMDeviceEnumerator`.

`AMVerifierSetup.exe` requires administrator privileges. It backs up the
original IFEO values, refuses to overwrite a different `VerifierDlls` value,
checks file hashes during uninstall, and never creates a System32 symlink to a
user-writable path.

A machine-wide IFEO/verifier entry is a persistent code-loading point. Keeping
the shim minimal, protected and auditable is part of the design; there is no
claim of zero risk against a compromised administrator account.

## How it works

- `AMVerifierShim.dll` is loaded by the Windows Application Verifier
  infrastructure for `AppleMusic.exe`. It starts `AMPLibraryAgent.exe` through
  Apple's packaged COM server.
- `AMAudioBridge.dll` is registered per user for `CLSID_MMDeviceEnumerator`. It
  forwards device enumeration to `MMDevApi.dll`, then hooks audio client
  creation, the `IAudioClient` interface and Apple's CoreAudio format functions.
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

- The verifier shim needs administrator installation and a machine-wide IFEO
  entry. The system-wide entry targets every image named `AppleMusic.exe`; the
  shim's package check limits its effect to the real Apple Music package.
- The per-user `MMDeviceEnumerator` override affects other processes that create
  an audio device enumerator. Non-target processes only forward the calls.
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
powershell -ExecutionPolicy Bypass -File .\build.ps1 -OutputDir bin\x64\Release\v050
powershell -ExecutionPolicy Bypass -File .\release.ps1 -BuildDir bin\x64\Release\v050 -Version 0.5.0
```

The release contains `AMAudioBridgeSetup.exe`, `AMVerifierSetup.exe`,
`AMAudioBridge.dll`, `AMVerifierShim.dll`, `AMPrefsPersist.dll`,
`AMAudioBridge.ini`, `README.md` and `LICENSE`. The build directory also
contains diagnostic tools that are not packaged.

Binaries are x64, use the static C runtime (`/MT`), and depend only on standard
Windows system DLLs. Source and release text are ASCII-only.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
