#pragma once
#include <cstdint>
#include <cstddef>
using Result = std::uint32_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#define R_FAILED(x) ((x) != 0)
enum PcmFormat { PcmFormat_Int16 };
struct AudioOutBuffer { AudioOutBuffer* next = nullptr; void* buffer = nullptr; u64 buffer_size = 0, data_size = 0, data_offset = 0; };
Result audoutInitialize();
void audoutExit();
u32 audoutGetSampleRate();
u32 audoutGetChannelCount();
PcmFormat audoutGetPcmFormat();
Result audoutAppendAudioOutBuffer(AudioOutBuffer*);
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer**, u32*);
Result audoutStartAudioOut();
Result audoutStopAudioOut();
void armDCacheFlush(void*, std::size_t);
