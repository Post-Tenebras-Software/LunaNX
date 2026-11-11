#pragma once
#include <cstdint>
#include <vector>
using u32 = std::uint32_t;
using Result = std::uint32_t;
inline bool R_SUCCEEDED(Result result) { return !result; }
inline bool R_FAILED(Result result) { return result != 0; }
inline constexpr unsigned PIXEL_FORMAT_RGBA_8888 = 1;
inline constexpr u32 RGBA8_MAXALPHA(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16) | 0xff000000; }
struct PrintConsole;
struct ConsoleRenderer
{
    bool (*init)(PrintConsole*);
    void (*deinit)(PrintConsole*);
    void (*draw)(PrintConsole*, int, int, int);
    void (*scroll)(PrintConsole*);
    void (*flush)(PrintConsole*);
};
struct PrintConsole { ConsoleRenderer* renderer = nullptr; };
struct NWindow { u32 width = 854, height = 480; };
struct Framebuffer { u32 width = 0, height = 0; std::vector<unsigned char> pixels; };
inline NWindow window;
inline Framebuffer* activeFramebuffer = nullptr;
inline NWindow* nwindowGetDefault() { return &window; }
inline Result nwindowGetDimensions(NWindow* w, u32* width, u32* height) { *width = w->width; *height = w->height; return 0; }
inline Result nwindowSetDimensions(NWindow* w, u32 width, u32 height) { w->width = width; w->height = height; return 0; }
inline Result framebufferCreate(Framebuffer* fb, NWindow*, u32 width, u32 height, unsigned, unsigned)
{ fb->width = width; fb->height = height; fb->pixels.resize(width * height * 4); activeFramebuffer = fb; return 0; }
inline Result framebufferMakeLinear(Framebuffer*) { return 0; }
inline void framebufferClose(Framebuffer* fb) { if (activeFramebuffer == fb) activeFramebuffer = nullptr; fb->pixels.clear(); }
inline void* framebufferBegin(Framebuffer* fb, u32* stride) { *stride = fb->width * 4; return fb->pixels.data(); }
inline void framebufferEnd(Framebuffer*) {}
inline void consoleExit(PrintConsole*) {}
inline void consoleInit(PrintConsole*) {}
