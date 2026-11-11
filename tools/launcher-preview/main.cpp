#include "launcher_ui.hpp"
#include "launcher_media.hpp"
#include "launcher_flow.hpp"
#include "launcher_image_cache.hpp"
#include "launcher_shortcut.hpp"
#include "performance_overlay.hpp"
#include <cstring>
#include <fstream>
#include <png.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace luna::launcher;
namespace
{
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
void save(const Image& image, const std::filesystem::path& path)
{
    png_image png{}; png.version = PNG_IMAGE_VERSION;
    png.width = image.width; png.height = image.height; png.format = PNG_FORMAT_RGBA;
    require(png_image_write_to_file(&png, path.c_str(), 0, image.pixels.data(), 0, nullptr), "PNG write failed");
}
}
int main(int argc, char** argv)
{
    const auto root = std::filesystem::current_path();
    const auto output = argc > 1 ? std::filesystem::path(argv[1]) : root / "build-launcher/previews";
    std::filesystem::create_directories(output);
    Renderer renderer((root / "assets/launcher/fonts/Inter-Regular.ttf").string(),
        (root / "assets/launcher/fonts/Inter-SemiBold.ttf").string());
    const auto draw = [&](const Model& model, const char* name)
    {
        const auto& image = renderer.draw(model, [](const std::string&) -> const Image* { return nullptr; });
        require(image.width == 1280 && image.height == 720 && image.pixels.size() == 1280 * 720,
            "All launcher pages must retain bounded RGBA storage");
        save(image, output / name);
    };
    Model m;
    draw(m, "01-welcome.png");
    m.page = Page::Link; m.publicCode = "DEMO-CODE"; draw(m, "02-sign-in.png");
    const std::vector<luna::auth::Profile> profiles = {{"preview1", "Player one", luna::auth::ProfileBlock::None},
        {"preview2", "Player two", luna::auth::ProfileBlock::PinRequired}};
    m.profiles = &profiles; m.page = Page::Profiles; draw(m, "03-profiles.png");
    const std::vector<luna::auth::ProfileManager::Entry> managed={
        {"fixture_profile_1","Player one","",true},{"fixture_profile_2","Protected profile","",false}};
    luna::auth::ProfileManager::Form profileForm;
    profileForm.name="Player one"; profileForm.gamertag="GeneratedGamerTag";
    for (unsigned i=0;i<12;++i) profileForm.avatars.push_back({"fixture_avatar_"+std::to_string(i),"Luna avatar",""});
    m.managedProfiles=&managed; m.canCreateProfile=true; m.page=Page::ManageProfiles;
    draw(m,"32-manage-profiles.png");
    m.profileForm=&profileForm; m.page=Page::EditProfile; m.canSaveProfile=true;
    draw(m,"33-edit-profile-name.png");
    m.profileRow=1; profileForm.selectedAvatar=9; draw(m,"34-edit-profile-avatar.png");
    m.profileRow=2; profileForm.creating=true; profileForm.name="New player"; m.confirmProfileSave=true;
    draw(m,"35-create-profile-confirmation.png");
    m.confirmProfileSave=false; m.page=Page::ProfileResult; m.title="Profile saved.";
    m.message="Saved to your Amazon account and verified."; draw(m,"36-profile-saved.png");
    m.title="We couldn't complete that.";
    m.message="This profile or action needs setup or verification this version doesn't support. Manage it in Luna.";
    draw(m,"37-profile-protected.png");
    m.page=Page::Connecting; m.title="Saving your profile."; m.message="Saving to Amazon. Please wait."; m.profileSaving=true;
    draw(m,"38-profile-saving.png");
    m=Model{}; m.profiles=&profiles;
    const std::vector<luna::auth::Game> games = {
        {"fixture1", "Assassin's Creed Valhalla", true, ""}, {"fixture2", "Sample game two", true, ""},
        {"fixture3", "Sample game three", true, ""}, {"fixture4", "Sample game four", true, ""}};
    m.games = &games; m.page = Page::Home; m.profileName = "Player one"; draw(m, "04-home.png");
    m.profileArtwork = "https://m.media-amazon.com/images/I/fixture-avatar.png";
    const Image avatar{8, 8, std::vector<std::uint32_t>(64, 0xff3298e1)};
    const auto& withAvatar = renderer.draw(m, [&](const std::string& url) -> const Image*
        { return url == m.profileArtwork ? &avatar : nullptr; });
    require(withAvatar.pixels[42 * 1280 + 1222] == avatar.pixels[0], "Header displays the selected profile's loaded avatar");
    save(withAvatar, output / "39-profile-header-avatar.png");
    const auto& withoutAvatar = renderer.draw(m, [](const std::string&) -> const Image* { return nullptr; });
    require(withoutAvatar.pixels[42 * 1280 + 1222] != avatar.pixels[0], "Unavailable avatar retains the initial fallback");
    m.profileArtwork.clear();
    std::vector<luna::auth::Game> catalog;
    for (unsigned i = 0; i < 50; ++i)
        catalog.push_back({"fixture" + std::to_string(i), i % 2 ? "A longer sample game title for grid wrapping" : "Sample catalog game", true, ""});
    m.page = Page::Library; m.games = &catalog; m.browsing = true; m.hasNextPage = true;
    m.selectedGame = 5; draw(m, "23-catalog.png");
    m.searchQuery = u8"Pokémon & adventure"; m.catalogOffset = 50;
    draw(m, "24-search.png");
    m.selectedGame = 49; draw(m, "25-search-last-row.png");
    const std::vector<luna::auth::Game> emptyGames;
    m.games = &emptyGames; m.hasNextPage = false; m.selectedGame = 0; m.catalogOffset = 0;
    draw(m, "26-search-empty.png");
    m.browseMessage = "Couldn't load results. Press Y to search again, or B for Home.";
    draw(m, "27-search-error.png");
    m.games=&catalog; m.browseMessage.clear(); m.includedOnly=true; m.hasNextPage=true;
    draw(m,"40-included-games.png");
    m.games=&emptyGames; draw(m,"41-included-empty-page.png");
    m.browseMessage="Couldn't verify game access. Press L to show all, or B for Home.";
    draw(m,"42-included-access-error.png");
    std::vector<RecentGame> recent;
    for (unsigned i = 0; i < 12; ++i)
        recent.push_back({{"recent" + std::to_string(i), i ? "Recently played adventure " + std::to_string(i) : games[0].title, true, ""},
            "game/fixture/FAKE_ASIN"});
    m = Model{}; m.page = Page::Home; m.games = &games; m.recentGames = &recent; m.profileName = "Player one";
    draw(m, "28-recently-played.png");
    m.recentFocused = false; m.selectedGame = 2; draw(m, "29-home-games-row.png");
    m.recentFocused = true; m.selectedRecent = 5; draw(m, "30-recently-played-carousel.png");
    m.games = &emptyGames; draw(m, "31-recently-played-without-home.png");
    m.games = &games; m.hasRemoteGame = true; m.remoteFocused = true;
    m.remoteGameTitle = games[0].title;
    draw(m, "50-return-to-game.png");
    m.remoteAction = 1; draw(m, "51-end-game-selected.png");
    m.page = Page::EndGame; draw(m, "52-end-game-confirm.png");
    m.replacementGameTitle = games[1].title; draw(m, "53-switch-game-confirm.png");
    m.replacementGameTitle.clear(); m.page = Page::Home; m.games = &emptyGames; m.recentGames = nullptr;
    draw(m, "54-return-to-game-empty-home.png");
    m.games = &games; m.recentGames = &recent;
    m.remoteMessage = "Couldn't confirm the game ended. It may still be active.";
    draw(m, "55-end-game-failed.png");
    bool sessionFocus = true, recentFocus = true;
    unsigned sessionAction = 0; std::size_t ri = 2, gi = 1;
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,12,40,true,1,0);
    require(sessionFocus && sessionAction == 1 && ri == 2, "Right selects End Game without moving the recent selection");
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,12,40,true,0,1);
    require(!sessionFocus && recentFocus && ri == 2, "Down from session controls reaches Recently Played");
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,12,40,true,0,-1);
    require(sessionFocus && sessionAction == 0, "Up from Recently Played reaches Return to game");
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,0,40,true,0,1);
    require(!sessionFocus && !recentFocus, "Session controls remain navigable without history");
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,0,40,true,0,-1);
    require(sessionFocus, "Top game row returns to session controls without history");
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,0,0,true,0,1);
    require(sessionFocus, "Empty catalog cannot strand focus below the only available controls");
    navigateHomeWithSession(sessionFocus,sessionAction,recentFocus,ri,gi,12,40,false,0,1);
    require(!sessionFocus, "Removed session controls cannot retain focus");
    bool focus = true; std::size_t recentIndex = 2, gameIndex = 1;
    navigateHome(focus, recentIndex, gameIndex, 12, 40, 1, 0);
    require(focus && recentIndex == 3 && gameIndex == 1, "Recent row moves independently of the Home selection");
    navigateHome(focus, recentIndex, gameIndex, 12, 40, 0, 1);
    require(!focus && recentIndex == 3 && gameIndex == 1, "Down moves to the main games row");
    navigateHome(focus, recentIndex, gameIndex, 12, 40, 1, 0);
    require(!focus && gameIndex == 2 && recentIndex == 3, "Main row keeps its own position");
    navigateHome(focus, recentIndex, gameIndex, 12, 40, 0, -1);
    require(focus && recentIndex == 3, "Up restores recent-row focus");
    navigateHome(focus, recentIndex, gameIndex, 0, 40, 0, 1);
    require(!focus && gameIndex == 6, "No-history users keep the existing Home navigation");
    navigateHome(focus, recentIndex, gameIndex, 12, 0, 0, 1);
    require(focus, "Empty Home response leaves recent games accessible");
    m = Model{}; m.games = &games; m.profileName = "Player one";
    luna::auth::GameDetail detail;
    detail.title = games[0].title;
    detail.description = "Preview fixture. In the app, this description and the artwork come from the selected game's authenticated Luna page. This preview does not establish catalog availability or access rights.";
    detail.buttonText = "Play now";
    detail.screenshots = {"fixture1", "fixture2", "fixture3"};
    detail.trailers = {"https://m.media-amazon.com/images/S/fixture-trailer.mp4"};
    m.detail = &detail; m.page = Page::Detail; m.canPlay = true; draw(m, "05-detail.png");
    const auto shortDescription = detail.description;
    for (unsigned i = 0; i < 35; ++i) detail.description += " A long synthetic synopsis with words that must remain readable across pages.";
    require(renderer.descriptionPages(detail.description) > 1, "Full game descriptions must remain accessible beyond the detail-page preview");
    m.page = Page::About; draw(m, "43-about-game.png");
    m.descriptionPage = renderer.descriptionPages(detail.description) - 1;
    draw(m, "44-about-game-last-page.png");
    detail.description = shortDescription; m.descriptionPage = 0;
    m.page = Page::Trailer; m.message = "Downloading trailer: 1200 KiB. B returns to game details.";
    draw(m, "45-trailer-loading.png");
    m.trailerFailed = true; m.message = "Couldn't download the trailer. Check your connection or try another game.";
    draw(m, "46-trailer-error.png"); m.trailerFailed = false;
    m.page = Page::Gallery; draw(m, "06-gallery.png");
    luna::auth::ExperimentalNotice notice;
    notice.choices = {{"Continue", "GAME_SESSION_ASYNC", luna::auth::LaunchAction::GameSession},
        {"Cancel", "DIALOG_DISMISS", luna::auth::LaunchAction::Other}};
    m.page = Page::Notice; m.title = "A note from Luna"; m.notice = &notice;
    m.noticeComplete = true;
    for (unsigned i = 0; i < 16; ++i) m.noticeLines.push_back("Preview line " + std::to_string(i + 1) + ": complete notice text is paginated before confirmation.");
    draw(m, "07-notice.png");
    notice.choices[0] = {"Accept", "ACCEPT_EULA", luna::auth::LaunchAction::AcceptEula};
    m.title = "Game terms of service";
    m.noticeLines = {"Terms of Service", "By clicking Accept, you receive a license to play the game and agree",
        "to our Terms of Use [http://www.amazon.com/help/ags/terms].",
        "Please see our Privacy Notice [http://www.amazon.com/help/ags/privacy].",
        "Open these addresses on another device to review the linked documents."};
    draw(m, "47-eula.png");
    m.noticePages = 2; draw(m, "48-eula-read-pages.png"); m.noticePages = 1;
    m.noticeComplete = false; draw(m, "49-eula-unsupported.png"); m.noticeComplete = true;
    m.page = Page::Connecting; m.title = "You're in the queue."; m.message = games[0].title;
    m.queuePosition = 96; m.waitSeconds = 240; draw(m, "08-queue.png");
    m.page = Page::Settings; draw(m, "09-settings.png");
    m.settingsRow = 4; m.recordingShortcut = true; draw(m, "13-shortcut-recording.png");
    m.recordingShortcut = false; m.menuShortcut = "Minus + Plus";
    m.shortcutInstructions = "Hold the combination for two seconds.";
    m.shortcutStatus = "Capture notifications are unavailable. Hold Minus + Plus for two seconds to return to Home.";
    draw(m, "14-shortcut-fallback.png");
    m.page = Page::Settings; m.settingsRow = 5; m.rememberedLogin = m.signedIn = true;
    m.loginMessage = "Sign-in saved for this Switch. Your profile is selected each launch.";
    draw(m, "15-remembered-sign-in.png");
    m.forgetLoginConfirmation = true;
    draw(m, "16-forget-sign-in.png");
    m.forgetLoginConfirmation = false;
    m.settingsRow = 1; draw(m, "17-hardware-decoder.png");
    m.settingsRow = 2; m.video = luna::video::Mode::P720_60; m.bitrateMbps = 20;
    m.videoPreferencesMessage = "Saved. Changes apply to the next game.";
    draw(m, "18-bitrate-limit.png");
    m.video = luna::video::Mode::P1080_60; m.bitrateMbps = 30; m.docked = true;
    draw(m, "19-docked-bitrate.png");
    m.video = luna::video::Mode::P720_30; m.bitrateMbps = 0; m.docked = false;
    m.settingsRow = 3; m.statsOverlay = true; draw(m, "20-overlay-setting.png");
    m.waitingForVideo = true; m.page = Page::Connecting; m.title = "Starting your game.";
    m.message = "Connected to Luna. Waiting for the first video frame."; m.queuePosition.reset(); m.waitSeconds.reset();
    draw(m, "21-first-frame-loading.png"); m.waitingForVideo = false;
    luna::video::PerformanceOverlay overlay((root / "assets/launcher/fonts/Inter-Regular.ttf").string());
    luna::video::PerformanceSample statsSample;
    statsSample.mode = luna::video::Mode::P720_60; statsSample.bitrateMbps = 20;
    statsSample.backend = luna::video::Backend::Nvdec; statsSample.ready = true;
    statsSample.decodedFps = 60; statsSample.presentedFps = 59.7; statsSample.encodedMbps = 15.36;
    statsSample.decodeMs = 2.66; statsSample.transferMs = 1.05; statsSample.rgbaMs = 2.06;
    statsSample.presentMs = 4.84; statsSample.loopMs = 6.28; statsSample.replaced = 1;
    const auto& panel = overlay.draw(statsSample);
    require(panel.width == 540 && panel.height == 182 && panel.stride == 2160 &&
        panel.pixels.size() == 540 * 182 * 4, "Overlay uses bounded compact storage");
    Image overlayPreview{1280, 720, std::vector<std::uint32_t>(1280 * 720, 0xff696441)};
    for (int y = 0; y < panel.height; ++y)
        std::memcpy(overlayPreview.pixels.data() + (y + 16) * 1280 + 16,
            panel.pixels.data() + y * panel.stride, panel.width * 4);
    save(overlayPreview, output / "22-live-stats.png");
    require(overlayPreview.pixels[16 * 1280 + 16] != 0xff696441, "Overlay panel pixels are present in the preview");
    const auto readyPixels = panel.pixels;
    statsSample.ready = false;
    const auto& warmup = overlay.draw(statsSample);
    require(warmup.width == 540 && warmup.height == 182 && warmup.pixels != readyPixels, "Warm-up overlay shows a distinct collecting state within the same bounds");
    require(overlayShortcutAvailable(DefaultMenuButtons) && !overlayShortcutAvailable(MinusButton | (1ull << 7)) &&
        !overlayShortcutAvailable(CaptureButton | MinusButton), "Custom return-shortcut overlaps disable the overlay chord");
    m.page = Page::Error; m.title = "Your game couldn't start.";
    m.message = "Luna couldn't complete this launch. Check your connection and account status, or return to your games.";
    m.remoteMayRemain = true; draw(m, "10-error.png");
    m.page = Page::Ended; m.title = "See you next game.";
    m.message = "The local connection has closed. Press B to return to your games."; draw(m, "11-ended.png");
    m.page = Page::Detail;
    detail.title = "UTF-8: 日本語 / Français / Ελληνικά";
    detail.description = std::string(5000, 'x');
    draw(m, "12-long-text.png");
    // Loaded screenshot pixels must actually appear, independently of HTTP.
    const std::uint32_t green = 0xff31bf24;
    Image sample{64, 36, std::vector<std::uint32_t>(64 * 36, green)};
    const auto& loaded = renderer.draw(m, [&](const std::string&) { return &sample; });
    require(loaded.pixels[600 * 1280 + 878] == green && loaded.pixels[600 * 1280 + 1136] == green,
        "Decoded screenshot images must be drawn into both thumbnail positions");
    require(navigate(0, 0, 1, 1, 4) == 0 && navigate(0, 10, -1, 0, 4) == 0 &&
        navigate(9, 10, 1, 0, 4) == 9 && navigate(2, 10, 0, 1, 4) == 6 &&
        navigate(8, 10, 0, 1, 4) == 9 && navigate(99, 3, 0, -1, 4) == 0,
        "Grid navigation must clamp empty, partial and final rows");
    require(navigate(0, 2, 1, 0, 2) == 1 && navigate(1, 2, -1, 0, 2) == 0 &&
        navigate(0, 4, 0, 1, 2) == 2, "Horizontal notice responses and multirow choices match their visual layout");
    PlayIntent play;
    luna::auth::Status state;
    state.step = luna::auth::Step::DetailReady;
    state.detailParsing.action = luna::auth::LaunchAction::ExperimentalClient;
    play.request(3);
    require(play.needsInspection(state,3) && !play.needsInspection(state,4),
        "Return to game can fetch fresh launch permission only for the explicitly requested game");
    require(!play.consume(state, 3), "Opening a notice must not create a session");
    state.step = luna::auth::Step::ExperimentalReady;
    require(!play.consume(state, 3), "A displayed notice is not consent");
    state.detailParsing.primaryAction = luna::auth::LaunchAction::ExperimentalClient;
    state.noticeParsing.complete = true;
    require(play.mayAcknowledgeClientWarning(state, 3) && !play.mayAcknowledgeClientWarning(state, 4),
        "Known client-warning acknowledgment requires the requested game");
    play.cancel();
    require(!play.mayAcknowledgeClientWarning(state, 3), "Opening a notice without a pending Play never acknowledges it");
    play.request(3); state.noticeParsing.complete = false;
    require(!play.mayAcknowledgeClientWarning(state, 3), "Unsupported/required notice content is never auto-acknowledged");
    state.noticeParsing.complete = true;
    state.detailParsing.primaryAction = luna::auth::LaunchAction::PageAction;
    require(!play.mayAcknowledgeClientWarning(state, 3), "The client-warning policy does not apply to other dialogs");
    state.step = luna::auth::Step::DetailReady; state.detailParsing.hasPlayToken = true;
    require(play.consume(state, 3) && !play.consume(state, 3), "Confirmed continuation consumes the original Play exactly once");
    play.request(3); play.cancel(); require(!play.consume(state, 3), "Back/Cancel cannot auto-launch later");
    play.request(3); require(!play.consume(state, 4) && !play.consume(state, 3), "Game navigation invalidates previous Play intent");
    play.request(3); state.detailParsing.error = luna::auth::Error::Response;
    require(!play.consume(state, 3), "Failed launch reads do not auto-retry");
    state.detailParsing.error = luna::auth::Error::None;
    require(!play.consume(state, 3), "A later response does not revive a failed Play");
    require(artworkUrl("https://m.media-amazon.com/images/I/example.jpg") &&
        artworkUrl("https://images-na.ssl-images-amazon.com/images/G/example.png"), "Known HTTPS image hosts");
    for (const auto* url : {"https://m.media-amazon.com.evil.invalid/images/a.jpg", "http://m.media-amazon.com/images/a.jpg",
        "https://m.media-amazon.com@evil.invalid/images/a.jpg", "https://m.media-amazon.com/images/a.jpg?token=secret",
        "https://127.0.0.1/private", "file:///tmp/secret", "https://m.media-amazon.com/images/a\nb.jpg"})
        require(!artworkUrl(url), "Untrusted/local/credential-bearing artwork must be ignored");
    const std::string original = "https://m.media-amazon.com/images/I/fixture._SX200_FMwebp_.jpg";
    const auto optimized = artworkRequestUrl(original);
    require(optimized == "https://m.media-amazon.com/images/I/fixture._SX960_QL85_FMjpg_.jpg" &&
        artworkRequestUrl(optimized) == optimized, "Replace existing CDN transforms with bounded JPEG, idempotently");
    require(artworkRequestUrl("https://evil.invalid/fixture.jpg").empty(), "Image transformations cannot bypass origin checks");
    const auto cacheRoot = argc > 2 ? std::filesystem::path(argv[2]) : output / "offline-cache-test";
    // The shortcut must not steal single game buttons, fire repeatedly, or
    // carry held buttons across the game-to-launcher boundary.
    MenuShortcut shortcut;
    shortcut.configure(DefaultMenuButtons);
    require(!shortcut.update(0, true, true, true, 0).menu, "Capture alone remains a screenshot");
    require(!shortcut.update(PlusButton, false, true, true, 1).suppress, "Plus alone remains a game button");
    require(!shortcut.update(PlusButton, true, true, false, 2).menu, "The game shortcut is inactive in menus");
    require(shortcut.update(PlusButton, true, true, true, 3).menu, "Hold Plus and tap Capture opens Home");
    auto action = shortcut.update(PlusButton, true, true, false, 4);
    require(!action.menu && action.suppress, "A fired menu shortcut stays neutral until release on Home");
    require(!shortcut.update(0, false, true, false, 5).suppress, "Release restores launcher input");
    shortcut.configure(FallbackMenuButtons);
    require(shortcut.update(FallbackMenuButtons, false, true, true, 100).suppress, "A held chord neutralizes game input");
    require(!shortcut.update(FallbackMenuButtons, false, true, true, 2099).menu, "A regular chord needs two full seconds");
    require(shortcut.update(FallbackMenuButtons, false, true, true, 2100).menu, "The held fallback returns Home");
    action = shortcut.update(PlusButton, false, true, false, 2200);
    require(!action.menu && action.suppress, "Partial release cannot leak Plus into the launcher Exit action");
    require(!shortcut.update(0, false, true, false, 2300).suppress, "All chord buttons must release");
    shortcut.update(FallbackMenuButtons, false, true, true, 3000);
    require(shortcut.update(MinusButton, false, true, true, 4500).suppress, "Cancelled holds stay neutral through release");
    require(!shortcut.update(FallbackMenuButtons, false, true, true, 5000).menu, "Rejoining a chord restarts its hold");
    require(shortcut.update(FallbackMenuButtons, false, true, true, 7000).menu, "A restarted hold can complete");
    require(!shortcut.update(FallbackMenuButtons, false, false, true, 8000).suppress, "Disconnect clears stale held buttons");
    shortcut.update(FallbackMenuButtons, false, true, true, 10000);
    require(!shortcut.update(FallbackMenuButtons, false, true, true, 9000).menu &&
        !shortcut.update(FallbackMenuButtons, false, true, true, 10999).menu &&
        shortcut.update(FallbackMenuButtons, false, true, true, 11000).menu, "Clock rollback restarts a timed hold");

    ShortcutRecorder recorder;
    recorder.begin();
    require(!recorder.update(1, false, true, 0).binding, "The A button that opens recording cannot become the binding");
    recorder.update(0, false, true, 1);
    recorder.update(FallbackMenuButtons, false, true, 2);
    require(!recorder.update(FallbackMenuButtons, false, true, 601).binding, "Recording waits for a stable combination");
    require(recorder.update(FallbackMenuButtons, false, true, 602).binding == FallbackMenuButtons,
        "A stable two-button combination is recorded");
    require(recorder.active() && !recorder.recording(), "Recorded buttons stay blocked through release");
    recorder.update(PlusButton, false, true, 603);
    require(recorder.active(), "Partial release cannot reactivate settings");
    recorder.update(0, false, true, 604);
    require(!recorder.active(), "Full release ends the recording cooldown");
    recorder.begin(); recorder.update(0, false, true, 0);
    require(recorder.update(PlusButton, true, true, 1).binding == DefaultMenuButtons, "Capture combinations can be recorded");
    recorder.update(0, false, true, 2); recorder.begin(); recorder.update(0, false, true, 3);
    recorder.update(1ull << 1, false, true, 4);
    require(recorder.update(0, false, true, 5).cancelled, "Pressing and releasing B alone cancels recording");
    recorder.begin(); recorder.update(0, false, true, 0);
    recorder.update(1ull << 1, false, true, 1);
    require(recorder.update(1ull << 1, true, true, 2).binding == ((1ull << 1) | CaptureButton),
        "B can be held as a Capture modifier without cancelling");
    recorder.update(0, false, true, 5); recorder.begin();
    require(recorder.update(0, false, false, 6).cancelled && !recorder.active(), "Controller disconnect cancels recording");
    require(!validShortcut(PlusButton) && !validShortcut(PadButtons) && !validShortcut((1ull << 40) | PlusButton) &&
        validShortcut(DefaultMenuButtons), "Only combinations of two through four supported buttons are accepted");
    const auto settingsFile = (cacheRoot.parent_path() / "shortcut.cfg").string();
    require(loadShortcut(settingsFile) == DefaultMenuButtons, "Missing settings use Capture + Plus");
    require(saveShortcut(settingsFile, FallbackMenuButtons) && loadShortcut(settingsFile) == FallbackMenuButtons,
        "Shortcut settings survive a saved round trip");
    require(saveShortcut(settingsFile, DefaultMenuButtons) && loadShortcut(settingsFile) == DefaultMenuButtons,
        "Reset replaces the previous saved binding");
    require(!saveShortcut(settingsFile, PlusButton), "A single button cannot overwrite saved settings");
    { std::ofstream file(settingsFile); file << "LUNANX_SHORTCUT_V1\n0000000000000400\n"; }
    require(loadShortcut(settingsFile) == DefaultMenuButtons, "An invalid stored mask falls back to the default");
    { std::ofstream file(settingsFile); file << std::string(65, 'x'); }
    require(loadShortcut(settingsFile) == DefaultMenuButtons, "Oversized settings are rejected");
    require(!saveShortcut(settingsFile + "/child", FallbackMenuButtons), "Unavailable SD settings fail without blocking use");
    ImageDiskCache cache(cacheRoot.string(), 2048);
    const std::vector<unsigned char> bytes(1000, 73);
    require(cache.load(original).empty(), "A missing SD cache entry is a normal miss");
    cache.save(original, bytes);
    require(cache.load(original) == bytes, "Public image bytes survive a disk-cache round trip");
    const auto another = "https://m.media-amazon.com/images/I/another.jpg";
    cache.save(another, bytes);
    require(cache.load(another) == bytes, "Newly cached artwork is retained under the disk budget");
    std::uintmax_t total = 0;
    for (const auto& file : std::filesystem::directory_iterator(cacheRoot)) total += file.file_size();
    require(total <= 2048, "Disk artwork cache evicts entries to enforce its byte budget");
    cache.save("https://evil.invalid/fixture.jpg", bytes);
    require(cache.load("https://evil.invalid/fixture.jpg").empty(), "Disk cache refuses untrusted origins");
    for (const auto& file : std::filesystem::directory_iterator(cacheRoot))
    { std::ofstream corrupt(file.path(), std::ios::binary | std::ios::trunc); corrupt << "broken"; }
    require(cache.load(another).empty(), "Truncated cache files cannot supply malformed image storage");
    ImageDiskCache unavailable((cacheRoot / "not-a-directory/child").string());
    { std::ofstream file(cacheRoot / "not-a-directory"); file << 'x'; }
    unavailable.save(original, bytes);
    require(unavailable.load(original).empty(), "Unavailable/full/read-only cache paths remain cache misses");
    std::cout << "PASS: 55 native-renderer previews including Return to game, End Game, switch confirmation, empty Home and termination failure; profile/catalog/navigation/one-shot Play/cache regressions.\n";
}
