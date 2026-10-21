#pragma once

#include "common.h"

typedef struct AmbConfig {
    AmbMode mode;
    int log_level;
    BOOL strict_bitperfect;
    BOOL volume_control;
    wchar_t asio_driver[128];
    int asio_channels;
    int asio_buffer_frames;
    int asio_rate;
    int wasapi_buffer_ms;
    int device_poll_ms;
} AmbConfig;

extern AmbConfig g_cfg;

void Config_Load(void);
const char* Config_ModeName(AmbMode mode);
