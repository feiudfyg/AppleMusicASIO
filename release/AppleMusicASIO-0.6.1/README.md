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
- One elevated installer. It installs the Application Verifier shim and the
  audio bridge; no separate verifier helper program.
- The verifier shim loads the bridge only in `AMPLibraryAgent.exe`; no verifier
  is attached to the UI process.
- No COM hijack. The bridge is loaded only by the verifier shim inside the
  Apple Music package.
- The bridge is installed in protected `%ProgramFiles%\AppleMusicASIO`; its
  SHA-256 is compiled into the shim and checked before loading.
- Static C runtime and no Visual C++ redistributable.
- Simple installer with an ASIO / WASAPI-exclusive mode selector.

Tested with Windows 11 x64, Apple Music `1.1540.23042.0`, a MiniFuse ASIO driver
and both output modes. Other drivers and Apple Music versions are untested.

## Install

The installer is machine-wide and requires administrator approval.

1. Close Apple Music.
2. Run `AMAudioBridgeSetup.exe` from the release folder. Approve the UAC prompt.
3. Select `WASAPI exclusive` or `ASIO`, click `Install`, then start Apple Music.

The installer:

- Copies `AMVerifierShim.dll`, `AMAudioBridge.dll`, `AMPrefsPersist.dll` and
  `AMAudioBridge.ini` to `%ProgramFiles%\AppleMusicASIO`.
- Creates a hard link to the shim in `System32` and configures the Application
  Verifier IFEO entry for `AMPLibraryAgent.exe` only.
- Protects the Program Files directory with an administrator-only write ACL.
- Removes old `AppleMusic.exe` verifier, per-user early proxy and
  `MMDeviceEnumerator` COM registrations during migration.
- Stores the selected mode in
  `HKCU\Software\AppleMusicASIO\Mode`.

`Apply` saves a mode change and closes the window. `Close` discards the change.
Restart Apple Music after changing the mode.

To uninstall, close Apple Music, run `AMAudioBridgeSetup.exe` and click
`Uninstall`. The preferences junction, if created, is kept because it also fixes
Apple Music's settings persistence.

## Security model

`AMVerifierShim.dll` is intentionally small:

- It verifies the current package family and does nothing outside the Apple
  Music package. The IFEO key is name-based, so a fake `AMPLibraryAgent.exe` is
  rejected by the package check.
- It loads `AMAudioBridge.dll` in `AMPLibraryAgent.exe` only after checking the
  file's compile-time SHA-256. There is no UI-process verifier, no COM
  registration, no remote thread injection and no process-name injection.
- The bridge and shim live in an administrator-protected directory. The
  System32 entry is a hard link, not a symlink to a user-writable path. The
  directory ACL grants users read/execute only.
- The installer backs up the original IFEO values, refuses to overwrite another
  `VerifierDlls` provider, verifies hashes during uninstall, and restores the
  original values.

A machine-wide IFEO/verifier entry is a persistent code-loading point. Keeping
the shim minimal, protected and auditable is part of the design; there is no
claim of zero risk against a compromised administrator account.

## How it works

- Apple Music starts `AMPLibraryAgent.exe` normally.
- Windows Application Verifier loads `AMVerifierShim.dll` only for
  `AMPLibraryAgent.exe`.
- The shim verifies the bridge hash and loads `AMAudioBridge.dll` in-process.
  The bridge hooks audio client creation, `IAudioClient` and Apple's CoreAudio
  format functions.
- The UI process is not instrumented by Application Verifier; this avoids the
  startup and stability problems seen when the verifier was attached to the
  V8-based UI.
- `AMPrefsPersist.dll` is loaded by the bridge and fixes settings persistence
  when Apple's MSIX LocalCache is redirected to another volume.

The bridge never resamples. ASIO opens the driver at the source rate and
channel count. WASAPI exclusive negotiates a format the endpoint accepts; when
float32 is unavailable, the bridge packs it to an integer PCM format.

## Configuration

The setup program stores the output mode here:

```text
HKCU\Software\AppleMusicASIO\Mode
```

Values: `wasapi`, `asio`, `auto`, `monitor`, `off`. The registry value overrides
`Mode` in `AMAudioBridge.ini`. Other settings are read from the INI in
`%ProgramFiles%\AppleMusicASIO`:

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

Bridge logs are written to `%LOCALAPPDATA%\AppleMusicASIO\bridge.log` when the
Program Files directory is not writable.

## Limitations

- The installer needs administrator rights and writes a machine-wide IFEO entry
  for `AMPLibraryAgent.exe`. The shim's package check limits its effect to the
  real Apple Music package.
- The bridge must be updated together with the shim because its hash is compiled
  into the shim.
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
powershell -ExecutionPolicy Bypass -File .\build.ps1 -OutputDir bin\x64\Release\v061
powershell -ExecutionPolicy Bypass -File .\release.ps1 -BuildDir bin\x64\Release\v061 -Version 0.6.1
```

The release contains `AMAudioBridgeSetup.exe`, `AMAudioBridge.dll`,
`AMVerifierShim.dll`, `AMPrefsPersist.dll`, `AMAudioBridge.ini`, `README.md`
and `LICENSE`. The build directory also contains diagnostic tools that are not
packaged.

Binaries are x64, use the static C runtime (`/MT`), and depend only on standard
Windows system DLLs. Source and release text are ASCII-only.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
