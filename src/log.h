#pragma once

#include "common.h"

void Log_Init(HMODULE module);
void Log_Close(void);
void Log_SetLevel(int level);
int Log_Level(void);
const wchar_t* Log_Dir(void);
void Log_Write(int level, const char* file, int line, const char* fmt, ...);

#define LOGE(...) Log_Write(0, __FILE__, __LINE__, __VA_ARGS__)
#define LOGW(...) Log_Write(1, __FILE__, __LINE__, __VA_ARGS__)
#define LOGI(...) Log_Write(2, __FILE__, __LINE__, __VA_ARGS__)
#define LOGD(...) Log_Write(3, __FILE__, __LINE__, __VA_ARGS__)
#define LOGT(...) Log_Write(4, __FILE__, __LINE__, __VA_ARGS__)
