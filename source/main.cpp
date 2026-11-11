#include "native_auth.hpp"
#include "native_session.hpp"
#include "native_runtime.hpp"
#include "local_shortcuts.hpp"
#include "launcher_ui.hpp"
#include "launcher_view.hpp"
#include "launcher_artwork.hpp"
#include "launcher_flow.hpp"
#include "launcher_shortcut.hpp"
#include "launcher_capture.hpp"
#include "launcher_preferences.hpp"
#include "launcher_recent.hpp"
#include "native_profiles.hpp"
#include "remembered_login.hpp"
#include "login_key_switch.hpp"
#include "launcher_trailer.hpp"
#include "video_view.hpp"

#include <switch.h>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
std::unique_ptr<luna::auth::NativeAuthProbe> makeAuthProbe()
{
    return std::make_unique<luna::auth::NativeAuthProbe>(luna::auth::Mode::ProfilesAndCatalog, true);
}
std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void erase(std::string& string)
{
    volatile char* data = string.empty() ? nullptr : &string[0];
    for (std::size_t i = 0; i < string.size(); ++i) data[i] = 0;
    string.clear();
}
CURLcode importTrustedRoots(CURL*, void* context, void*)
{
    // switch-curl's libnx callback receives an SslContext. Import only roots
    // currently marked trusted by Horizon; peer/hostname/date checks stay on.
    try
    {
        u32 selector = static_cast<u32>(SslCaCertificateId_All), size = 0, count = 0;
        if (R_FAILED(sslGetCertificateBufSize(&selector, 1, &size)) || !size || size > 2 * 1024 * 1024)
            return CURLE_SSL_CACERT_BADFILE;
        std::vector<u8> buffer(size);
        if (R_FAILED(sslGetCertificates(buffer.data(), size, &selector, 1, &count)) ||
            count > size / sizeof(SslBuiltInCertificateInfo)) return CURLE_SSL_CACERT_BADFILE;
        const auto* certificates = reinterpret_cast<const SslBuiltInCertificateInfo*>(buffer.data());
        unsigned imported = 0;
        for (u32 i = 0; i < count; ++i)
        {
            const auto& cert = certificates[i];
            if (cert.status != SslTrustedCertStatus_EnabledTrusted) continue;
            const auto offset = reinterpret_cast<std::uintptr_t>(cert.cert_data) - reinterpret_cast<std::uintptr_t>(buffer.data());
            if (offset > size || !cert.cert_size || cert.cert_size > size - offset)
                return CURLE_SSL_CACERT_BADFILE;
            if (R_FAILED(sslContextImportServerPki(static_cast<SslContext*>(context), cert.cert_data,
                static_cast<u32>(cert.cert_size), SslCertificateFormat_Der, nullptr))) return CURLE_SSL_CACERT_BADFILE;
            ++imported;
        }
        return imported ? CURLE_OK : CURLE_SSL_CACERT_BADFILE;
    }
    catch (...) { return CURLE_OUT_OF_MEMORY; }
}
const char* label(luna::auth::Step step)
{
    using luna::auth::Step;
    switch (step)
    {
    case Step::Idle: return "Ready. Press A to begin native sign-in.";
    case Step::CodePair: return "Requesting an Amazon sign-in code...";
    case Step::Register: return "Waiting for approval on Amazon...";
    case Step::Refresh: return "Account linked. Testing token refresh...";
    case Step::LunaPage: return "Refresh passed. Testing a read-only Luna page request...";
    case Step::NegativeControl: return "Luna returned a page. Checking rejection of an invalid token...";
    case Step::Complete: return "PASS: native linking, refresh and Luna token acceptance verified.";
    case Step::Failed: return "Sign-in probe stopped. See status codes below.";
    case Step::Cancelled: return "Sign-in probe cancelled.";
    case Step::AppConfig: return "Sign-in passed. Loading Luna's profile page context...";
    case Step::ProfilesPage: return "Loading your Luna profiles...";
    case Step::ChooseProfile: return "Choose a Luna profile. D-pad: select | A: confirm";
    case Step::PlayerLogin: return "Signing in to the selected profile...";
    case Step::Catalog: return "Loading the selected profile's home-page games...";
    case Step::CatalogReady: return "Profile login passed. Games loaded.";
    case Step::SearchPage: return "Loading catalog results.";
    case Step::BrowsePage: return "Loading Luna's browse page.";
    case Step::CatalogAccess: return "Checking which games are included with your access.";
    case Step::ProfileManagement: return "Managing Luna profiles.";
    case Step::DetailPage: return "Loading the selected game's detail page...";
    case Step::DetailReady: return "Game detail loaded. Launch-action inspection only.";
    case Step::LaunchWidget: return "Reading Luna's launch action (not starting a session)...";
    case Step::ExperimentalPage: return "Reading Luna's experimental-client notice...";
    case Step::ExperimentalReady: return "Review Luna's experimental-client notice.";
    case Step::NativeSession: return "Native session request...";
    case Step::NativeNotice: return "Reading Luna's software-decoding warning...";
    case Step::ClientIdentity: return "Preparing this Luna client's session identity...";
    }
    return "Unknown state";
}
const char* blockLabel(luna::auth::ProfileBlock block)
{
    using luna::auth::ProfileBlock;
    switch (block)
    {
    case ProfileBlock::None: return "";
    case ProfileBlock::PinRequired: return "PIN/setup required; not supported in this probe";
    case ProfileBlock::Migration: return "Profile migration pending or failed";
    case ProfileBlock::NotInFamily: return "Profile is not in this Amazon family";
    case ProfileBlock::ImportRequired: return "Profile creation/import required in Luna";
    }
    return "Unavailable";
}
std::string consoleName(const std::string& text)
{
    // The debug console's font is limited. Keep server text from injecting
    // console controls and limit row width without splitting UTF-8 sequences.
    std::string result;
    for (const unsigned char c : text)
    {
        if ((c & 0xc0) == 0x80) continue;
        result += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
        if (result.size() >= 65) { result += "..."; break; }
    }
    return result;
}
[[maybe_unused]] std::vector<std::string> consoleLines(const std::string& text)
{
    // Preserve the entire notice over pages. HTML stays literal text; it is
    // never executed and its links are never followed by the debug console.
    std::vector<std::string> lines;
    std::string line;
    for (const unsigned char c : text)
    {
        if ((c & 0xc0) == 0x80) continue;
        if (c == '\n') { lines.push_back(std::move(line)); line.clear(); continue; }
        line += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
        if (line.size() == 72) { lines.push_back(std::move(line)); line.clear(); }
    }
    if (!line.empty() || lines.empty()) lines.push_back(std::move(line));
    return lines;
}
std::vector<std::string> noticeLines(const luna::auth::ExperimentalNotice& notice)
{
    // Long response labels must be readable in full before selecting a
    // compact graphical button. Use the same pages for rendering and input.
    auto text = notice.text;
    for (std::size_t i = 0; i < notice.choices.size(); ++i)
        if (notice.choices[i].label.size() > 48)
            text += "\nResponse " + std::to_string(i + 1) + ": " + notice.choices[i].label + "\n";
    std::vector<std::string> lines;
    std::string line;
    std::size_t columns = 0;
    for (std::size_t at = 0; at < text.size();)
    {
        const auto c = static_cast<unsigned char>(text[at]);
        if (c == '\n') { lines.push_back(std::move(line)); line.clear(); columns = 0; ++at; continue; }
        if (columns == 72) { lines.push_back(std::move(line)); line.clear(); columns = 0; }
        const std::size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        line.append(text, at, std::min(bytes, text.size() - at)); at += bytes; ++columns;
    }
    if (!line.empty() || lines.empty()) lines.push_back(std::move(line));
    return lines;
}
const char* errorLabel(luna::auth::Error error)
{
    using luna::auth::Error;
    switch (error)
    {
    case Error::None: return "none";
    case Error::Http: return "HTTP request rejected";
    case Error::Response: return "Unexpected or unsuccessful response";
    case Error::Transport: return "Network/TLS transfer failed";
    case Error::Expired: return "Sign-in deadline expired";
    case Error::AuthorizationNotProven: return "Invalid-token rejection was not verified";
    }
    return "unknown";
}
class Http
{
public:
    struct Completion { std::uint64_t id; CURLcode result; long status; std::string body; };
    Http() : multi_(curl_multi_init()) { if (!multi_) throw std::runtime_error("HTTP initialization failed"); }
    ~Http() { cancel(); curl_multi_cleanup(multi_); }
    bool busy() const { return easy_ != nullptr; }
    void cancel()
    {
        if (easy_) { curl_multi_remove_handle(multi_, easy_); curl_easy_cleanup(easy_); easy_ = nullptr; }
        curl_slist_free_all(headers_); headers_ = nullptr;
        if (request_)
        {
            erase(request_->body);
            for (auto& header : request_->headers) erase(header);
            request_.reset();
        }
        erase(body_);
    }
    void start(luna::auth::Request request)
    {
        if (busy()) throw std::runtime_error("HTTP request already pending");
        request_ = std::move(request);
        limit_ = luna::auth::responseLimit(request_->step);
        easy_ = curl_easy_init();
        if (!easy_) throw std::runtime_error("HTTP handle initialization failed");
        for (const auto& header : request_->headers)
        {
            auto* appended = curl_slist_append(headers_, header.c_str());
            if (!appended) throw std::runtime_error("HTTP header allocation failed");
            headers_ = appended;
        }
        set(CURLOPT_URL, request_->url.c_str());
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, request_->body.c_str());
        set(CURLOPT_POSTFIELDSIZE, static_cast<long>(request_->body.size()));
        set(CURLOPT_HTTPHEADER, headers_);
        set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_SSL_CTX_FUNCTION, importTrustedRoots);
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        set(CURLOPT_TIMEOUT_MS, 15000L);
        set(CURLOPT_FORBID_REUSE, 1L);
        set(CURLOPT_VERBOSE, 0L);
        set(CURLOPT_WRITEFUNCTION, receive);
        set(CURLOPT_WRITEDATA, this);
        if (curl_multi_add_handle(multi_, easy_) != CURLM_OK) throw std::runtime_error("HTTP dispatch failed");
    }
    std::optional<Completion> poll()
    {
        if (!easy_) return std::nullopt;
        int running = 0;
        if (curl_multi_perform(multi_, &running) != CURLM_OK) throw std::runtime_error("HTTP progress failed");
        int remaining = 0;
        while (auto* message = curl_multi_info_read(multi_, &remaining))
        {
            if (message->msg != CURLMSG_DONE || message->easy_handle != easy_) continue;
            long status = 0;
            curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &status);
            Completion result{request_->id, message->data.result, status, std::move(body_)};
            cancel();
            return result;
        }
        return std::nullopt;
    }
private:
    template<typename T> void set(CURLoption option, T value)
    {
        if (curl_easy_setopt(easy_, option, value) != CURLE_OK) throw std::runtime_error("Required HTTP option unavailable");
    }
    static std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context)
    {
        auto& self = *static_cast<Http*>(context);
        if (size && count > (self.limit_ - self.body_.size()) / size) return 0;
        try { self.body_.append(bytes, size * count); return size * count; }
        catch (...) { return 0; }
    }
    CURLM* multi_;
    CURL* easy_ = nullptr;
    curl_slist* headers_ = nullptr;
    std::optional<luna::auth::Request> request_;
    std::string body_;
    std::size_t limit_ = luna::auth::MaxAuthResponse;
};
std::optional<std::string> searchKeyboard(const std::string& initial, std::string& message,
    const char* heading = "Search Luna games", const char* guide = "Game title; leave empty to explore the catalog",
    const char* ok = "Search", unsigned minimum = 0)
{
    message.clear();
    SwkbdConfig keyboard{};
    const auto created = swkbdCreate(&keyboard, 0);
    if (R_FAILED(created))
    {
        printf("Launcher search keyboard unavailable: result=0x%08X.\n", static_cast<unsigned>(created));
        message = "Couldn't open the keyboard. Try again.";
        return std::nullopt;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, heading);
    swkbdConfigSetGuideText(&keyboard, guide);
    swkbdConfigSetOkButtonText(&keyboard, ok);
    swkbdConfigSetStringLenMax(&keyboard, 50);
    swkbdConfigSetStringLenMin(&keyboard, minimum);
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    char result[luna::auth::MaxSearchQueryBytes + 1]{};
    const auto shown = swkbdShow(&keyboard, result, sizeof(result));
    swkbdClose(&keyboard);
    if (R_FAILED(shown))
    {
        printf("Launcher search keyboard closed: result=0x%08X.\n", static_cast<unsigned>(shown));
        return std::nullopt;
    }
    return std::string(result);
}
luna::launcher::Model launcherModel(const luna::auth::NativeAuthProbe& probe,
    const luna::auth::NativeSession* native, std::size_t profile, std::size_t game,
    std::size_t choice, std::size_t page, const std::string& profileName,
    luna::video::Mode mode, bool docked, bool answerApplied, bool runtimeActive)
{
    using Page = luna::launcher::Page;
    using Step = luna::auth::Step;
    using Session = luna::auth::NativeSession;
    luna::launcher::Model m;
    const auto& s = probe.status();
    m.profiles = &probe.profiles(); m.games = &probe.games(); m.detail = &probe.detail();
    m.profileName = s.step == Step::ChooseProfile ? "" : profileName;
    m.video = luna::video::effectiveMode(mode, docked); m.docked = docked;
    m.selectedGame = game; m.selected = profile;
    const auto& browse = probe.browsing();
    m.browsing = browse.active; m.searchQuery = browse.query;
    m.catalogOffset = browse.offset; m.hasNextPage = browse.hasNext;
    m.includedOnly=browse.includedOnly;
    if (browse.error != luna::auth::Error::None)
        m.browseMessage = browse.includedOnly ? "Couldn't verify game access. Press L to show all, or B for Home." :
            "Couldn't load results. Press Y to search again, or B for Home.";
    m.page = Page::Connecting;
    m.title = "Connecting to Luna.";
    m.message = "Getting everything ready for your next game.";
    const luna::auth::ExperimentalNotice* notice = nullptr;
    if (native)
    {
        const auto& n = native->status();
        m.remoteMayRemain = n.remoteWorkMayRemain;
        m.queuePosition = n.position; m.waitSeconds = n.waitSeconds;
        if (n.state == Session::State::SoftwareNotice || n.state == Session::State::EulaNotice)
        {
            m.page = Page::Notice;
            m.title = n.state == Session::State::EulaNotice ? "Game terms of service" : "Before you play";
            notice = &native->softwareNotice(); m.noticeComplete = n.softwareNotice.complete;
        }
        else if (n.state == Session::State::Blocked || n.state == Session::State::Failed)
        {
            m.page = Page::Error; m.title = "Your game couldn't start.";
            m.message = n.state == Session::State::Blocked ?
                "Luna requires a step this version doesn't support yet. Complete any account, parental-control or purchase requirements in Luna before trying again." :
                "Luna couldn't complete this launch. Check your connection and account status. You can return to your games or inspect the debug console for details.";
            if (n.jobError == luna::session::Error::JobFailed)
                m.message = "Luna reported that this game's launch failed without a specific error code. Press B to return and try again. If it repeats, check the same game and profile in Luna's browser client.";
        }
        else if (n.state == Session::State::Cancelled || (answerApplied && !runtimeActive))
        {
            m.page = Page::Ended; m.title = "See you next game.";
            m.message = "The local connection has closed. Press B to return to your games.";
        }
        else
        {
            m.title = n.position ? "You're in the queue." : "Getting your game ready.";
            m.message = probe.detail().title;
            if (n.state == Session::State::CheckingPin) m.message = "Checking your profile's play permissions.";
            if (n.state == Session::State::SoftwarePage) m.message = "Loading Luna's notice. Your confirmation is required before play.";
            if (n.state == Session::State::EulaPage) m.message = "Loading this game's terms. Your confirmation is required before play.";
            if (n.state == Session::State::EulaAccept) m.message = "Saving your acceptance with Luna.";
        }
    }
    else switch (s.step)
    {
    case Step::Idle: m.page = Page::Welcome; break;
    case Step::CodePair:
        m.title = "Getting your sign-in ready."; m.message = "Connecting securely to Amazon."; break;
    case Step::Register: m.page = Page::Link; m.publicCode = s.publicCode; break;
    case Step::ChooseProfile:
        m.page = Page::Profiles;
        if (s.profileBlock != luna::auth::ProfileBlock::None) m.message = blockLabel(s.profileBlock);
        else m.message.clear();
        break;
    case Step::CatalogReady:
        m.page = browse.active || probe.games().empty() ? Page::Library : Page::Home;
        break;
    case Step::SearchPage:
        m.title = "Finding your next adventure."; m.message = "Searching Luna's catalog."; break;
    case Step::BrowsePage:
        m.title = "Exploring Luna."; m.message = "Loading Luna's catalog and browse options."; break;
    case Step::CatalogAccess:
        m.title="Finding your included games.";
        m.message="Checking access with Luna. "+std::to_string(s.catalogParsing.accessChecked)+" games checked."; break;
    case Step::DetailReady:
        m.page = Page::Detail;
        m.canPlay = s.detailParsing.error == luna::auth::Error::None && (s.detailParsing.hasPlayToken ||
            s.detailParsing.action == luna::auth::LaunchAction::DirectLaunch ||
            s.detailParsing.action == luna::auth::LaunchAction::ExperimentalClient);
        break;
    case Step::ExperimentalReady:
        m.page = Page::Notice; m.title = "A note from Luna";
        notice = &probe.notice(); m.noticeComplete = s.noticeParsing.complete; break;
    case Step::Failed: case Step::Cancelled:
        m.page = Page::Error; m.title = s.step == Step::Cancelled ? "Sign-in cancelled." : "We couldn't connect.";
        m.message = "Check your connection and Amazon account, then press B to retry sign-in.";
        if (s.error == luna::auth::Error::Expired)
        {
            m.title = "Your sign-in has expired.";
            m.message = "Sign in again to continue browsing and start another game. Press B to return to sign-in.";
        }
        break;
    case Step::DetailPage:
        m.title = "Opening your game."; m.message = "Loading game details and artwork from Luna."; break;
    case Step::Catalog:
        m.title = "Finding your next adventure."; m.message = "Loading your Luna home games."; break;
    case Step::ProfilesPage: case Step::PlayerLogin:
        m.title = "Making it yours."; m.message = "Loading your Luna profile."; break;
    case Step::ClientIdentity:
        m.title = "Preparing your Luna connection.";
        m.message = "Getting this client's session identity from Luna."; break;
    default: break;
    }
    if (notice)
    {
        const auto lines = noticeLines(*notice);
        m.notice = notice; m.selected = choice;
        m.noticePages = std::max<std::size_t>(1, (lines.size() + 15) / 16);
        m.noticePage = std::min(page, m.noticePages - 1);
        for (std::size_t i = m.noticePage * 16; i < lines.size() && i < (m.noticePage + 1) * 16; ++i)
            m.noticeLines.push_back(lines[i]);
    }
    return m;
}
}

int main()
{
    PrintConsole* console = consoleInit(nullptr);
    (void)console;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    const int consoleFd = dup(STDOUT_FILENO), savedError = dup(STDERR_FILENO);
    int logSocket = -1;
    SocketInitConfig socketConfig = *socketGetDefaultInitConfig();
    socketConfig.sb_efficiency = 8;
    const bool networkReady = R_SUCCEEDED(socketInitialize(&socketConfig));
    const bool randomReady = R_SUCCEEDED(csrngInitialize());
    const bool curlReady = networkReady && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (networkReady && __nxlink_host.s_addr && consoleFd >= 0 && savedError >= 0) logSocket = nxlinkStdio();
    printf("LunaNX v%s by PostTenebrasSoftware. Unofficial Amazon Luna client.\n", LUNANX_AUTH_VERSION);
    auto probe = makeAuthProbe();
    std::unique_ptr<Http> http;
    if (curlReady)
    {
        try { http = std::make_unique<Http>(); }
        catch (...) { printf("HTTP initialization failed.\n"); }
    }
    using NativeSession = luna::auth::NativeSession;
    std::unique_ptr<NativeSession> native;
    NativeRuntime runtime(console);
    LocalShortcuts shortcuts;
    bool exiting = false, answerApplied = false;
    std::int64_t exitDeadline = 0;
    bool sleepChanged = false, previousSleepDisabled = false;
    luna::video::Mode selectedVideo = luna::video::Mode::P720_30;
    const bool romfsReady = R_SUCCEEDED(romfsInit());
    std::unique_ptr<luna::launcher::Renderer> launcher;
    std::unique_ptr<luna::launcher::ArtworkCache> artwork;
    luna::launcher::View launcherView(console);
    if (romfsReady)
        try
        {
            launcher = std::make_unique<luna::launcher::Renderer>("romfs:/fonts/Inter-Regular.ttf", "romfs:/fonts/Inter-SemiBold.ttf");
            if (curlReady) artwork = std::make_unique<luna::launcher::ArtworkCache>(importTrustedRoots);
        }
        catch (...) { printf("Launcher initialization failed; debug console available.\n"); }
    runtime.setVideoHandoff([&] { launcherView.hide(); });
    bool settingsOpen = false, galleryOpen = false, aboutOpen = false, trailerOpen = false, debugConsole = false;
    std::size_t descriptionPage = 0, descriptionPageCount = 1;
    luna::launcher::TrailerPlayer trailer(importTrustedRoots);
    VideoView trailerView(console);
    std::string trailerDisplayError;
    bool trailerSleepChanged = false, trailerPreviousSleep = false;
    std::size_t screenshot = 0;
    std::string profileName, profileId, previousGui;
    luna::launcher::RecentGames recentHistory("sdmc:/switch/LunaNX/recent-games.json");
    bool historyCatalogReady = false;
    std::optional<luna::launcher::RecentGame> pendingPlayedGame;
    std::optional<luna::launcher::RecentGame> playingGame, disconnectedGame, replacementGame;
    std::unique_ptr<NativeSession> disconnectedSession;
    std::string disconnectedProfile, remoteMessage;
    bool remoteFocused = false, confirmEndGame = false;
    unsigned remoteAction = 0;
    int previousEndState = -1;
    std::size_t selectedRecent = 0;
    bool recentFocused = true, homeRecentFocus = true;
    std::unique_ptr<luna::auth::ProfileManager> profileManager;
    std::size_t managedProfile = 0;
    unsigned profileRow = 0;
    bool confirmProfileSave = false;
    std::string profileKeyboardMessage, previousProfileStatus;
    u64 navigationHeld = 0;
    std::int64_t navigationRepeat = 0;
    luna::launcher::PlayIntent playIntent;
    constexpr auto shortcutPath = "sdmc:/switch/LunaNX/shortcut.cfg";
    auto menuButtons = luna::launcher::loadShortcut(shortcutPath);
    luna::launcher::CaptureInput captureInput;
    luna::launcher::MenuShortcut menuShortcut;
    luna::launcher::ShortcutRecorder shortcutRecorder;
    u64 configuredMenuButtons = 0;
    unsigned settingsRow = 0;
    constexpr auto videoPreferencesPath = "sdmc:/switch/LunaNX/video.cfg";
    const auto videoPreferences = luna::launcher::loadVideoPreferences(videoPreferencesPath);
    selectedVideo = videoPreferences.mode;
    auto selectedDecoder = videoPreferences.decoder;
    unsigned selectedBitrate = videoPreferences.bitrateMbps;
    bool selectedOverlay = videoPreferences.statsOverlay;
    std::string videoPreferencesMessage;
    const auto saveVideoSettings = [&]
    {
        videoPreferencesMessage = luna::launcher::saveVideoPreferences(videoPreferencesPath,
            {selectedVideo, selectedDecoder, selectedBitrate, selectedOverlay}) ?
            "Saved. Changes apply to the next game." :
            "Changed for this session; the SD card could not save video settings.";
        previousGui.clear();
    };
    std::string shortcutStatus;
    bool forgetLoginConfirmation = false;
    luna::auth::RememberedLogin rememberedLogin("sdmc:/switch/LunaNX/login.bin", luna::auth::switchLoginKey,
        [&](unsigned char* bytes, std::size_t size) { return randomReady && R_SUCCEEDED(csrngGetRandomBytes(bytes, size)); });
    if (http) rememberedLogin.resume(*probe, nowMs());
    std::optional<luna::auth::LoginMemoryState> previousLoginMemory;
    const auto saveMenuButtons = [&](u64 buttons)
    {
        menuButtons = buttons;
        shortcutStatus = luna::launcher::saveShortcut(shortcutPath, buttons) ?
            "Shortcut saved on the SD card." : "Shortcut changed for this session. The SD card could not save the setting.";
        previousGui.clear();
    };
    std::string previousScreen;
    int transportCode = 0;
    std::size_t selectedProfile = 0, selectedGame = 0, selectedNotice = 0, noticePage = 0;
    std::size_t homeSelection = 0;
    std::string searchMessage;
    const auto retainRemoteGame = [&]
    {
        if (native && native->canEnd() && playingGame)
        {
            disconnectedSession = std::move(native); disconnectedGame = playingGame;
            disconnectedProfile = profileId; remoteFocused = true; remoteAction = 0;
            remoteMessage.clear(); previousEndState = -1;
        }
        playingGame.reset();
    };
    while (true)
    {
        const auto frameStarted = std::chrono::steady_clock::now();
        if (!appletMainLoop()) break;
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (launcher && trailerOpen)
        {
            const bool leave = down & (HidNpadButton_B | HidNpadButton_Plus);
            if (!leave && trailerDisplayError.empty()) trailer.update();
            if (leave || trailer.state() == luna::launcher::TrailerPlayer::State::Finished)
            {
                trailer.stop(); trailerView.hide(); trailerOpen = false; trailerDisplayError.clear();
                if (trailerSleepChanged) { appletSetAutoSleepDisabled(trailerPreviousSleep); trailerSleepChanged = false; }
                previousGui.clear(); previousScreen.clear();
                if (down & HidNpadButton_Plus) break;
                down = 0;
            }
            else
            {
                if (auto image = trailer.takeFrame())
                    try
                    {
                        launcherView.hide();
                        if (!trailerView.show(*image)) throw std::runtime_error("Trailer display unavailable");
                    }
                    catch (...)
                    { trailer.stop(); trailerView.hide(); trailerDisplayError = "Couldn't open the trailer display. Press B to return."; }
                if (trailer.state() == luna::launcher::TrailerPlayer::State::Failed) trailerView.hide();
                if (!trailerView.isVisible())
                {
                    luna::launcher::Model model;
                    model.page = luna::launcher::Page::Trailer;
                    model.trailerFailed = !trailerDisplayError.empty() || trailer.state() == luna::launcher::TrailerPlayer::State::Failed;
                    model.message = trailerDisplayError.empty() ? trailer.message() : trailerDisplayError;
                    model.animation = unsigned(nowMs() / 150);
                    const auto key = "trailer:" + model.message + ":" + std::to_string(model.animation);
                    if (key != previousGui || !launcherView.visible())
                    {
                        launcherView.show(launcher->draw(model, [](const std::string&) -> const luna::launcher::Image* { return nullptr; }));
                        previousGui = key;
                    }
                }
                svcSleepThread(trailerView.isVisible() ? 5'000'000 : 16'666'667);
                continue;
            }
        }
        if (disconnectedSession && disconnectedSession->status().state == NativeSession::State::Ending)
            down = 0;
        else if (confirmEndGame)
        {
            if (down & HidNpadButton_B) { confirmEndGame = false; replacementGame.reset(); }
            else if ((down & HidNpadButton_A) && http && !http->busy())
            {
                if (disconnectedSession && disconnectedSession->endGame(nowMs()))
                { confirmEndGame = false; previousEndState = -1; remoteMessage.clear(); }
            }
            down = 0;
        }
        if (profileManager && profileManager->status().state == luna::auth::ProfileManager::State::Saving)
            down &= ~HidNpadButton_Plus;
        if (launcher && !answerApplied && !confirmEndGame &&
            !(disconnectedSession && disconnectedSession->status().state == NativeSession::State::Ending))
        {
            const auto stick = padGetStickPos(&pad, 0);
            constexpr u64 directions = HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
            u64 held = padGetButtons(&pad) & directions;
            if (stick.x < -16000) held |= HidNpadButton_Left;
            if (stick.x > 16000) held |= HidNpadButton_Right;
            if (stick.y > 16000) held |= HidNpadButton_Up;
            if (stick.y < -16000) held |= HidNpadButton_Down;
            if (held != navigationHeld) { down |= held; navigationHeld = held; navigationRepeat = nowMs() + 350; }
            else if (held && nowMs() >= navigationRepeat) { down |= held; navigationRepeat = nowMs() + 100; }
        }
        // The graphical launcher owns its menu binding; keep the debug toggle
        // but never feed the legacy exit chord its Plus button.
        auto effectiveMenuButtons = menuButtons;
        if (!captureInput.available() && (menuButtons & luna::launcher::CaptureButton))
            effectiveMenuButtons = luna::launcher::FallbackMenuButtons;
        const bool shortcutInputReady = padIsConnected(&pad) && appletGetFocusState() == AppletFocusState_InFocus;
        const bool inGame = native && answerApplied && runtime.active();
        const auto modifiers = effectiveMenuButtons & luna::launcher::PadButtons;
        captureInput.listen(shortcutInputReady && (shortcutRecorder.recording() ||
            (inGame && (effectiveMenuButtons & luna::launcher::CaptureButton) &&
                (padGetButtons(&pad) & modifiers) == modifiers)));
        if (!captureInput.available() && (effectiveMenuButtons & luna::launcher::CaptureButton))
            effectiveMenuButtons = luna::launcher::FallbackMenuButtons;
        if (configuredMenuButtons != effectiveMenuButtons)
        { menuShortcut.configure(effectiveMenuButtons); configuredMenuButtons = effectiveMenuButtons; }
        auto shortcut = shortcuts.update(shortcutRecorder.active() ? 0 : padGetButtons(&pad) & ~HidNpadButton_Plus,
            shortcutInputReady, LocalShortcuts::Clock::now(),
            inGame && luna::launcher::overlayShortcutAvailable(effectiveMenuButtons));
        const bool capturePressed = captureInput.takePress();
        const bool wasRecording = shortcutRecorder.active();
        if (wasRecording)
        {
            const auto result = shortcutRecorder.update(padGetButtons(&pad), capturePressed, shortcutInputReady, nowMs());
            if (result.binding) saveMenuButtons(result.binding);
            else if (result.cancelled) shortcutStatus = "Shortcut recording cancelled.";
            down = 0;
        }
        const auto menu = menuShortcut.update(padGetButtons(&pad), capturePressed,
            shortcutInputReady, inGame, nowMs());
        if (menu.suppress || wasRecording)
        { shortcut.suppressInput = true; shortcut.toggleVideo = shortcut.toggleOverlay = false; }
        if (menu.menu)
        {
            captureInput.listen(false);
            runtime.stop();
            if (http) http->cancel();
            retainRemoteGame();
            native.reset(); answerApplied = false; exiting = false; exitDeadline = 0;
            if (sleepChanged) { appletSetAutoSleepDisabled(previousSleepDisabled); sleepChanged = false; }
            probe->returnToCatalog();
            if (probe->browsing().active) { probe->returnToHome(); selectedGame = homeSelection; }
            recentFocused = true; selectedRecent = 0; pendingPlayedGame.reset();
            settingsOpen = galleryOpen = aboutOpen = debugConsole = false;
            playIntent.cancel(); selectedNotice = noticePage = 0;
            previousScreen.clear(); previousGui.clear(); down = 0;
            printf("Local stream disconnected; returned to the launcher. Remote game may remain active.\n");
        }
        if (shortcut.toggleOverlay)
        {
            runtime.setStatsOverlay(!runtime.statsOverlayEnabled());
            selectedOverlay = runtime.statsOverlayEnabled(); saveVideoSettings();
            printf("Performance overlay: enabled=%d.\n", selectedOverlay);
        }
        if (shortcut.suppressInput) down = 0;
        if (shortcut.toggleVideo) previousScreen.clear();
        if (launcher && shortcut.toggleVideo && (!answerApplied || !runtime.active()))
        {
            debugConsole = !debugConsole; previousGui.clear();
            if (debugConsole) launcherView.hide();
        }
        if (launcher && native && (down & HidNpadButton_B))
        {
            const auto state = native->status().state;
            if (state == NativeSession::State::Failed || state == NativeSession::State::Blocked ||
                state == NativeSession::State::Cancelled || (answerApplied && !runtime.active()))
            {
                runtime.stop(); retainRemoteGame(); native.reset(); answerApplied = false;
                probe->returnToCatalog(); down &= ~HidNpadButton_B;
                selectedNotice = noticePage = 0; previousGui.clear();
                playIntent.cancel();
            }
        }
        if (launcher && answerApplied && !runtime.active() && (down & HidNpadButton_Plus)) break;
        if (native)
        {
            if (!exiting && (native->status().state == NativeSession::State::SoftwareNotice ||
                native->status().state == NativeSession::State::EulaNotice))
            {
                const auto& warning = native->softwareNotice();
                const auto choices = warning.choices.size();
                const auto pages = (noticeLines(warning).size() + 15) / 16;
                if (choices)
                {
                    if (launcher)
                    {
                        const int dx = bool(down & HidNpadButton_Right) - bool(down & HidNpadButton_Left);
                        const int dy = bool(down & HidNpadButton_Down) - bool(down & HidNpadButton_Up);
                        selectedNotice = luna::launcher::navigate(selectedNotice, choices, dx, dy, 2);
                    }
                    else
                    {
                    if (down & HidNpadButton_Down) selectedNotice = (selectedNotice + 1) % choices;
                    if (down & HidNpadButton_Up) selectedNotice = (selectedNotice + choices - 1) % choices;
                    }
                }
                const auto nextPage = launcher ? HidNpadButton_R : HidNpadButton_Right;
                const auto previousPage = launcher ? HidNpadButton_L : HidNpadButton_Left;
                if ((down & nextPage) && noticePage + 1 < pages) ++noticePage;
                if ((down & previousPage) && noticePage > 0) --noticePage;
                if ((down & HidNpadButton_A) && noticePage + 1 == pages)
                {
                    const bool selected = native->status().state == NativeSession::State::EulaNotice ?
                        native->chooseEulaButton(selectedNotice, nowMs()) :
                        native->chooseSoftwareNoticeButton(selectedNotice, nowMs());
                    if (selected) selectedNotice = noticePage = 0;
                }
            }
            if ((down & HidNpadButton_B) && !answerApplied)
            {
                native->cancel(nowMs());
                if (native->status().state == NativeSession::State::Cancelled && http) http->cancel();
            }
            if (shortcut.exit || (!answerApplied && (down & HidNpadButton_Plus)))
            {
                if (!exiting) { exiting = true; exitDeadline = nowMs() + 21'000; native->cancel(nowMs()); }
                if (answerApplied || native->status().state != NativeSession::State::Provisioning) break;
            }
            if (exiting && (native->status().state != NativeSession::State::Provisioning || nowMs() >= exitDeadline)) break;
            down = 0; // Game buttons belong to the streaming controller, not the catalog UI.
        }
        else if (down & HidNpadButton_Plus) break;
        auto step = probe->status().step;
        const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        if (launcher && !native)
        {
            if (profileManager && profileManager->active())
            {
                using Manager=luna::auth::ProfileManager;
                const auto state=profileManager->status().state;
                if (state==Manager::State::Picker)
                {
                    const int dx=bool(down&HidNpadButton_Right)-bool(down&HidNpadButton_Left);
                    const int dy=bool(down&HidNpadButton_Down)-bool(down&HidNpadButton_Up);
                    managedProfile=luna::launcher::navigate(managedProfile,profileManager->profiles().size(),dx,dy,4);
                    if (down&HidNpadButton_A) { profileManager->edit(managedProfile,nowMs()); profileRow=0; }
                    if (down&HidNpadButton_Y) { profileManager->create(nowMs()); profileRow=0; }
                    if (down&HidNpadButton_ZR) profileManager->refresh(nowMs());
                }
                else if (state==Manager::State::Editing)
                {
                    if (confirmProfileSave)
                    {
                        if (down&HidNpadButton_B) confirmProfileSave=false;
                        else if (down&HidNpadButton_A) { profileManager->save(nowMs()); confirmProfileSave=false; }
                        down=0;
                    }
                    else
                    {
                        if ((down&HidNpadButton_Up)&&profileRow) --profileRow;
                        if ((down&HidNpadButton_Down)&&profileRow<2) ++profileRow;
                        if (profileRow==0 && (down&HidNpadButton_A))
                        {
                            if (auto name=searchKeyboard(profileManager->form().name,profileKeyboardMessage,
                                "Luna profile name","Use 1-50 characters, without < or >","OK",1)) profileManager->setName(*name);
                            padUpdate(&pad); down=0;
                        }
                        if (profileRow==1)
                        {
                            const auto& form=profileManager->form();
                            const int dx=bool(down&HidNpadButton_Right)-bool(down&HidNpadButton_Left);
                            profileManager->selectAvatar(luna::launcher::navigate(form.selectedAvatar,form.avatars.size(),dx,0,8));
                        }
                        if (profileRow==2 && (down&HidNpadButton_A) && profileManager->canSave()) confirmProfileSave=true;
                    }
                }
                if ((down&HidNpadButton_B) && state!=Manager::State::Saving)
                {
                    if (http) http->cancel();
                    profileManager->back(nowMs()); confirmProfileSave=false; profileKeyboardMessage.clear();
                    if (!profileManager->active()) profileManager.reset();
                }
                down=0;
            }
            else if (!settingsOpen && step==luna::auth::Step::ChooseProfile && (down&HidNpadButton_Y))
            {
                profileManager=std::make_unique<luna::auth::ProfileManager>(*probe);
                profileManager->begin(nowMs()); managedProfile=0; profileRow=0; confirmProfileSave=false;
                profileKeyboardMessage.clear(); previousProfileStatus.clear(); down=0;
            }
            if (settingsOpen)
            {
                if (down & (HidNpadButton_Up | HidNpadButton_Down)) forgetLoginConfirmation = false;
                if ((down & HidNpadButton_Up) && settingsRow) --settingsRow;
                if ((down & HidNpadButton_Down) && settingsRow < 5) ++settingsRow;
                if (settingsRow == 0 && (down & (HidNpadButton_A | HidNpadButton_Y)))
                {
                    selectedVideo = luna::video::nextMode(selectedVideo, docked);
                    selectedBitrate = luna::video::effectiveBitrate(selectedVideo, selectedBitrate);
                    saveVideoSettings();
                }
                if (settingsRow == 1 && (down & (HidNpadButton_A | HidNpadButton_Y)))
                {
                    selectedDecoder = luna::video::nextDecoder(selectedDecoder);
                    saveVideoSettings();
                }
                if (settingsRow == 2 && (down & HidNpadButton_A))
                {
                    selectedBitrate = luna::video::nextBitrate(luna::video::effectiveMode(selectedVideo, docked), selectedBitrate);
                    saveVideoSettings();
                }
                if (settingsRow == 2 && (down & HidNpadButton_Y))
                { selectedBitrate = 0; saveVideoSettings(); }
                if (settingsRow == 3 && (down & (HidNpadButton_A | HidNpadButton_Y)))
                { selectedOverlay = !selectedOverlay; saveVideoSettings(); }
                if (settingsRow == 4 && (down & HidNpadButton_A))
                { shortcutRecorder.begin(); shortcutStatus.clear(); }
                if (settingsRow == 4 && (down & HidNpadButton_Y)) saveMenuButtons(luna::launcher::DefaultMenuButtons);
                if (settingsRow == 5 && (down & HidNpadButton_A))
                {
                    if (!forgetLoginConfirmation) forgetLoginConfirmation = true;
                    else
                    {
                        forgetLoginConfirmation = false;
                        if (rememberedLogin.forget())
                        {
                            if (http) http->cancel();
                            disconnectedSession.reset(); disconnectedGame.reset(); playingGame.reset();
                            disconnectedProfile.clear(); remoteMessage.clear(); remoteFocused = confirmEndGame = false;
                            probe = makeAuthProbe();
                            if (artwork) artwork->clear();
                            profileName.clear(); profileId.clear(); pendingPlayedGame.reset();
                            selectedProfile = selectedGame = selectedNotice = noticePage = 0;
                            playIntent.cancel(); galleryOpen = false; settingsOpen = false;
                            previousScreen.clear(); previousGui.clear();
                            printf("Saved sign-in removed; signed out locally.\n");
                        }
                    }
                }
                if (down & (HidNpadButton_B | HidNpadButton_X))
                {
                    if (forgetLoginConfirmation) forgetLoginConfirmation = false;
                    else settingsOpen = false;
                }
                down = 0;
            }
            else if (aboutOpen)
            {
                if ((down & (HidNpadButton_Right | HidNpadButton_R | HidNpadButton_Down)) && descriptionPage + 1 < descriptionPageCount) ++descriptionPage;
                if ((down & (HidNpadButton_Left | HidNpadButton_L | HidNpadButton_Up)) && descriptionPage) --descriptionPage;
                if (down & HidNpadButton_B) aboutOpen = false;
                down = 0;
            }
            else if (galleryOpen)
            {
                const auto images = probe->detail().screenshots.size();
                if (images && (down & HidNpadButton_Right)) screenshot = (screenshot + 1) % images;
                if (images && (down & HidNpadButton_Left)) screenshot = (screenshot + images - 1) % images;
                if (down & HidNpadButton_B) galleryOpen = false;
                down = 0;
            }
            else if (step != luna::auth::Step::ExperimentalReady)
            {
                if (down & HidNpadButton_X)
                { settingsOpen = true; forgetLoginConfirmation = false; playIntent.cancel(); down = 0; }
                if (step == luna::auth::Step::DetailReady)
                {
                    if ((down & HidNpadButton_Y) && !probe->detail().description.empty())
                    {
                        aboutOpen = true; descriptionPage = 0;
                        descriptionPageCount = launcher->descriptionPages(probe->detail().description);
                        playIntent.cancel(); down = 0;
                    }
                    if ((down & HidNpadButton_ZL) && !probe->detail().trailers.empty())
                    {
                        if (artwork) artwork->clear();
                        captureInput.listen(false);
                        trailerDisplayError.clear(); trailer.start(probe->detail().trailers.front());
                        trailerOpen = true; playIntent.cancel(); down = 0;
                        if (!trailerSleepChanged)
                        {
                            if (R_SUCCEEDED(appletIsAutoSleepDisabled(&trailerPreviousSleep)) &&
                                R_SUCCEEDED(appletSetAutoSleepDisabled(true))) trailerSleepChanged = true;
                        }
                    }
                    if ((down & HidNpadButton_ZR) && !probe->detail().screenshots.empty())
                    { galleryOpen = true; playIntent.cancel(); screenshot = 0; down = 0; }
                    if (down & HidNpadButton_A)
                    {
                        playIntent.request(selectedGame);
                        down &= ~HidNpadButton_A;
                        down |= probe->status().detailParsing.hasPlayToken ? HidNpadButton_R : HidNpadButton_X;
                    }
                }
            }
            if ((step == luna::auth::Step::Failed || step == luna::auth::Step::Cancelled) && (down & HidNpadButton_B))
            {
                if (http) http->cancel();
                disconnectedSession.reset(); disconnectedGame.reset(); playingGame.reset();
                disconnectedProfile.clear(); remoteMessage.clear(); remoteFocused = confirmEndGame = false;
                probe = makeAuthProbe();
                if (http) rememberedLogin.resume(*probe, nowMs());
                down = 0; profileName.clear(); profileId.clear(); pendingPlayedGame.reset(); playIntent.cancel();
                selectedProfile = selectedGame = selectedNotice = noticePage = 0;
            }
            step = probe->status().step;
            if (!settingsOpen && !galleryOpen && !aboutOpen && step == luna::auth::Step::CatalogReady)
            {
                const auto beginBrowse = [&](const std::string& query, std::size_t offset, bool includedOnly=true)
                {
                    const bool fromHome = !probe->browsing().active;
                    if (probe->browseGames(query, offset, nowMs(), includedOnly))
                    {
                        if (fromHome) { homeSelection = selectedGame; homeRecentFocus = recentFocused; }
                        selectedGame = 0; playIntent.cancel(); searchMessage.clear(); previousGui.clear();
                    }
                };
                if (down & HidNpadButton_Y)
                {
                    if (const auto query = searchKeyboard(probe->browsing().query, searchMessage))
                        beginBrowse(*query, 0,!probe->browsing().active || probe->browsing().includedOnly);
                    down = 0; previousGui.clear(); padUpdate(&pad);
                }
                else if (down & HidNpadButton_R) { beginBrowse("", 0); down = 0; }
                else if (probe->browsing().active && (down & HidNpadButton_L))
                { beginBrowse(probe->browsing().query,0,!probe->browsing().includedOnly); down=0; }
                else if (probe->browsing().active && (down & (HidNpadButton_ZL | HidNpadButton_ZR)))
                {
                    const auto& browse = probe->browsing();
                    const auto start = selectedGame / 8 * 8;
                    if (down & HidNpadButton_ZR)
                    {
                        if (start + 8 < probe->games().size()) selectedGame = start + 8;
                        else if (browse.hasNext) beginBrowse(browse.query, browse.offset + luna::auth::SearchPageSize,browse.includedOnly);
                    }
                    else if (start) selectedGame = start - 8;
                    else if (browse.offset) beginBrowse(browse.query, browse.offset - luna::auth::SearchPageSize,browse.includedOnly);
                    down = 0;
                }
            }
            step = probe->status().step;
        }
        if (!native &&
            !launcher &&
            (down & HidNpadButton_Y))
        {
            selectedVideo = luna::video::nextMode(selectedVideo, docked);
            selectedBitrate = luna::video::effectiveBitrate(selectedVideo, selectedBitrate);
            saveVideoSettings();
        }
        const auto count = step == luna::auth::Step::ChooseProfile ? probe->profiles().size() :
            step == luna::auth::Step::CatalogReady ? probe->games().size() :
            step == luna::auth::Step::ExperimentalReady ? probe->notice().choices.size() : 0;
        auto& selected = step == luna::auth::Step::ChooseProfile ? selectedProfile :
            step == luna::auth::Step::ExperimentalReady ? selectedNotice : selectedGame;
        if (count
            || (launcher && step == luna::auth::Step::CatalogReady && !probe->browsing().active &&
                (!recentHistory.games(profileId).empty() ||
                (disconnectedSession && disconnectedSession->canEnd() && disconnectedGame && disconnectedProfile == profileId)))
            )
        {
            if (launcher)
            {
                if (step == luna::auth::Step::CatalogReady || step == luna::auth::Step::ChooseProfile || step == luna::auth::Step::ExperimentalReady)
                {
                    const int dx = bool(down & HidNpadButton_Right) - bool(down & HidNpadButton_Left);
                    const int dy = bool(down & HidNpadButton_Down) - bool(down & HidNpadButton_Up);
                    if (step == luna::auth::Step::CatalogReady && !probe->browsing().active)
                        luna::launcher::navigateHomeWithSession(remoteFocused, remoteAction, recentFocused, selectedRecent, selectedGame,
                            recentHistory.games(profileId).size(), count,
                            disconnectedSession && disconnectedSession->canEnd() && disconnectedGame && disconnectedProfile == profileId, dx, dy);
                    else selected = luna::launcher::navigate(selected, count, dx, dy,
                        step == luna::auth::Step::ExperimentalReady ? 2 : 4);
                }
            }
            else
            {
            if (down & HidNpadButton_Down) selected = (selected + 1) % count;
            if (down & HidNpadButton_Up) selected = (selected + count - 1) % count;
            }
        }
        if ((down & HidNpadButton_A) && step == luna::auth::Step::ChooseProfile)
        {
            const bool chosen = probe->chooseProfile(selectedProfile, nowMs());
            if (chosen && selectedProfile < probe->profiles().size())
            {
                profileName = probe->profiles()[selectedProfile].name;
                profileId = probe->profiles()[selectedProfile].id;
                selectedRecent = 0; recentFocused = true; pendingPlayedGame.reset();
            }
            selectedGame = 0;
        }
        if ((down & HidNpadButton_A) && step == luna::auth::Step::CatalogReady)
        {
            if (launcher && !probe->browsing().active && remoteFocused && disconnectedSession &&
                disconnectedSession->canEnd() && disconnectedGame && disconnectedProfile == profileId)
            {
                if (!remoteAction)
                {
                    const auto game = *disconnectedGame;
                    if (probe->openGamePath(game.path, game.game.title, nowMs()))
                    {
                        pendingPlayedGame = game; playIntent.request(selectedGame);
                        remoteMessage.clear();
                        printf("Return to game: fetching fresh launch permission for the disconnected game.\n");
                    }
                    else remoteMessage = "Couldn't reopen this game. Try its game page.";
                }
                else { replacementGame.reset(); confirmEndGame = true; }
                down &= ~HidNpadButton_A;
            }
            else
            {
            pendingPlayedGame.reset();
            const auto& recent = recentHistory.games(profileId);
            if (launcher && !probe->browsing().active && !recent.empty() && (recentFocused || probe->games().empty()))
            {
                const auto entry = recent[std::min(selectedRecent, recent.size() - 1)];
                const auto found = std::find_if(probe->games().begin(), probe->games().end(),
                    [&](const auto& game) { return game.id == entry.game.id && game.detailsAvailable; });
                bool opened = false;
                if (found != probe->games().end())
                {
                    opened = probe->openGame(static_cast<std::size_t>(found - probe->games().begin()), nowMs());
                }
                else opened = probe->openGamePath(entry.path, entry.game.title, nowMs());
                if (opened)
                {
                    pendingPlayedGame = entry;
                    pendingPlayedGame->path = probe->currentGamePath();
                }
                down &= ~HidNpadButton_A;
            }
            else
            if (selectedGame < probe->games().size())
            {
                const auto game = probe->games()[selectedGame];
                if (probe->openGame(selectedGame, nowMs()))
                {
                    pendingPlayedGame = luna::launcher::RecentGame{game, probe->currentGamePath()};
                }
            }
            }
        }
        if ((down & HidNpadButton_X) && step == luna::auth::Step::DetailReady)
        {
            selectedNotice = noticePage = 0;
            probe->inspectLaunch(nowMs());
        }
        if (step == luna::auth::Step::ExperimentalReady)
        {
            const auto pages = (noticeLines(probe->notice()).size() + 15) / 16;
            const auto nextPage = launcher ? HidNpadButton_R : HidNpadButton_Right;
            const auto previousPage = launcher ? HidNpadButton_L : HidNpadButton_Left;
            if ((down & nextPage) && noticePage + 1 < pages) ++noticePage;
            if ((down & previousPage) && noticePage > 0) --noticePage;
            if ((down & HidNpadButton_A) && noticePage + 1 == pages)
            {
                if (launcher && selectedNotice < probe->notice().choices.size() &&
                    probe->notice().choices[selectedNotice].actionTarget == "DIALOG_DISMISS")
                { playIntent.cancel(); probe->returnToDetail(); }
                else
                probe->chooseNoticeButton(selectedNotice, nowMs());
            }
        }
        if ((down & HidNpadButton_A) && http && probe->status().step == luna::auth::Step::Idle)
        {
            if (!rememberedLogin.saved() || !rememberedLogin.resume(*probe, nowMs()))
            {
                u8 entropy[16];
                if (randomReady && R_SUCCEEDED(csrngGetRandomBytes(entropy, sizeof(entropy))))
                {
                    entropy[6] = (entropy[6] & 0x0f) | 0x40;
                    entropy[8] = (entropy[8] & 0x3f) | 0x80;
                    char serial[33];
                    for (unsigned i = 0; i < sizeof(entropy); ++i) std::snprintf(serial + i * 2, 3, "%02x", entropy[i]);
                    probe->start(serial, nowMs());
                }
                else printf("Device randomness unavailable. Restart the probe.\n");
            }
        }
        bool continuePlay = false;
        if (launcher && !native && http && !http->busy() && !(down & HidNpadButton_B) &&
            playIntent.needsInspection(probe->status(), selectedGame))
            probe->inspectLaunch(nowMs());
        if (launcher && !native && http && !http->busy() && !(down & HidNpadButton_B))
            continuePlay = playIntent.consume(probe->status(), selectedGame);
        if (!native && ((down & HidNpadButton_R) || continuePlay) && probe->status().step == luna::auth::Step::DetailReady &&
            probe->status().detailParsing.hasPlayToken && http && !http->busy())
        {
            if (launcher && disconnectedSession && disconnectedSession->canEnd() && disconnectedGame &&
                pendingPlayedGame && disconnectedProfile == profileId &&
                disconnectedGame->path != pendingPlayedGame->path &&
                disconnectedGame->game.id != pendingPlayedGame->game.id)
            {
                replacementGame = pendingPlayedGame; confirmEndGame = true; playIntent.cancel();
            }
            else
            {
            native = std::make_unique<NativeSession>(*probe);
            playingGame = pendingPlayedGame;
            if (artwork) artwork->clear(); // Return artwork memory to the game decoder.
            playIntent.cancel();
            try
            {
                const auto mode = luna::video::effectiveMode(selectedVideo,
                    appletGetOperationMode() == AppletOperationMode_Console);
                runtime.prepareVideo(mode, selectedDecoder, selectedBitrate);
                runtime.setStatsOverlay(selectedOverlay);
                const bool hardware = runtime.hardwareDecoderPrepared();
                printf("Native decoder preflight: hardwarePrepared=%d.\n", hardware);
                if (!native->start(nowMs(), hardware)) throw std::runtime_error("Native launch preflight failed");
            }
            catch (...)
            {
                runtime.stop(); native->cancel(nowMs());
                printf("Native decoder preparation failed; launch cancelled.\n");
            }
            selectedNotice = noticePage = 0;
            }
        }
        if (down & HidNpadButton_B)
        {
            playIntent.cancel();
            if ((step == luna::auth::Step::CatalogReady || step == luna::auth::Step::SearchPage ||
                step == luna::auth::Step::BrowsePage || step==luna::auth::Step::CatalogAccess) && probe->browsing().active)
            {
                if (http) http->cancel();
                probe->returnToHome();
                selectedGame = homeSelection; recentFocused = homeRecentFocus; searchMessage.clear();
            }
            else if (step == luna::auth::Step::CatalogReady) probe->returnToProfiles();
            else if (step == luna::auth::Step::ExperimentalPage || step == luna::auth::Step::ExperimentalReady)
            {
                if (http) http->cancel();
                probe->returnToDetail();
            }
            else if (step == luna::auth::Step::DetailPage || step == luna::auth::Step::DetailReady ||
                step == luna::auth::Step::LaunchWidget)
            {
                if (http) http->cancel();
                probe->returnToCatalog();
            }
            else { if (http) http->cancel(); probe->cancel(); }
        }
        try
        {
            if (http)
            {
                if (auto completion = http->poll())
                {
                    transportCode = completion->result;
                    if (completion->result == CURLE_OK)
                    {
                        if (disconnectedSession && disconnectedSession->status().state == NativeSession::State::Ending)
                            disconnectedSession->complete(completion->id, completion->status, completion->body, nowMs());
                        else
                        if (profileManager && profileManager->active()) profileManager->complete(completion->id,completion->status,completion->body,nowMs());
                        else
                        if (native) native->complete(completion->id, completion->status, completion->body, nowMs());
                        else
                        probe->complete(completion->id, completion->status, completion->body, nowMs());
                    }
                    else
                    {
                        if (disconnectedSession && disconnectedSession->status().state == NativeSession::State::Ending)
                            disconnectedSession->transportFailed(completion->id, nowMs());
                        else
                        if (profileManager && profileManager->active()) profileManager->transportFailed(completion->id);
                        else
                        if (native) native->transportFailed(completion->id, nowMs());
                        else
                        probe->transportFailed(completion->id);
                    }
                    erase(completion->body);
                }
                if (native)
                {
                    if (native->status().state == NativeSession::State::AwaitOffer && !runtime.active())
                    {
                        // Capture quality at Play, together with its prepared decoder.
                        auto offer = runtime.createOffer(runtime.videoMode(), selectedDecoder, selectedBitrate);
                        native->supplyOffer(std::move(offer), nowMs(), runtime.videoMode());
                    }
                    if (auto request = native->nextRequest(nowMs())) http->start(std::move(*request));
                    if (auto answer = native->takeAnswer())
                    {
                        if (artwork) artwork->cancel();
                        runtime.applyAnswer(*answer);
                        erase(*answer);
                        answerApplied = true;
                        disconnectedSession.reset(); disconnectedGame.reset(); disconnectedProfile.clear();
                        remoteMessage.clear(); remoteFocused = confirmEndGame = false;
                        printf("Native Luna answer applied to its original Switch peer.\n");
                    }
                    const auto& state = native->status();
                    const bool waiting = state.state == NativeSession::State::Provisioning;
                    if (waiting && !sleepChanged)
                    {
                        bool disabled = false;
                        if (R_SUCCEEDED(appletIsAutoSleepDisabled(&disabled)) &&
                            R_SUCCEEDED(appletSetAutoSleepDisabled(true)))
                        { previousSleepDisabled = disabled; sleepChanged = true; }
                    }
                    if (!waiting && sleepChanged)
                    {
                        appletSetAutoSleepDisabled(previousSleepDisabled); sleepChanged = false;
                    }
                    if (state.state == NativeSession::State::Failed || state.state == NativeSession::State::Cancelled ||
                        state.state == NativeSession::State::Blocked)
                    {
                        http->cancel(); runtime.stop();
                    }
                }
                else
                if (disconnectedSession && disconnectedSession->status().state == NativeSession::State::Ending)
                { if (auto request = disconnectedSession->nextRequest(nowMs())) http->start(std::move(*request)); }
                else
                if (profileManager && profileManager->active())
                { if (auto request=profileManager->nextRequest(nowMs())) http->start(std::move(*request)); }
                else
                if (auto request = probe->nextRequest(nowMs())) http->start(std::move(*request));
                if (probe->status().step == luna::auth::Step::Failed || probe->status().step == luna::auth::Step::Complete)
                    http->cancel();
            }
        }
        catch (...)
        {
            if (http) http->cancel();
            runtime.stop();
            if (native) { native->cancel(nowMs()); exiting = true; exitDeadline = nowMs(); }
            probe->cancel(); printf("Native operation stopped; restart the application.\n");
        }
        if (disconnectedSession)
        {
            const auto state = disconnectedSession->status().state;
            if ((state == NativeSession::State::Ending || state == NativeSession::State::Ended ||
                state == NativeSession::State::EndFailed) && previousEndState != static_cast<int>(state))
            {
                previousEndState = static_cast<int>(state);
                const auto& s = disconnectedSession->status();
                printf("End Game: state=%d HTTP=%ld block=%s api=%lld remoteMayRemain=%d\n",
                    previousEndState, s.httpStatus, NativeSession::blockLabel(s.block),
                    static_cast<long long>(s.apiErrorCode), s.remoteWorkMayRemain);
                if (state == NativeSession::State::Ended)
                {
                    disconnectedGame.reset(); remoteFocused = false;
                    remoteMessage = "Game ended. You can start another game."; searchMessage = remoteMessage;
                    if (replacementGame)
                    {
                        const auto next = *replacementGame; replacementGame.reset();
                        probe->returnToCatalog(); probe->returnToHome();
                        if (probe->openGamePath(next.path, next.game.title, nowMs()))
                        {
                            pendingPlayedGame = next; playIntent.request(selectedGame);
                            printf("Switch game: previous session ended; fetching fresh launch permission.\n");
                        }
                        else searchMessage = "Game ended, but the selected game couldn't be opened. Choose it again.";
                    }
                }
                else if (state == NativeSession::State::EndFailed)
                {
                    if (http) http->cancel();
                    replacementGame.reset(); playIntent.cancel();
                    probe->returnToCatalog(); probe->returnToHome(); remoteFocused = true; remoteAction = 0;
                    remoteMessage = "Couldn't confirm the game ended. It may still be active.";
                }
                previousGui.clear();
            }
        }
        // Completions arrive after input handling. Acknowledge before rendering
        // so a known warning is never shown for a single intermediate frame.
        if (launcher && !native && !settingsOpen && !galleryOpen && !(down & (HidNpadButton_B | HidNpadButton_Plus)) &&
            playIntent.mayAcknowledgeClientWarning(probe->status(), selectedGame) &&
            probe->acknowledgeUnsupportedClient(nowMs()))
        {
            selectedNotice = noticePage = 0;
            previousGui.clear();
            printf("Known unsupported-client warning acknowledged for the requested game.\n");
        }
        rememberedLogin.observe(*probe);
        if (!previousLoginMemory || *previousLoginMemory != rememberedLogin.state())
        {
            printf("Remembered sign-in: state=%d saved=%d restored=%d\n", static_cast<int>(rememberedLogin.state()),
                rememberedLogin.saved(), probe->status().restoredLogin);
            previousLoginMemory = rememberedLogin.state();
        }
        bool mediaToggle = shortcut.toggleVideo;
        if (launcher && !answerApplied) mediaToggle = false;
        try { runtime.update(pad, shortcut.suppressInput || !answerApplied, mediaToggle); }
        catch (...) { runtime.stop(); printf("Native media/input update stopped.\n"); }
        if (native && answerApplied && runtime.hasPresentedFrame() && pendingPlayedGame)
        {
            bool saved = false;
            try
            {
                if (!probe->detail().title.empty()) pendingPlayedGame->game.title = probe->detail().title;
                // Retain the catalog cover; detail artwork is a wide background.
                if (pendingPlayedGame->game.artwork.empty()) pendingPlayedGame->game.artwork = probe->detail().artwork;
                saved = recentHistory.record(profileId, *pendingPlayedGame);
            }
            catch (...) {} // Optional local history must not stop the stream.
            if (!saved)
            {
                searchMessage = "Recently Played couldn't be saved. You can continue playing.";
                const auto historyStatus = recentHistory.saveStatus();
                printf("Recently Played: saved=0 issue=%s error=%d\n",
                    historyStatus.issue == luna::launcher::RecentGames::Issue::None ? "exception" :
                        luna::launcher::RecentGames::issueLabel(historyStatus.issue), historyStatus.error);
            }
            selectedRecent = 0; pendingPlayedGame.reset(); previousGui.clear();
        }
        const auto& status = probe->status();
        const bool catalogReady = status.step == luna::auth::Step::CatalogReady;
        if (catalogReady && !historyCatalogReady)
        {
            try { recentHistory.refreshArtwork(profileId, probe->games()); }
            catch (...) {} // Optional artwork repair must not prevent browsing.
            previousGui.clear();
        }
        historyCatalogReady = catalogReady;
        std::string screen = "\x1b[2J\x1b[HLunaNX - Connection details\n\n";
        screen += http ? label(status.step) : "Network/HTTP initialization unavailable.";
        if (!status.publicCode.empty()) screen += "\n\nOpen https://www.amazon.com/code on your phone or computer.\n\nCode: " + status.publicCode;
        screen += "\n\nRegistered: " + std::string(status.registered ? "yes" : "no") +
            " | Refresh: " + (status.refreshed ? "yes" : "no") + " | Luna page: " + (status.lunaAccepted ? "yes" : "no");
        screen += "\nHTTP: " + std::to_string(status.httpStatus) + " | error: " + std::to_string(static_cast<int>(status.error)) +
            " | transport: " + std::to_string(transportCode);
        if (status.error != luna::auth::Error::None)
            screen += "\n" + std::string(errorLabel(status.error)) + "\nStopped at: " + label(status.failedAt);
        if (status.profileParsing.stage != luna::auth::ProfileParse::None)
            screen += "\nProfile parsing: " + std::string(luna::auth::profileParseLabel(status.profileParsing.stage)) +
                " | widgets: " + std::to_string(status.profileParsing.widgets) +
                " | tiles: " + std::to_string(status.profileParsing.tiles);
        if (status.catalogParsing.stage != luna::auth::CatalogParse::None)
            screen += "\nCatalog parsing: " + std::string(luna::auth::catalogParseLabel(status.catalogParsing.stage)) +
                " | widgets: " + std::to_string(status.catalogParsing.widgets) +
                " | tiles: " + std::to_string(status.catalogParsing.tiles);
        if (status.authError != luna::auth::AuthError::None)
        {
            screen += "\nAmazon error: " + std::string(luna::auth::authErrorLabel(status.authError)) +
                " | parameter: " + luna::auth::authParameterLabel(status.authParameter);
            if (!probe->localAuthErrorIdentifier().empty())
                screen += "\nAmazon identifier (local): " + probe->localAuthErrorIdentifier();
            screen += "\nResponse: " + std::string(luna::auth::authResponseLabel(status.authResponse)) +
                " | bytes: " + std::to_string(status.authResponseBytes);
            screen += "\nApproval polls: " + std::to_string(status.approvalPolls) + " | code age: " +
                std::to_string(status.codeAgeSeconds) + " / " + std::to_string(status.codeLifetimeSeconds) + " s";
        }
        if (status.step == luna::auth::Step::ChooseProfile)
        {
            screen += "\n\n";
            for (std::size_t i = 0; i < probe->profiles().size(); ++i)
            {
                const auto& profile = probe->profiles()[i];
                screen += (i == selectedProfile ? "> " : "  ") + consoleName(profile.name);
                if (profile.block != luna::auth::ProfileBlock::None) screen += " [unavailable]";
                screen += "\n";
            }
            if (status.profileBlock != luna::auth::ProfileBlock::None)
                screen += "\n" + std::string(blockLabel(status.profileBlock)) + "\n";
        }
        if (status.step == luna::auth::Step::CatalogReady)
        {
            if (!probe->games().empty()) selectedGame = std::min(selectedGame, probe->games().size() - 1);
            else selectedGame = 0;
            screen += std::string(probe->browsing().active ? "\n\nCatalog results: " : "\n\nHome-page game tiles: ") +
                std::to_string(probe->games().size()) + "\n";
            const auto start = selectedGame / 10 * 10;
            for (std::size_t i = start; i < probe->games().size() && i < start + 10; ++i)
                screen += (i == selectedGame ? "> " : "  ") + consoleName(probe->games()[i].title) +
                    (probe->games()[i].detailsAvailable ? "\n" : " [no supported detail link]\n");
            if (probe->games().empty()) screen += "No supported game tiles found. See catalog counters.\n";
            screen += probe->browsing().active ? "\nD-pad: browse | A: game details | B: Home\n" :
                "\nD-pad: browse | A: game details | B: choose another profile\n";
            screen += "Catalog visibility does not prove game entitlement.\n";
        }
        if (status.step == luna::auth::Step::DetailReady || status.step == luna::auth::Step::LaunchWidget)
        {
            const auto& detail = probe->detail();
            screen += "\n\n" + consoleName(detail.title) + "\n";
            if (!detail.description.empty()) screen += consoleName(detail.description) + "\n";
            screen += "Artwork: " + std::string(detail.hasArtwork ? "available" : "not supplied") +
                " | Trailer: " + (detail.hasTrailer ? "available" : "not supplied") + "\n";
            if (!detail.buttonText.empty()) screen += "Luna button: " + consoleName(detail.buttonText) + "\n";
            if (!detail.primaryActionTarget.empty()) screen += "Button action (local): " + detail.primaryActionTarget + "\n";
            for (std::size_t i = 0; i < detail.pageActionTargets.size() && i < 4; ++i)
                screen += "Page action (local): " + detail.pageActionTargets[i] + "\n";
            if (detail.pageActionTargets.size() > 4) screen += "Additional page actions omitted from this screen.\n";
            screen += "B: return to game list\n";
            if (status.detailParsing.action == luna::auth::LaunchAction::DirectLaunch && status.detailParsing.error == luna::auth::Error::None)
                screen += "X: fetch launch action (read only)\n";
            else if (status.detailParsing.action == luna::auth::LaunchAction::ExperimentalClient && status.detailParsing.error == luna::auth::Error::None)
                screen += "X: read Luna's experimental-client notice\n";
            else if (status.detailParsing.action == luna::auth::LaunchAction::Other)
                screen += "X unavailable: this button action is not supported yet.\n";
            else if (status.detailParsing.action == luna::auth::LaunchAction::PageAction)
                screen += "X unavailable: a required page action is unresolved.\n";
            else if (status.detailParsing.action == luna::auth::LaunchAction::UnsupportedOs)
                screen += "X unavailable: Luna returned OS_UNSUPPORTED for this client.\n";
            if (status.detailParsing.hasPlayToken)
                screen += "Game ID and play token received; kept private in memory.\n";
        }
        if (status.detailParsing.stage != luna::auth::DetailParse::None || status.detailParsing.error != luna::auth::Error::None)
        {
            screen += "\nDetails: " + std::string(luna::auth::detailParseLabel(status.detailParsing.stage)) +
                " | action: " + luna::auth::launchActionLabel(status.detailParsing.action);
            screen += "\nPage actions: " + std::to_string(status.detailParsing.pageActions) +
                " | unresolved: " + std::to_string(status.detailParsing.blockingPageActions) +
                " | button: " + luna::auth::launchActionLabel(status.detailParsing.primaryAction);
            if (status.detailParsing.error != luna::auth::Error::None)
                screen += "\nDetail read stopped: " + std::string(errorLabel(status.detailParsing.error)) + " Reopen to retry.";
        }
        screen += "\nA: start/select | B: back/cancel | Plus: exit\n";
        screen += "\nApproving the code links this test device to your Amazon account.\nTokens stay in memory; restart requires another sign-in.\n";
        if (status.step == luna::auth::Step::ExperimentalReady)
        {
            const auto& notice = probe->notice();
            const auto lines = noticeLines(notice);
            const auto pages = (lines.size() + 15) / 16;
            screen = "\x1b[2J\x1b[HLunaNX - Luna experimental-client notice\n" + consoleName(probe->detail().title) +
                "\n\nPage " + std::to_string(noticePage + 1) + " / " + std::to_string(pages) + "\n";
            for (std::size_t i = noticePage * 16; i < lines.size() && i < (noticePage + 1) * 16; ++i)
                screen += lines[i] + "\n";
            screen += "\nLuna's buttons (Up/Down to select):\n";
            const auto start = selectedNotice / 4 * 4;
            for (std::size_t i = start; i < notice.choices.size() && i < start + 4; ++i)
                screen += (i == selectedNotice ? "> " : "  ") + consoleName(notice.choices[i].label) +
                    " [" + notice.choices[i].actionTarget + "]\n";
            if (!status.noticeParsing.complete) screen += "Notice has unsupported content/actions; continuation disabled.\n";
            for (std::size_t i = 0; i < notice.unsupportedWidgetTypes.size() && i < 3; ++i)
                screen += "Widget (local): " + notice.unsupportedWidgetTypes[i] + "\n";
            screen += "\nLeft/Right: read pages | B: back | Plus: exit\n";
            screen += noticePage + 1 == pages ? "A: inspect a supported selected action; no game launch.\n" :
                "Read to the last page before selecting a continuation.\n";
        }
        if (!native)
            screen += "\nY: video mode | Requested: " + std::string(luna::video::configuration(
                luna::video::effectiveMode(selectedVideo, docked)).label) +
                (docked ? " | Docked\n" : " | Handheld (1080p locked)\n") +
                "Decoder preference: " + luna::video::decoderLabel(selectedDecoder) + ". Higher modes are unverified.\n";
        if (status.step == luna::auth::Step::DetailReady && status.detailParsing.hasPlayToken && !native)
            screen += "\nR: start one native solo session (allocates a Luna game).\n";
        if (native)
        {
            const auto& n = native->status();
            screen = "\x1b[2J\x1b[HLunaNX - Native session v" LUNANX_AUTH_VERSION "\n\n" + consoleName(probe->detail().title) + "\n\n";
            screen += "State: " + std::to_string(static_cast<int>(n.state)) +
                " | job: " + std::to_string(static_cast<int>(n.jobState)) +
                " | HTTP: " + std::to_string(n.httpStatus) + "\n";
            screen += "Block: " + std::string(NativeSession::blockLabel(n.block)) +
                " | API error: " + std::to_string(n.apiErrorCode) +
                " | job error: " + std::to_string(static_cast<int>(n.jobError)) + "\n";
            if (n.httpDiagnostic.operation != NativeSession::Operation::None)
                screen += "HTTP operation: " + std::string(NativeSession::operationLabel(n.httpDiagnostic.operation)) +
                    "\nError category: " + NativeSession::httpErrorLabel(n.httpDiagnostic.error) +
                    " | field hint: " + NativeSession::errorFieldLabel(n.httpDiagnostic.fieldHint) + "\n";
            screen += "Requested video: " + std::string(luna::video::configuration(runtime.active() || answerApplied ?
                runtime.videoMode() : luna::video::effectiveMode(selectedVideo, docked)).label) + "\n";
            if (n.unsupportedFields)
                screen += "Requirement: " + std::string(NativeSession::requirementLabel(n.requirement)) +
                    " | value type: " + NativeSession::valueKindLabel(n.requirementKind) +
                    " | field mask: " + std::to_string(n.unsupportedFields) + "\n";
            if (n.position) screen += "Queue position: " + std::to_string(*n.position) + "\n";
            if (n.waitSeconds) screen += "Estimated wait: " + std::to_string(*n.waitSeconds) + " seconds\n";
            if (n.refreshing) screen += "Refreshing account access for queue polling...\n";
            if (n.state == NativeSession::State::Blocked)
                screen += "\nThe native launch guard stopped this attempt.\nNo session was created. Copy the Block/Requirement lines for debugging.\n";
            if (n.remoteWorkMayRemain)
                screen += "\nRemote work/session may remain; closing this app does not terminate it.\n";
            screen += answerApplied ?
                "\nStreaming: Xbox button positions (Switch B = Xbox A).\nHold Minus+L: console/video | Menu: " +
                    luna::launcher::shortcutLabel(effectiveMenuButtons) +
                    ((effectiveMenuButtons & luna::launcher::CaptureButton) ? " (hold modifiers, tap Capture)\n" : " (hold two seconds)\n") :
                "\nQueue limit: two hours. B: cancel job | Plus: cancel and exit\n";
            if (exiting) screen += "\nExiting after bounded job cancellation...\n";
            if (n.state == NativeSession::State::SoftwarePage)
                screen += "\nLoading Luna's software-decoding warning...\n";
            if (n.state == NativeSession::State::EulaPage) screen += "\nLoading this game's terms of service...\n";
            if (n.state == NativeSession::State::EulaAccept) screen += "\nSaving your acceptance with Luna...\n";
            if (n.state == NativeSession::State::SoftwareNotice || n.state == NativeSession::State::EulaNotice)
            {
                const auto& warning = native->softwareNotice();
                const auto lines = noticeLines(warning);
                const auto pages = (lines.size() + 15) / 16;
                screen = std::string("\x1b[2J\x1b[HLunaNX - ") +
                    (n.state == NativeSession::State::EulaNotice ? "Game terms of service\n" : "Luna software-decoding warning\n") +
                    consoleName(probe->detail().title) +
                    "\n\nPage " + std::to_string(noticePage + 1) + " / " + std::to_string(pages) + "\n";
                for (std::size_t i = noticePage * 16; i < lines.size() && i < (noticePage + 1) * 16; ++i)
                    screen += lines[i] + "\n";
                screen += "\nLuna's buttons (Up/Down to select):\n";
                const auto start = selectedNotice / 4 * 4;
                for (std::size_t i = start; i < warning.choices.size() && i < start + 4; ++i)
                    screen += (i == selectedNotice ? "> " : "  ") + consoleName(warning.choices[i].label) +
                        " [" + warning.choices[i].actionTarget + "]\n";
                if (!n.softwareNotice.complete)
                    screen += "Unsupported notice content/actions; continuation disabled.\n";
                for (std::size_t i = 0; i < warning.unsupportedWidgetTypes.size() && i < 3; ++i)
                    screen += "Widget (local): " + warning.unsupportedWidgetTypes[i] + "\n";
                if (!n.softwareNotice.complete)
                    screen += "Reason: " + std::string(luna::auth::noticeIssueLabel(n.softwareNotice.firstIssue)) + "\n";
                for (std::size_t i = 0; i < warning.unsupportedActionTargets.size() && i < 3; ++i)
                    screen += "Action (local): " + warning.unsupportedActionTargets[i] + "\n";
                screen += "\nLeft/Right: read pages | B: cancel attempt | Plus: exit\n";
                screen += noticePage + 1 == pages ? "A: select a supported Luna action and continue this attempt.\n" :
                    "Read to the last page before selecting a continuation.\n";
            }
        }
        if (screen != previousScreen && !runtime.visible())
        {
            dprintf(consoleFd >= 0 ? consoleFd : STDOUT_FILENO, "%s", screen.c_str());
            printf("Client identity: ready=%d\n", status.clientIdentityReady);
            // Only fixed stages and status numbers are sent to nxlink.
            printf("Auth probe: step=%d HTTP=%ld error=%d transport=%d registered=%d refreshed=%d luna=%d failedAt=%d profiles=%zu games=%zu profileBlock=%d authError=%s authParameter=%s authResponse=%s responseBytes=%zu polls=%u codeAge=%lld codeLifetime=%lld\n",
                static_cast<int>(status.step), status.httpStatus, static_cast<int>(status.error), transportCode,
                status.registered, status.refreshed, status.lunaAccepted, static_cast<int>(status.failedAt),
                probe->profiles().size(), probe->games().size(), static_cast<int>(status.profileBlock),
                luna::auth::authErrorLabel(status.authError), luna::auth::authParameterLabel(status.authParameter),
                luna::auth::authResponseLabel(status.authResponse), status.authResponseBytes, status.approvalPolls,
                static_cast<long long>(status.codeAgeSeconds), static_cast<long long>(status.codeLifetimeSeconds));
            if (status.profileParsing.stage != luna::auth::ProfileParse::None)
                printf("Profile parser: stage=%s widgets=%zu tiles=%zu encodedActions=%zu parsed=%zu\n",
                    luna::auth::profileParseLabel(status.profileParsing.stage), status.profileParsing.widgets,
                    status.profileParsing.tiles, status.profileParsing.encodedActions, status.profileParsing.parsed);
            if (status.catalogParsing.stage != luna::auth::CatalogParse::None)
                printf("Catalog parser: stage=%s widgets=%zu tiles=%zu horizontal=%zu vertical=%zu parsed=%zu browse=%d navToken=%d navQuery=%d filter=%d defaultOnLoad=%d\n",
                    luna::auth::catalogParseLabel(status.catalogParsing.stage), status.catalogParsing.widgets,
                    status.catalogParsing.tiles, status.catalogParsing.horizontal, status.catalogParsing.vertical,
                    status.catalogParsing.parsed, status.catalogParsing.browse, status.catalogParsing.navigationToken,
                    status.catalogParsing.navigationQuery, status.catalogParsing.filterWidget, status.catalogParsing.defaultSearchOnLoad);
            if (status.catalogParsing.stage==luna::auth::CatalogParse::Access)
                printf("Catalog access: checked=%zu included=%zu hidden=%zu validation=%d fields=%u singleFallback=%d\n",status.catalogParsing.accessChecked,
                    status.catalogParsing.accessIncluded,status.catalogParsing.accessHidden,status.catalogParsing.accessValidation,
                    status.catalogParsing.accessFieldHints,status.catalogParsing.accessSingleFallback);
            if (status.detailParsing.stage != luna::auth::DetailParse::None || status.detailParsing.error != luna::auth::Error::None)
                printf("Detail parser: stage=%s widgets=%zu products=%zu buttons=%zu action=%s error=%d gameId=%d playToken=%d pageActions=%zu blockingPageActions=%zu primaryAction=%s\n",
                    luna::auth::detailParseLabel(status.detailParsing.stage), status.detailParsing.widgets,
                    status.detailParsing.products, status.detailParsing.buttons, luna::auth::launchActionLabel(status.detailParsing.action),
                    static_cast<int>(status.detailParsing.error), status.detailParsing.hasGameId, status.detailParsing.hasPlayToken,
                    status.detailParsing.pageActions, status.detailParsing.blockingPageActions,
                    luna::auth::launchActionLabel(status.detailParsing.primaryAction));
            if (status.noticeParsing.stage != luna::auth::DetailParse::None)
                printf("Notice parser: stage=%s widgets=%zu textBlocks=%zu buttons=%zu unsupported=%zu blockingActions=%zu complete=%d accepted=%d\n",
                    luna::auth::detailParseLabel(status.noticeParsing.stage), status.noticeParsing.widgets,
                    status.noticeParsing.textBlocks, status.noticeParsing.buttons, status.noticeParsing.unsupported,
                    status.noticeParsing.blockingActions, status.noticeParsing.complete, status.noticeParsing.accepted);
            if (native)
            {
                const auto& n = native->status();
                printf("Native session: state=%d block=%s requirement=%s valueType=%s fields=%u job=%d jobError=%d api=%lld HTTP=%ld queue=%lld wait=%lld refresh=%d remoteMayRemain=%d jobResult=%s\n",
                    static_cast<int>(n.state), NativeSession::blockLabel(n.block),
                    NativeSession::requirementLabel(n.requirement), NativeSession::valueKindLabel(n.requirementKind),
                    n.unsupportedFields, static_cast<int>(n.jobState),
                    static_cast<int>(n.jobError), static_cast<long long>(n.apiErrorCode), n.httpStatus,
                    static_cast<long long>(n.position.value_or(-1)), static_cast<long long>(n.waitSeconds.value_or(-1)),
                    n.refreshing, n.remoteWorkMayRemain, luna::session::errorLabel(n.jobError));
                if (n.httpDiagnostic.operation != NativeSession::Operation::None)
                    printf("Native HTTP error: operation=%s category=%s body=%s bytes=%zu fieldHint=%s fieldHints=%u\n",
                        NativeSession::operationLabel(n.httpDiagnostic.operation),
                        NativeSession::httpErrorLabel(n.httpDiagnostic.error),
                        NativeSession::errorBodyLabel(n.httpDiagnostic.body), n.httpDiagnostic.responseBytes,
                        NativeSession::errorFieldLabel(n.httpDiagnostic.fieldHint), n.httpDiagnostic.fieldHints);
                if (n.softwareNotice.stage != luna::auth::DetailParse::None)
                {
                    printf("Launch notice: stage=%s widgets=%zu textBlocks=%zu buttons=%zu unsupported=%zu blockingActions=%zu complete=%d accepted=%d\n",
                        luna::auth::detailParseLabel(n.softwareNotice.stage), n.softwareNotice.widgets,
                        n.softwareNotice.textBlocks, n.softwareNotice.buttons, n.softwareNotice.unsupported,
                        n.softwareNotice.blockingActions, n.softwareNotice.complete, n.softwareNotice.accepted);
                    if (n.softwareNotice.checkboxes)
                        printf("Software checkbox: actionKind=%s optionalPreference=%d leftUnchecked=1\n",
                            luna::auth::noticeActionLabel(n.softwareNotice.checkboxAction),
                            n.softwareNotice.checkboxPreferenceSupported);
                    if (!n.softwareNotice.complete)
                        printf("Software notice issue: reason=%s actionKind=%s button=%zu action=%zu unknownWidgets=%zu checkboxes=%zu legalTerms=%zu secondaryActions=%zu\n",
                        luna::auth::noticeIssueLabel(n.softwareNotice.firstIssue),
                        luna::auth::noticeActionLabel(n.softwareNotice.firstUnsupportedAction),
                        n.softwareNotice.firstIssueButton, n.softwareNotice.firstIssueAction,
                        n.softwareNotice.unsupportedWidgets, n.softwareNotice.checkboxes,
                        n.softwareNotice.legalTerms, n.softwareNotice.secondaryActions);
                }
                if (n.eulaAccepted) printf("Game EULA: acceptance saved.\n");
            }
            previousScreen = std::move(screen);
        }
        if (launcher && !debugConsole && (!answerApplied || !runtime.active() || runtime.waitingForVideo()))
        {
            auto model = launcherModel(*probe, native.get(), selectedProfile, selectedGame,
                selectedNotice, noticePage, profileName, selectedVideo, docked, answerApplied, runtime.active());
            if (!model.profileName.empty())
                for (const auto& profile : probe->profiles())
                    if (profile.id == profileId) { model.profileArtwork = profile.artwork; break; }
            model.menuShortcut = luna::launcher::shortcutLabel(effectiveMenuButtons);
            model.recentGames = &recentHistory.games(profileId);
            model.selectedRecent = selectedRecent; model.recentFocused = recentFocused;
            model.hasRemoteGame = disconnectedSession && disconnectedSession->canEnd() &&
                disconnectedGame && disconnectedProfile == profileId;
            model.remoteFocused = model.hasRemoteGame && remoteFocused; model.remoteAction = remoteAction;
            model.remoteGameTitle = disconnectedGame ? disconnectedGame->game.title : std::string{};
            model.remoteGameArtwork = disconnectedGame ? disconnectedGame->game.artwork : std::string{};
            model.replacementGameTitle = replacementGame ? replacementGame->game.title : std::string{};
            model.remoteMessage = remoteMessage;
            if (!native && status.step == luna::auth::Step::CatalogReady && !probe->browsing().active &&
                (!model.recentGames->empty() || model.hasRemoteGame))
                model.page = luna::launcher::Page::Home;
            if (!searchMessage.empty()) model.browseMessage = searchMessage;
            if (runtime.waitingForVideo())
            {
                model.page = luna::launcher::Page::Connecting;
                model.title = "Starting your game.";
                model.message = "Connected to Luna. Waiting for the first video frame.";
                model.waitingForVideo = true;
            }
            model.rememberedLogin = rememberedLogin.saved() &&
                (rememberedLogin.state() == luna::auth::LoginMemoryState::Saved ||
                 rememberedLogin.state() == luna::auth::LoginMemoryState::Restoring);
            model.signedIn = probe->status().registered;
            model.loginMessage = luna::auth::loginMemoryMessage(rememberedLogin.state());
            if (model.page == luna::launcher::Page::Welcome)
                model.message = model.loginMessage;
            if (!native && probe->status().step == luna::auth::Step::Refresh && probe->status().restoredLogin)
            { model.title = "Welcome back."; model.message = "Refreshing your saved Amazon sign-in."; }
            if (model.page == luna::launcher::Page::Error &&
                rememberedLogin.state() == luna::auth::LoginMemoryState::Rejected)
            { model.title = "Please sign in again."; model.message = model.loginMessage + " Press B to return to sign-in."; }
            if (!http)
            {
                model.page = luna::launcher::Page::Error; model.title = "Network unavailable.";
                model.message = "LunaNX couldn't initialize its network connection. Check the Switch connection and restart the app.";
            }
            if (aboutOpen)
            { model.page = luna::launcher::Page::About; model.descriptionPage = descriptionPage; }
            if (settingsOpen)
            {
                model.page = luna::launcher::Page::Settings;
                model.settingsRow = settingsRow;
                model.decoder = selectedDecoder;
                model.bitrateMbps = selectedBitrate;
                model.statsOverlay = selectedOverlay;
                model.overlayShortcutAvailable = luna::launcher::overlayShortcutAvailable(effectiveMenuButtons);
                model.videoPreferencesMessage = videoPreferencesMessage;
                model.recordingShortcut = shortcutRecorder.recording();
                model.forgetLoginConfirmation = forgetLoginConfirmation;
                model.menuShortcut = luna::launcher::shortcutLabel(effectiveMenuButtons);
                model.shortcutInstructions = (effectiveMenuButtons & luna::launcher::CaptureButton) ?
                    "Hold " + luna::launcher::shortcutLabel(effectiveMenuButtons & ~luna::launcher::CaptureButton) + ", then tap Capture." :
                    "Hold the combination for two seconds.";
                model.shortcutStatus = !captureInput.available() && (menuButtons & luna::launcher::CaptureButton) ?
                    "Capture notifications are unavailable. Hold Minus + Plus for two seconds to return to Home." : shortcutStatus;
            }
            if (galleryOpen) { model.page = luna::launcher::Page::Gallery; model.screenshot = screenshot; }
            if (confirmEndGame) model.page = luna::launcher::Page::EndGame;
            if (disconnectedSession && disconnectedSession->status().state == NativeSession::State::Ending)
            {
                model.page = luna::launcher::Page::Connecting; model.title = "Ending your game.";
                model.message = "Asking Luna to end the remote session. Please wait."; model.profileSaving = true;
            }
            if (profileManager && profileManager->active())
            {
                using Manager=luna::auth::ProfileManager;
                const auto& s=profileManager->status();
                model.profileName.clear(); model.message=Manager::message(s);
                model.profileForm=&profileManager->form(); model.managedProfiles=&profileManager->profiles();
                model.selected=managedProfile; model.profileRow=profileRow;
                model.canCreateProfile=profileManager->canCreate(); model.canSaveProfile=profileManager->canSave();
                model.confirmProfileSave=confirmProfileSave;
                if (!profileKeyboardMessage.empty()) model.message=profileKeyboardMessage;
                if (s.state==Manager::State::Picker) model.page=luna::launcher::Page::ManageProfiles;
                else if (s.state==Manager::State::Editing) model.page=luna::launcher::Page::EditProfile;
                else if (s.state==Manager::State::Saved || s.state==Manager::State::Failed)
                {
                    model.page=luna::launcher::Page::ProfileResult;
                    model.title=s.state==Manager::State::Saved ? "Profile saved." : "We couldn't complete that.";
                }
                else
                {
                    model.page=luna::launcher::Page::Connecting;
                    model.title=s.state==Manager::State::Saving ? "Saving your profile." :
                        s.state==Manager::State::Verifying ? "Verifying your changes." : "Loading profile settings.";
                    model.profileSaving=s.state==Manager::State::Saving;
                }
                const auto key=std::to_string(static_cast<int>(s.state))+":"+std::to_string(static_cast<int>(s.issue))+":"+
                    std::to_string(s.http)+":"+std::to_string(s.api)+":"+std::to_string(s.writeAttempted)+":"+std::to_string(s.blockedBy)+":"+
                    std::to_string(static_cast<int>(profileManager->diagnostics().stage));
                if (key!=previousProfileStatus)
                {
                    printf("Profile manager: state=%d issue=%d HTTP=%ld api=%d write=%d profiles=%zu avatars=%zu blockedBy=%u\n",
                        static_cast<int>(s.state),static_cast<int>(s.issue),s.http,s.api,s.writeAttempted,
                        profileManager->profiles().size(),profileManager->form().avatars.size(),s.blockedBy);
                    const auto& d=profileManager->diagnostics();
                    printf("Profile routes: tiles=%zu edit=%zu create=%zu add=%zu legacy=%zu sourcePins=%zu\n",
                        d.tiles,d.editRoutes,d.createRoutes,d.addRoutes,d.legacyRoutes,d.sourcePins);
                    if (d.stage!=Manager::ParseStage::None)
                        printf("Profile form: stage=%s widgets=%zu forms=%zu avatars=%zu saves=%zu\n",
                            Manager::parseLabel(d.stage),d.formWidgets,d.forms,d.avatars,d.saves);
                    if (s.state==Manager::State::Failed)
                        for (const auto& shape:d.shape)
                            printf("Profile shape: widget=%016llx action=%016llx\n",
                                static_cast<unsigned long long>(shape.widget),static_cast<unsigned long long>(shape.action));
                    previousProfileStatus=key;
                }
            }
            model.animation = static_cast<unsigned>(nowMs() / 180);
            std::vector<std::string> wanted, prefetch;
            if (!native && !model.profileArtwork.empty()) wanted.push_back(model.profileArtwork);
            if (!native && !settingsOpen)
            {
                if (profileManager && profileManager->active())
                {
                    if (model.page==luna::launcher::Page::ManageProfiles)
                    {
                        const auto& list=profileManager->profiles(); const auto start=managedProfile/4*4;
                        for (std::size_t i=start;i<list.size()&&i<start+4;++i) wanted.push_back(list[i].artwork);
                    }
                    else if (model.page==luna::launcher::Page::EditProfile)
                    {
                        const auto& f=profileManager->form(); const auto start=f.selectedAvatar/8*8;
                        for (std::size_t i=start;i<f.avatars.size()&&i<start+8;++i) wanted.push_back(f.avatars[i].artwork);
                    }
                }
                else if (status.step==luna::auth::Step::ChooseProfile)
                {
                    const auto start=selectedProfile/4*4;
                    for (std::size_t i=start;i<probe->profiles().size()&&i<start+4;++i) wanted.push_back(probe->profiles()[i].artwork);
                }
                else if (galleryOpen && screenshot < probe->detail().screenshots.size()) wanted.push_back(probe->detail().screenshots[screenshot]);
                else if (status.step == luna::auth::Step::CatalogReady)
                {
                    if (!probe->browsing().active && model.hasRemoteGame) wanted.push_back(model.remoteGameArtwork);
                    if (!probe->browsing().active && model.recentGames && !model.recentGames->empty())
                    {
                        const auto index = std::min(selectedRecent, model.recentGames->size() - 1);
                        if (recentFocused) wanted.push_back((*model.recentGames)[index].game.artwork);
                        const auto first = index / 4 * 4;
                        for (std::size_t i = first; i < model.recentGames->size() && i < first + 4; ++i)
                            wanted.push_back((*model.recentGames)[i].game.artwork);
                    }
                    if (selectedGame < probe->games().size()) wanted.push_back(probe->games()[selectedGame].artwork);
                    const std::size_t visible = probe->browsing().active ? 8 : 4;
                    const auto start = selectedGame / visible * visible;
                    for (std::size_t i = start; i < probe->games().size() && i < start + visible; ++i) wanted.push_back(probe->games()[i].artwork);
                    // Warm nearby covers as soon as the home response arrives.
                    const auto first = start >= 4 ? start - 4 : 0;
                    for (std::size_t i = first; i < probe->games().size() && i < start + 12; ++i) prefetch.push_back(probe->games()[i].artwork);
                }
                else if (status.step == luna::auth::Step::DetailReady)
                {
                    wanted.push_back(probe->detail().artwork);
                    for (std::size_t i = 0; i < probe->detail().screenshots.size() && i < 3; ++i) wanted.push_back(probe->detail().screenshots[i]);
                }
            }
            if (artwork) artwork->update(wanted, prefetch, http && !http->busy());
            auto key = previousScreen + std::to_string(static_cast<int>(model.page)) + ":" +
                std::to_string(selectedProfile) + ":" + model.profileArtwork + ":" + std::to_string(selectedGame) + ":" +
                std::to_string(screenshot) + ":" + std::to_string(static_cast<int>(selectedVideo)) + ":" +
                std::to_string(descriptionPage) + ":" +
                std::to_string(docked) + ":" + std::to_string(artwork ? artwork->revision() : 0) + ":" +
                std::to_string(settingsRow) + ":" + model.menuShortcut + ":" + model.shortcutStatus + ":" +
                std::to_string(model.recordingShortcut) + ":" + model.loginMessage + ":" +
                std::to_string(model.rememberedLogin) + ":" + std::to_string(model.forgetLoginConfirmation) + ":" +
                std::to_string(static_cast<int>(selectedDecoder)) + ":" + std::to_string(selectedBitrate) + ":" + videoPreferencesMessage +
                ":" + std::to_string(selectedOverlay) + ":" + std::to_string(model.waitingForVideo) + ":" +
                model.searchQuery + ":" + model.browseMessage + ":" + std::to_string(model.catalogOffset) + ":" + std::to_string(model.hasNextPage) + ":" +
                std::to_string(model.includedOnly) + ":" + model.message +
                ":" + std::to_string(selectedRecent) + ":" + std::to_string(recentFocused) + ":" + std::to_string(model.recentGames->size()) +
                ":" + std::to_string(model.hasRemoteGame) + ":" + std::to_string(model.remoteFocused) + ":" +
                std::to_string(remoteAction) + ":" + model.remoteGameTitle + ":" + remoteMessage + ":" + model.replacementGameTitle;
            if (profileManager && profileManager->active())
                key+=":"+previousProfileStatus+":"+std::to_string(managedProfile)+":"+std::to_string(profileRow)+":"+
                    std::to_string(confirmProfileSave)+":"+profileManager->form().name+":"+
                    std::to_string(profileManager->form().selectedAvatar)+":"+model.message;
            if (model.page == luna::launcher::Page::Connecting) key += ":" + std::to_string(model.animation);
            if (key != previousGui || !launcherView.visible())
                try
                {
                    if (model.page == luna::launcher::Page::Detail || model.page == luna::launcher::Page::Gallery)
                    {
                        std::size_t ready = 0;
                        if (artwork) for (const auto& url : probe->detail().screenshots) if (artwork->find(url)) ++ready;
                        printf("Launcher detail images: screenshots=%zu ready=%zu heroReady=%d descriptionBytes=%zu trailers=%zu\n",
                            probe->detail().screenshots.size(), ready, artwork && artwork->find(probe->detail().artwork),
                            probe->detail().description.size(), probe->detail().trailers.size());
                    }
                    const auto& image = launcher->draw(model, [&](const std::string& url)
                    { return artwork ? artwork->find(url) : nullptr; });
                    launcherView.show(image); previousGui = std::move(key);
                }
                catch (...)
                {
                    launcherView.hide(); launcher.reset(); previousScreen.clear();
                    printf("Launcher display stopped; debug console available.\n");
                }
        }
        else if (launcherView.visible()) launcherView.hide();
        if (runtime.visible())
        {
            // Include applet processing and keep one deadline timeline across
            // wake-ups instead of accumulating each iteration's delay.
            runtime.paceFrame(frameStarted);
        }
        else
        if (launcherView.visible()) svcSleepThread(16'666'667);
        else
        consoleUpdate(nullptr);
    }
    runtime.stop(); native.reset();
    if (sleepChanged) appletSetAutoSleepDisabled(previousSleepDisabled);
    disconnectedSession.reset();
    captureInput.listen(false);
    trailer.stop(); trailerView.hide();
    if (trailerSleepChanged) appletSetAutoSleepDisabled(trailerPreviousSleep);
    launcherView.hide(); artwork.reset(); launcher.reset();
    if (romfsReady) romfsExit();
    http.reset();
    probe.reset();
    if (curlReady) curl_global_cleanup();
    fflush(stdout); fflush(stderr);
    if (consoleFd >= 0) { dup2(consoleFd, STDOUT_FILENO); close(consoleFd); }
    if (savedError >= 0) { dup2(savedError, STDERR_FILENO); close(savedError); }
    if (logSocket >= 0) close(logSocket);
    if (randomReady) csrngExit();
    if (networkReady) socketExit();
    consoleExit(nullptr);
    return 0;
}
