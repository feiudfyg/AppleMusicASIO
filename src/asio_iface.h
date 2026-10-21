#pragma once

/*
 * Minimal binary-compatible declarations for the Steinberg ASIO 2.x host
 * interface.  The ASIO SDK is not bundled; this header only describes the
 * published COM-like ABI so the bridge can load a registered driver.
 */

#include "common.h"

typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;
typedef long ASIOSampleType;

#define ASIOTrue 1
#define ASIOFalse 0

#define ASE_OK 0
#define ASE_SUCCESS 0x3f4847a0
#define ASE_NotPresent (-1000)
#define ASE_HWMalfunction (-999)
#define ASE_InvalidParameter (-998)
#define ASE_InvalidMode (-997)
#define ASE_SPNotAdvancing (-996)
#define ASE_NoClock (-995)
#define ASE_NoMemory (-994)

typedef struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
} ASIOSamples;

typedef struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
} ASIOTimeStamp;

typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
} ASIOClockSource;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
} ASIOBufferInfo;

typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOTime ASIOTime;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex,
                                      ASIOBool directProcess);
} ASIOCallbacks;

enum {
    ASIOSTInt16MSB = 0,
    ASIOSTInt24MSB = 1,
    ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,
    ASIOSTDSDInt8LSB1 = 32,
    ASIOSTDSDInt8MSB1 = 33,
    ASIOSTDSDInt8NER8 = 40
};

enum {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion = 2,
    kAsioResetRequest = 3,
    kAsioBufferSizeChange = 4,
    kAsioResyncRequest = 5,
    kAsioLatenciesChanged = 6,
    kAsioSupportsTimeInfo = 7,
    kAsioSupportsTimeCode = 8,
    kAsioMMCCommand = 9,
    kAsioSupportsInputMonitor = 10,
    kAsioSupportsInputGain = 11,
    kAsioSupportsInputMeter = 12,
    kAsioSupportsOutputGain = 13,
    kAsioSupportsOutputMeter = 14,
    kAsioOverload = 15
};

typedef struct IASIO IASIO;

typedef struct IASIOVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(IASIO* This, REFIID riid, void** ppv);
    ULONG (STDMETHODCALLTYPE* AddRef)(IASIO* This);
    ULONG (STDMETHODCALLTYPE* Release)(IASIO* This);
    ASIOBool (STDMETHODCALLTYPE* init)(IASIO* This, void* sysHandle);
    void (STDMETHODCALLTYPE* getDriverName)(IASIO* This, char* name);
    long (STDMETHODCALLTYPE* getDriverVersion)(IASIO* This);
    void (STDMETHODCALLTYPE* getErrorMessage)(IASIO* This, char* string);
    ASIOError (STDMETHODCALLTYPE* start)(IASIO* This);
    ASIOError (STDMETHODCALLTYPE* stop)(IASIO* This);
    ASIOError (STDMETHODCALLTYPE* getChannels)(IASIO* This, long* numInputChannels,
                                               long* numOutputChannels);
    ASIOError (STDMETHODCALLTYPE* getLatencies)(IASIO* This, long* inputLatency,
                                                long* outputLatency);
    ASIOError (STDMETHODCALLTYPE* getBufferSize)(IASIO* This, long* minSize, long* maxSize,
                                                 long* preferredSize, long* granularity);
    ASIOError (STDMETHODCALLTYPE* canSampleRate)(IASIO* This, ASIOSampleRate sampleRate);
    ASIOError (STDMETHODCALLTYPE* getSampleRate)(IASIO* This, ASIOSampleRate* sampleRate);
    ASIOError (STDMETHODCALLTYPE* setSampleRate)(IASIO* This, ASIOSampleRate sampleRate);
    ASIOError (STDMETHODCALLTYPE* getClockSources)(IASIO* This, ASIOClockSource* clocks,
                                                   long* numSources);
    ASIOError (STDMETHODCALLTYPE* setClockSource)(IASIO* This, long reference);
    ASIOError (STDMETHODCALLTYPE* getSamplePosition)(IASIO* This, ASIOSamples* sPos,
                                                     ASIOTimeStamp* tStamp);
    ASIOError (STDMETHODCALLTYPE* getChannelInfo)(IASIO* This, ASIOChannelInfo* info);
    ASIOError (STDMETHODCALLTYPE* createBuffers)(IASIO* This, ASIOBufferInfo* bufferInfos,
                                                 long numChannels, long bufferSize,
                                                 ASIOCallbacks* callbacks);
    ASIOError (STDMETHODCALLTYPE* disposeBuffers)(IASIO* This);
    ASIOError (STDMETHODCALLTYPE* controlPanel)(IASIO* This);
    ASIOError (STDMETHODCALLTYPE* future)(IASIO* This, long selector, void* opt);
    ASIOError (STDMETHODCALLTYPE* outputReady)(IASIO* This);
} IASIOVtbl;

struct IASIO {
    const IASIOVtbl* lpVtbl;
};

#define IASIO_CALL(o, m, ...) ((o)->lpVtbl->m((o), __VA_ARGS__))
#define IASIO_CALLO(o, m) ((o)->lpVtbl->m((o)))
