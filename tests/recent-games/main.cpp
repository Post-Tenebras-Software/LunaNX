#include "launcher_recent.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace luna::launcher;
using Json = nlohmann::json;
namespace
{
unsigned checks = 0;
void require(bool test, const char* message) { ++checks; if (!test) throw std::runtime_error(message); }
RecentGame game(unsigned n)
{
    return {{"FAKE_GAME_" + std::to_string(n), u8"A game — Pokémon " + std::to_string(n), true,
        "https://m.media-amazon.com/images/I/fixture.jpg"}, "game/fixture/FAKE_ASIN_" + std::to_string(n)};
}
bool failInstall = false, failRecovery = false;
void switchRename(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
{
    error.clear();
    if ((failInstall && from.extension() == ".tmp") || (failRecovery && from.extension() == ".bak"))
    { error = std::make_error_code(std::errc::io_error); return; }
    // libnx fsdev_rename forwards to fsFsRenameFile without POSIX overwrite.
    if (std::filesystem::exists(to)) { error = std::make_error_code(std::errc::file_exists); return; }
    std::filesystem::rename(from, to, error);
}
std::string read(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char** argv)
{
    require(argc == 2, "Fixture directory required");
    const auto path = (std::filesystem::path(argv[1]) / "saved/recent.json").string();
    RecentGames history(path);
    require(history.games("FAKE_PROFILE_A").empty(), "First install has no invented play history");
    require(history.record("FAKE_PROFILE_A", game(1)) && history.record("FAKE_PROFILE_A", game(2)), "Save played games");
    auto updated = game(1); updated.game.title = "Updated title";
    require(history.record("FAKE_PROFILE_A", updated), "Replay succeeds");
    require(history.games("FAKE_PROFILE_A").size() == 2 && history.games("FAKE_PROFILE_A")[0].game.title == "Updated title" &&
        history.games("FAKE_PROFILE_A")[1].game.id == "FAKE_GAME_2", "Replay moves the game to front without duplicates and updates display data");
    require(history.record("FAKE_PROFILE_B", game(99)), "Separate profile save succeeds");
    RecentGames restored(path);
    require(restored.games("FAKE_PROFILE_A").size() == 2 && restored.games("FAKE_PROFILE_A")[0].path == updated.path &&
        restored.games("FAKE_PROFILE_B").size() == 1 && restored.games("FAKE_PROFILE_C").empty(),
        "Restart retains MRU order/routes and never shows another profile's history");
    auto cover = game(2).game;
    cover.artwork = "https://m.media-amazon.com/images/I/catalog-cover.jpg";
    cover.title = "Unrelated catalog display title";
    auto emptyCover = game(1).game; emptyCover.artwork.clear();
    auto unsafeCover = game(1).game; unsafeCover.artwork = "https://evil.invalid/cover.jpg";
    auto unrelatedCover = game(3).game;
    require(restored.refreshArtwork("FAKE_PROFILE_A", {cover, emptyCover, unsafeCover, unrelatedCover}),
        "Existing hero artwork is repaired from a matching catalog cover");
    require(restored.games("FAKE_PROFILE_A").size() == 2 &&
        restored.games("FAKE_PROFILE_A")[0].game.id == "FAKE_GAME_1" &&
        restored.games("FAKE_PROFILE_A")[0].game.artwork == updated.game.artwork &&
        restored.games("FAKE_PROFILE_A")[1].game.artwork == cover.artwork &&
        restored.games("FAKE_PROFILE_A")[1].game.title == game(2).game.title &&
        restored.games("FAKE_PROFILE_A")[1].path == game(2).path,
        "Artwork repair preserves MRU order, routes and titles and ignores missing/unsafe covers");
    const auto repairedBytes = read(path);
    require(restored.refreshArtwork("FAKE_PROFILE_A", {cover}) && read(path) == repairedBytes,
        "Unchanged catalog artwork does not rewrite history");
    cover.id = "FAKE_GAME_99";
    require(restored.refreshArtwork("FAKE_PROFILE_A", {cover}) &&
        restored.refreshArtwork("FAKE_PROFILE_C", {cover}) && read(path) == repairedBytes,
        "Artwork repair cannot add plays or update another profile's entries");
    RecentGames repaired(path);
    require(repaired.games("FAKE_PROFILE_A")[1].game.artwork ==
        "https://m.media-amazon.com/images/I/catalog-cover.jpg" &&
        repaired.games("FAKE_PROFILE_B")[0].game.artwork == game(99).game.artwork,
        "Repaired covers survive restart with profile isolation");
    for (unsigned i = 0; i < 40; ++i) require(history.record("FAKE_PROFILE_A", game(i)), "Bounded append succeeds");
    require(history.games("FAKE_PROFILE_A").size() == MaxRecentGames && history.games("FAKE_PROFILE_A")[0].game.id == "FAKE_GAME_39" &&
        history.games("FAKE_PROFILE_A").back().game.id == "FAKE_GAME_28", "Oldest games are evicted at the twelve-game cap");
    for (unsigned i = 0; i < 10; ++i) require(history.record("FAKE_PROFILE_" + std::to_string(i), game(i)), "Profile append succeeds");
    require(history.games("FAKE_PROFILE_A").empty() && history.games("FAKE_PROFILE_1").empty() &&
        history.games("FAKE_PROFILE_2").size() == 1 && history.games("FAKE_PROFILE_9").size() == 1,
        "Profile history also has a bounded MRU cap");
    std::ifstream input(path); const Json data = Json::parse(input);
    require(data.size() == 2 && data.at("profiles").size() == MaxRecentProfiles, "Only bounded versioned profiles persist");
    for (const auto& p : data.at("profiles")) for (const auto& e : p.at("games"))
        require(e.size() == 4 && e.contains("id") && e.contains("title") && e.contains("path") && e.contains("artwork"),
            "Only display metadata and public navigation persist, never session or login credentials");
    for (const auto& bad : {"", "https://evil.invalid/game/foo", "//game/foo", "game/../foo", "game/foo?token=FAKE_SECRET",
        "game/foo%2fbar", "settings/foo", "game/", "game//foo", "/game/foo"})
    {
        auto invalid = game(0); invalid.path = bad;
        require(!recentGamePath(bad) && !history.record("FAKE_PROFILE_9", invalid), "Reject unsafe or noncanonical game paths");
    }
    auto invalid = game(0); invalid.game.title = "bad\nconsole";
    require(!history.record("FAKE_PROFILE_9", invalid), "Control characters do not enter history");
    invalid = game(0); invalid.game.artwork = "https://evil.invalid/image.jpg";
    require(!history.record("FAKE_PROFILE_9", invalid), "History images retain the artwork origin restriction");
    require(!history.record("", game(0)) && !history.record("bad\nprofile", game(0)), "Reject invalid profile keys");
    const auto broken = (std::filesystem::path(argv[1]) / "broken.json").string();
    std::vector<std::string> corrupt = {"", "not json", "[]", "{\"version\":1,\"version\":1,\"profiles\":[]}", std::string(512 * 1024 + 1, 'x')};
    auto unknown = data; unknown["version"] = 2; corrupt.push_back(unknown.dump());
    unknown = data; unknown["access_token"] = "FAKE_SECRET"; corrupt.push_back(unknown.dump());
    unknown = data; unknown["profiles"].push_back(unknown["profiles"][0]); corrupt.push_back(unknown.dump());
    unknown = data; unknown["profiles"][0]["games"].push_back(unknown["profiles"][0]["games"][0]); corrupt.push_back(unknown.dump());
    unknown = data; unknown["profiles"][0]["games"][0]["path"] = "https://evil.invalid/"; corrupt.push_back(unknown.dump());
    for (const auto& bytes : corrupt)
    {
        { std::ofstream out(broken, std::ios::binary); out.write(bytes.data(), bytes.size()); }
        RecentGames bad(broken);
        for (unsigned i = 0; i < 10; ++i) require(bad.games("FAKE_PROFILE_" + std::to_string(i)).empty(),
            "Corrupt/oversized/unknown history is ignored atomically");
    }
    const auto blocked = std::filesystem::path(argv[1]) / "not-a-directory";
    { std::ofstream out(blocked); out << "fixture"; }
    RecentGames unavailable((blocked / "recent.json").string());
    require(!unavailable.record("FAKE_PROFILE_A", game(0)) && unavailable.games("FAKE_PROFILE_A").size() == 1,
        "SD write failure retains current-session history without preventing play");
    cover = game(0).game; cover.artwork = "https://m.media-amazon.com/images/I/catalog-cover.jpg";
    require(!unavailable.refreshArtwork("FAKE_PROFILE_A", {cover}) &&
        unavailable.games("FAKE_PROFILE_A")[0].game.artwork == cover.artwork &&
        unavailable.saveStatus().issue == RecentGames::Issue::Directory,
        "Artwork repair reports a failed SD write but keeps the repaired cover for this session");
    require(!std::filesystem::exists(path + ".tmp"), "Successful save leaves no temporary file");
    const auto switchPath = (std::filesystem::path(argv[1]) / "switch/recent.json").string();
    RecentGames switchHistory(switchPath, switchRename);
    require(switchHistory.record("FAKE_PROFILE_A", game(1)) && switchHistory.record("FAKE_PROFILE_A", game(2)) &&
        switchHistory.record("FAKE_PROFILE_A", game(1)), "Repeated saves work with Switch rename semantics");
    RecentGames switchRestored(switchPath, switchRename);
    require(switchRestored.games("FAKE_PROFILE_A").size() == 2 &&
        switchRestored.games("FAKE_PROFILE_A")[0].game.id == "FAKE_GAME_1",
        "Switch-compatible updates survive restart with the expected MRU order");
    const auto committed = read(switchPath);
    failInstall = true;
    require(!switchHistory.record("FAKE_PROFILE_A", game(3)) &&
        switchHistory.saveStatus().issue == RecentGames::Issue::Replace && switchHistory.saveStatus().error != 0 &&
        read(switchPath) == committed && switchHistory.games("FAKE_PROFILE_A")[0].game.id == "FAKE_GAME_3",
        "Failed installation restores the committed history while retaining this session's latest game");
    require(!std::filesystem::exists(switchPath + ".tmp") && !std::filesystem::exists(switchPath + ".bak"),
        "Successful rollback leaves no partial replacement or backup");
    failRecovery = true;
    require(!switchHistory.record("FAKE_PROFILE_A", game(4)) &&
        switchHistory.saveStatus().issue == RecentGames::Issue::Recovery &&
        !std::filesystem::exists(switchPath) && read(switchPath + ".bak") == committed,
        "A rollback failure preserves the previous complete file in its backup");
    failInstall = failRecovery = false;
    RecentGames recovered(switchPath, switchRename);
    require(read(switchPath) == committed && recovered.games("FAKE_PROFILE_A").size() == 2 &&
        !std::filesystem::exists(switchPath + ".bak"), "Restart recovers a previous incomplete file replacement");
    std::filesystem::rename(switchPath, switchPath + ".bak");
    { std::ofstream partial(switchPath + ".tmp"); partial << "interrupted write"; }
    RecentGames interrupted(switchPath, switchRename);
    require(interrupted.games("FAKE_PROFILE_A").size() == 2 && read(switchPath) == committed &&
        interrupted.record("FAKE_PROFILE_A", game(5)), "An interrupted save recovers the old file and can save again");
    require(interrupted.saveStatus().issue == RecentGames::Issue::None &&
        !std::filesystem::exists(switchPath + ".tmp") && !std::filesystem::exists(switchPath + ".bak"),
        "A successful save clears diagnostic failures and staging files");
    invalid = game(6); invalid.path = "settings/not-a-game";
    require(!interrupted.record("FAKE_PROFILE_A", invalid) && interrupted.saveStatus().issue == RecentGames::Issue::Route,
        "Metadata validation reports its fixed stage without paths, IDs or account data");
    require(unavailable.saveStatus().issue == RecentGames::Issue::Directory,
        "An unavailable directory is distinguished from rejected metadata or replacement errors");
    std::cout << "PASS: " << checks << " recent-games storage, profile isolation, MRU, bounds and privacy checks.\n";
}
