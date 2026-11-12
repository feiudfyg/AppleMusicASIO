#pragma once

#include "common.h"

void AppHooks_Start(HMODULE module);
uint32_t AppHooks_GetSourceRate(void);
uint32_t AppHooks_GetSourceCodec(void);
BOOL AppHooks_GetCaptureFormat(WAVEFORMATEX* out, DWORD bytes);
void AppHooks_ResetCapture(void);
