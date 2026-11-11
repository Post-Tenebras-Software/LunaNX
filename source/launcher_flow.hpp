#pragma once
#include "native_auth.hpp"
#include <optional>

namespace luna::launcher
{
// One explicit Play action survives its notice/widget reads. It is consumed
// once, and navigation/failure cancels it; it never retries provisioning.
class PlayIntent
{
public:
    void request(std::size_t game) { game_ = game; }
    void cancel() { game_.reset(); }
    bool needsInspection(const auth::Status& status, std::size_t game) const
    {
        return game_ && *game_ == game && status.step == auth::Step::DetailReady &&
            status.detailParsing.error == auth::Error::None && !status.detailParsing.hasPlayToken &&
            (status.detailParsing.action == auth::LaunchAction::DirectLaunch ||
             status.detailParsing.action == auth::LaunchAction::ExperimentalClient);
    }
    bool mayAcknowledgeClientWarning(const auth::Status& status, std::size_t game) const
    {
        return game_ && *game_ == game && status.step == auth::Step::ExperimentalReady &&
            status.detailParsing.error == auth::Error::None &&
            status.detailParsing.primaryAction == auth::LaunchAction::ExperimentalClient &&
            status.noticeParsing.complete;
    }
    bool consume(const auth::Status& status, std::size_t game)
    {
        if (!game_) return false;
        if (*game_ != game || status.step == auth::Step::Failed || status.step == auth::Step::Cancelled ||
            status.detailParsing.error != auth::Error::None)
        { cancel(); return false; }
        if (status.step != auth::Step::DetailReady) return false;
        if (status.detailParsing.hasPlayToken)
        { cancel(); return true; }
        if (status.detailParsing.action != auth::LaunchAction::DirectLaunch &&
            status.detailParsing.action != auth::LaunchAction::ExperimentalClient) cancel();
        return false;
    }
private:
    std::optional<std::size_t> game_;
};
}
