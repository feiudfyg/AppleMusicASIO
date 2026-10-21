# AppleMusicASIO

ASIO / WASAPI-exclusive audio bridge for the Apple Music desktop app on Windows 11 x64.

The bridge is injected into the Apple Music process and replaces the app's WASAPI
render stream with a bit-perfect sink:

- **ASIO** – PCM is handed to the ASIO driver without resampling, format
  conversion, or DSP (when the negotiated formats match).
- **WASAPI exclusive** – the app stream is rendered through an exclusive-mode
  endpoint, bypassing the Windows mixer, resampler and APOs.

Status: work in progress.
