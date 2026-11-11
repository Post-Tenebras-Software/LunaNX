#pragma once
#include <switch.h>

namespace luna::launcher
{
// Uses normal self-controller notifications. No hid:sys activation or access
// to system-owned button shared memory is required.
class CaptureInput
{
public:
    CaptureInput();
    ~CaptureInput();
    CaptureInput(const CaptureInput&) = delete;
    CaptureInput& operator=(const CaptureInput&) = delete;
    bool available() const { return available_; }
    bool takePress() { const bool press = pressed_; pressed_ = false; return press; }
    void listen(bool enabled);
private:
    static void hook(AppletHookType type, void* self);
    AppletHookCookie cookie_{};
    bool available_ = false, enabled_ = false, pressed_ = false;
};
}
