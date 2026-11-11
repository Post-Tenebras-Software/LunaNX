#include "launcher_capture.hpp"
#include "launcher_shortcut.hpp"

namespace luna::launcher
{
static_assert(PlusButton == HidNpadButton_Plus && MinusButton == HidNpadButton_Minus,
    "Saved shortcut masks must match standard libnx Npad buttons");
CaptureInput::CaptureInput()
{
    available_ = R_SUCCEEDED(appletSetRequiresCaptureButtonShortPressedMessage(false));
    appletHook(&cookie_, hook, this);
}
CaptureInput::~CaptureInput()
{
    listen(false);
    appletUnhook(&cookie_);
}
void CaptureInput::hook(AppletHookType type, void* self)
{
    auto& input = *static_cast<CaptureInput*>(self);
    if (type == AppletHookType_OnCaptureButtonShortPressed && input.enabled_) input.pressed_ = true;
    if (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus)
        input.pressed_ = false;
}
void CaptureInput::listen(bool enabled)
{
    if (enabled == enabled_ || (enabled && !available_)) return;
    const auto result = appletSetRequiresCaptureButtonShortPressedMessage(enabled);
    if (R_SUCCEEDED(result)) enabled_ = enabled;
    else available_ = false;
}
}
