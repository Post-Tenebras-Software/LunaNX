#pragma once
#include "native_auth.hpp"
#include "video_mode.hpp"
#include "video_bitrate.hpp"
#include "video_decoder.hpp"
#include "launcher_recent.hpp"
#include "native_profiles.hpp"
#include <functional>
#include <memory>

namespace luna::launcher
{
enum class Page { Welcome, Link, Profiles, Home, Detail, Notice, Connecting, Error, Settings, Gallery, Ended, Library, ManageProfiles, EditProfile, ProfileResult, About, Trailer, EndGame };
struct Image
{
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};
struct Model
{
    Page page = Page::Welcome;
    std::string title, message, profileName, profileArtwork, publicCode;
    std::string menuShortcut = "Capture + Plus", shortcutStatus, shortcutInstructions = "Hold Plus, then tap Capture.";
    std::string loginMessage;
    std::string searchQuery, browseMessage;
    std::size_t catalogOffset = 0;
    bool browsing = false, hasNextPage = false;
    bool includedOnly = false;
    const std::vector<auth::Profile>* profiles = nullptr;
    const std::vector<auth::ProfileManager::Entry>* managedProfiles = nullptr;
    const auth::ProfileManager::Form* profileForm = nullptr;
    unsigned profileRow = 0;
    bool canCreateProfile = false, canSaveProfile = false, confirmProfileSave = false;
    bool profileSaving = false;
    const std::vector<auth::Game>* games = nullptr;
    const std::vector<RecentGame>* recentGames = nullptr;
    std::size_t selectedRecent = 0;
    bool recentFocused = true;
    bool hasRemoteGame = false, remoteFocused = false;
    unsigned remoteAction = 0;
    std::string remoteGameTitle, remoteGameArtwork, remoteMessage, replacementGameTitle;
    const auth::GameDetail* detail = nullptr;
    const auth::ExperimentalNotice* notice = nullptr;
    std::vector<std::string> noticeLines;
    std::size_t selected = 0, selectedGame = 0, noticePage = 0, noticePages = 1, screenshot = 0;
    std::optional<std::int64_t> queuePosition, waitSeconds;
    video::Mode video = video::Mode::P720_30;
    video::Decoder decoder = video::Decoder::Automatic;
    unsigned bitrateMbps = 0;
    std::string videoPreferencesMessage;
    bool docked = false, canPlay = false, noticeComplete = false, remoteMayRemain = false;
    unsigned animation = 0;
    unsigned settingsRow = 0;
    bool recordingShortcut = false;
    bool statsOverlay = false, overlayShortcutAvailable = true, waitingForVideo = false;
    bool rememberedLogin = false, signedIn = false, forgetLoginConfirmation = false;
    std::size_t descriptionPage = 0;
    bool trailerFailed = false;
};
inline std::size_t navigate(std::size_t selected, std::size_t count, int dx, int dy, unsigned columns)
{
    if (!count) return 0;
    const auto next = static_cast<std::int64_t>(std::min(selected, count - 1)) + dx +
        static_cast<std::int64_t>(dy) * columns;
    return static_cast<std::size_t>(std::clamp<std::int64_t>(next, 0, count - 1));
}
inline void navigateHome(bool& recent, std::size_t& recentIndex, std::size_t& gameIndex,
    std::size_t recentCount, std::size_t gameCount, int dx, int dy)
{
    if (!recentCount) { recent = false; gameIndex = navigate(gameIndex, gameCount, dx, dy, 4); return; }
    if (!gameCount || dy < 0) recent = true;
    else if (dy > 0) recent = false;
    if (recent) recentIndex = navigate(recentIndex, recentCount, dx, 0, 4);
    else gameIndex = navigate(gameIndex, gameCount, dx, 0, 4);
}
inline void navigateHomeWithSession(bool& session, unsigned& action, bool& recent,
    std::size_t& recentIndex, std::size_t& gameIndex, std::size_t recentCount,
    std::size_t gameCount, bool available, int dx, int dy)
{
    if (!available) session = false;
    if (available && session)
    {
        action = static_cast<unsigned>(navigate(action, 2, dx, 0, 2));
        if (dy > 0 && (recentCount || gameCount)) { session = false; recent = recentCount != 0; }
        return;
    }
    if (available && dy < 0 && ((recentCount && recent) || (!recentCount && gameIndex < 4)))
    { session = true; action = 0; return; }
    navigateHome(recent, recentIndex, gameIndex, recentCount, gameCount, dx, dy);
}
class Renderer
{
public:
    using Artwork = std::function<const Image*(const std::string&)>;
    Renderer(const std::string& regularFont, const std::string& boldFont);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    const Image& draw(const Model& model, const Artwork& artwork);
    std::size_t descriptionPages(const std::string& text);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
