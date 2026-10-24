#pragma once

#include "common.h"

BOOL Hooks_Install(void);
HRESULT Hooks_ActivateDevice(IMMDevice* device, REFIID iid, DWORD clsctx,
                             PROPVARIANT* params, void** out);
