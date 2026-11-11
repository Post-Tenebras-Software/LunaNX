#pragma once
#include "native_auth.hpp"
#include <filesystem>

namespace luna::launcher
{
struct RecentGame
{
    auth::Game game;
    std::string path;
};
inline constexpr std::size_t MaxRecentGames = 12, MaxRecentProfiles = 8;
bool recentGamePath(const std::string& path);
class RecentGames
{
public:
    enum class Issue { None, Profile, GameId, Title, Artwork, Route, Encoding, Size,
        Directory, Write, Backup, Replace, Recovery, Filesystem };
    struct SaveStatus { Issue issue = Issue::None; int error = 0; };
    // The optional rename operation lets host tests reproduce the Switch's
    // refusal to overwrite an existing destination and exercise recovery.
    using Rename = void (*)(const std::filesystem::path&, const std::filesystem::path&, std::error_code&);
    explicit RecentGames(std::string path, Rename rename = nullptr);
    const std::vector<RecentGame>& games(const std::string& profile) const;
    // Call after the first presented frame, never for a pending/failed launch.
    // A failed write retains the new order for the current app session.
    bool record(const std::string& profile, RecentGame game);
    // Repair saved artwork from current catalog covers without recording a play.
    bool refreshArtwork(const std::string& profile, const std::vector<auth::Game>& catalog);
    SaveStatus saveStatus() const { return saveStatus_; }
    static const char* issueLabel(Issue issue);
private:
    struct Profile { std::string id; std::vector<RecentGame> games; };
    void load();
    bool save();
    std::string path_;
    std::vector<Profile> profiles_;
    Rename rename_;
    SaveStatus saveStatus_;
};
}
