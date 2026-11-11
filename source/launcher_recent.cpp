#include "launcher_recent.hpp"
#include "launcher_media.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <set>

namespace luna::launcher
{
namespace
{
using Json = nlohmann::json;
constexpr std::size_t MaxFile = 512 * 1024;
bool identifier(const std::string& s)
{
    return !s.empty() && s.size() <= 512 && std::all_of(s.begin(), s.end(),
        [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
RecentGames::Issue validationIssue(const RecentGame& entry)
{
    using Issue = RecentGames::Issue;
    const auto& g = entry.game;
    if (!identifier(g.id)) return Issue::GameId;
    if (g.title.empty() || g.title.size() > 256 ||
        std::any_of(g.title.begin(), g.title.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return Issue::Title;
    if (!g.artwork.empty() && !artworkUrl(g.artwork)) return Issue::Artwork;
    return recentGamePath(entry.path) ? Issue::None : Issue::Route;
}
}
bool recentGamePath(const std::string& path)
{
    return path.size() <= 1024 && (path.rfind("game/", 0) == 0 || path.rfind("detail/", 0) == 0) &&
        path.back() != '/' && path.find("//") == std::string::npos &&
        path.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/_-") == std::string::npos;
}
RecentGames::RecentGames(std::string path, Rename rename) : path_(std::move(path)),
    rename_(rename ? rename : static_cast<Rename>(std::filesystem::rename)) { load(); }
const char* RecentGames::issueLabel(Issue issue)
{
    switch (issue)
    {
    case Issue::None: return "none";
    case Issue::Profile: return "profile";
    case Issue::GameId: return "game_id";
    case Issue::Title: return "title";
    case Issue::Artwork: return "artwork";
    case Issue::Route: return "route";
    case Issue::Encoding: return "encoding";
    case Issue::Size: return "size";
    case Issue::Directory: return "directory";
    case Issue::Write: return "write";
    case Issue::Backup: return "backup";
    case Issue::Replace: return "replace";
    case Issue::Recovery: return "recovery";
    case Issue::Filesystem: return "filesystem";
    }
    return "unknown";
}
const std::vector<RecentGame>& RecentGames::games(const std::string& profile) const
{
    static const std::vector<RecentGame> empty;
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.id == profile; });
    return found == profiles_.end() ? empty : found->games;
}
void RecentGames::load()
{
    try
    {
        // A power loss between moving the old file aside and installing the
        // new one leaves a backup, not a reason to discard the user's history.
        if (!std::filesystem::exists(path_) && std::filesystem::exists(path_ + ".bak"))
        {
            std::error_code error;
            rename_(path_ + ".bak", path_, error);
            if (error) { saveStatus_ = {Issue::Recovery, error.value()}; return; }
        }
        const auto size = std::filesystem::file_size(path_);
        if (size > MaxFile) return;
        std::ifstream input(path_, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        input.read(bytes.data(), bytes.size());
        if (!input || input.peek() != std::char_traits<char>::eof()) return;
        std::vector<std::set<std::string>> keys;
        const auto data = Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value)
        {
            if (depth > 12) throw std::invalid_argument("Recent history depth");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                throw std::invalid_argument("Duplicate recent field");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
        if (!data.is_object() || data.size() != 2 || data.at("version") != 1) return;
        const auto& profiles = data.at("profiles");
        if (!profiles.is_array() || profiles.size() > MaxRecentProfiles) return;
        std::vector<Profile> loaded;
        std::set<std::string> ids;
        for (const auto& p : profiles)
        {
            if (!p.is_object() || p.size() != 2) return;
            Profile profile{p.at("id").get<std::string>(), {}};
            if (!identifier(profile.id) || !ids.insert(profile.id).second) return;
            const auto& entries = p.at("games");
            if (!entries.is_array() || entries.size() > MaxRecentGames) return;
            std::set<std::string> games;
            for (const auto& e : entries)
            {
                if (!e.is_object() || e.size() != 4) return;
                RecentGame entry{{e.at("id").get<std::string>(), e.at("title").get<std::string>(), true,
                    e.at("artwork").get<std::string>()}, e.at("path").get<std::string>()};
                if (validationIssue(entry) != Issue::None || !games.insert(entry.game.id).second) return;
                profile.games.push_back(std::move(entry));
            }
            loaded.push_back(std::move(profile));
        }
        profiles_ = std::move(loaded);
    }
    catch (...) {} // A missing/corrupt history must never block sign-in or play.
}
bool RecentGames::save()
{
    try
    {
        Json data = {{"version", 1}, {"profiles", Json::array()}};
        for (const auto& p : profiles_)
        {
            Json profile = {{"id", p.id}, {"games", Json::array()}};
            for (const auto& e : p.games)
                profile["games"].push_back({{"id", e.game.id}, {"title", e.game.title},
                    {"artwork", e.game.artwork}, {"path", e.path}});
            data["profiles"].push_back(std::move(profile));
        }
        const auto bytes = data.dump();
        if (bytes.size() > MaxFile) { saveStatus_ = {Issue::Size, 0}; return false; }
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), error);
        if (error) { saveStatus_ = {Issue::Directory, error.value()}; return false; }
        const auto temporary = path_ + ".tmp";
        const auto backup = path_ + ".bak";
        errno = 0;
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), bytes.size()); out.close();
        if (!out) { saveStatus_ = {Issue::Write, errno}; return false; }
        // Horizon's rename does not replace a destination. Keep the previous
        // complete file until the new file is closed, then move it aside.
        const bool previous = std::filesystem::exists(path_);
        if (previous)
        {
            if (!std::filesystem::is_regular_file(path_) ||
                (std::filesystem::exists(backup) && !std::filesystem::is_regular_file(backup)))
            { saveStatus_ = {Issue::Backup, 0}; return false; }
            std::filesystem::remove(backup, error);
            if (!error) rename_(path_, backup, error);
            if (error) { saveStatus_ = {Issue::Backup, error.value()}; return false; }
        }
        rename_(temporary, path_, error);
        if (error)
        {
            saveStatus_ = {Issue::Replace, error.value()};
            if (previous)
            {
                error.clear(); rename_(backup, path_, error);
                if (error) saveStatus_ = {Issue::Recovery, error.value()};
            }
            std::filesystem::remove(temporary, error);
            return false;
        }
        // If cleanup fails, the new history is still saved. The next write
        // removes this stale backup before moving the current file aside.
        std::filesystem::remove(backup, error);
        saveStatus_ = {};
        return true;
    }
    catch (const std::filesystem::filesystem_error& e)
    { saveStatus_ = {Issue::Filesystem, e.code().value()}; return false; }
    catch (...) { saveStatus_ = {Issue::Encoding, 0}; return false; }
}
bool RecentGames::refreshArtwork(const std::string& profile, const std::vector<auth::Game>& catalog)
{
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.id == profile; });
    if (found == profiles_.end()) return true;
    bool changed = false;
    for (auto& entry : found->games)
    {
        const auto cover = std::find_if(catalog.begin(), catalog.end(), [&](const auto& g)
            { return g.id == entry.game.id && !g.artwork.empty() && artworkUrl(g.artwork); });
        if (cover != catalog.end() && cover->artwork != entry.game.artwork)
        {
            entry.game.artwork = cover->artwork;
            changed = true;
        }
    }
    return !changed || save();
}
bool RecentGames::record(const std::string& profile, RecentGame game)
{
    saveStatus_ = {};
    if (!identifier(profile)) { saveStatus_ = {Issue::Profile, 0}; return false; }
    saveStatus_.issue = validationIssue(game);
    if (saveStatus_.issue != Issue::None) return false;
    try { (void)Json(game.game.title).dump(); } catch (...) { saveStatus_ = {Issue::Encoding, 0}; return false; }
    auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.id == profile; });
    Profile updated{profile, {}};
    if (found != profiles_.end()) { updated = std::move(*found); profiles_.erase(found); }
    auto& list = updated.games;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& e) { return e.game.id == game.game.id; }), list.end());
    list.insert(list.begin(), std::move(game));
    if (list.size() > MaxRecentGames) list.resize(MaxRecentGames);
    profiles_.insert(profiles_.begin(), std::move(updated));
    if (profiles_.size() > MaxRecentProfiles) profiles_.resize(MaxRecentProfiles);
    return save();
}
}
