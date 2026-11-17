#pragma once

#include "common.h"

BOOL Hooks_Install(void);
HRESULT Hooks_ActivateDevice(IMMDevice* device, REFIID iid, DWORD clsctx,
                             PROPVARIANT* params, void** out);
void Hooks_WriteCapture(const void* data, uint32_t frames);
void Hooks_CaptureAudioUnit(void* io_data, uint32_t frames);
BOOL Hooks_SwitchToCapture(void);
void Hooks_StopCapture(void);
void Hooks_StopNormalStream(void);
