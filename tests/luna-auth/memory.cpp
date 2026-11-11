#include "remembered_login.hpp"
#include <nlohmann/json.hpp>
#include <mbedtls/gcm.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace luna::auth;
using Json = nlohmann::json;
namespace
{
unsigned checks = 0, randomCalls = 0;
const std::string Serial = "123456781234423481231234567890ab";
const std::string Refresh = "SYNTHETIC_REFRESH_TOKEN";
void require(bool condition, const char* message)
{ ++checks; if (!condition) throw std::runtime_error(message); }
bool key(RememberedLogin::Key& output) { output.fill(0x42); return true; }
bool randomBytes(unsigned char* out, std::size_t size)
{ ++randomCalls; for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<unsigned char>(randomCalls + i); return true; }
Request request(NativeAuthProbe& auth, Step step, std::int64_t now = 4000)
{
    auto result = auth.nextRequest(now);
    require(result && result->step == step, "Expected an authentication request");
    return std::move(*result);
}
void registerAccount(NativeAuthProbe& auth, const std::string& refresh = Refresh)
{
    auth.start(Serial, 0);
    auto req = request(auth, Step::CodePair, 0);
    auth.complete(req.id, 200, Json{{"public_code", "FAKE-CODE"}, {"private_code", "FAKE_PRIVATE"},
        {"expires_in", 600}, {"polling_interval_in_seconds", 1}}.dump(), 0);
    req = request(auth, Step::Register, 1000);
    auth.complete(req.id, 200, Json{{"response", {{"success", {{"tokens", {{"bearer",
        {{"access_token", "FAKE_ACCESS"}, {"refresh_token", refresh}, {"expires_in", 3600}}}}}}}}}}.dump(), 1000);
    require(auth.status().step == Step::Refresh && auth.status().registered, "Fixture registration succeeded");
}
std::string read(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::string& path, const std::string& data)
{ std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(data.data(), data.size()); }
}
int main(int argc, char** argv)
{
    require(argc == 2, "Tests require a temporary directory");
    require(mbedtls_gcm_self_test(0) == 0, "Vendored GCM passes its known-answer vectors");
    const auto root = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(root);
    const auto path = (root / "login.bin").string();
    RememberedLogin memory(path, key, randomBytes);
    NativeAuthProbe fresh(Mode::ProfilesAndCatalog);
    require(!memory.resume(fresh, 0) && !memory.saved() && fresh.status().step == Step::Idle,
        "A first launch waits for explicit code sign-in");
    registerAccount(fresh);
    memory.observe(fresh);
    require(memory.saved() && memory.state() == LoginMemoryState::Saved, "Approved registration saves login");
    const auto file = read(path);
    for (const auto& secret : {Refresh, Serial, std::string("FAKE_ACCESS"), std::string("FAKE_PRIVATE"), std::string("FAKE-CODE")})
        require(file.find(secret) == std::string::npos, "Saved file contains no plaintext identity, tokens or codes");
    require(!std::filesystem::exists(path + ".tmp"), "Successful commit leaves no temporary file");
    const auto calls = randomCalls;
    memory.observe(fresh); memory.observe(fresh);
    require(randomCalls == calls && read(path) == file, "A credential revision writes once, not every frame");

    NativeAuthProbe resumed(Mode::ProfilesAndCatalog);
    require(memory.resume(resumed, 2000) && resumed.status().restoredLogin, "A restart restores saved login");
    auto req = request(resumed, Step::Refresh);
    require(req.url == "https://api.amazon.com/auth/token" && Json::parse(req.body)["source_token"] == Refresh,
        "Resume refreshes against Amazon without creating a code pair or device");
    require(resumed.status().publicCode.empty() && !resumed.status().refreshed, "Restoration alone is not authorization");
    resumed.complete(req.id, 200, Json{{"access_token", "NEW_ACCESS"}, {"expires_in", 3600},
        {"refresh_token", "ROTATED_REFRESH"}}.dump(), 4000);
    memory.observe(resumed);
    require(memory.state() == LoginMemoryState::Saved && read(path) != file, "Rotated refresh token is saved with a new nonce");
    const auto rotatedFile = read(path);
    req = request(resumed, Step::LunaPage, 4001);
    bool identity = false, access = false;
    for (const auto& header : req.headers)
    {
        identity |= header == "x-amz-device-serial-number: " + Serial;
        access |= header == "x-amz-access-token: NEW_ACCESS";
    }
    require(identity && access, "Saved device identity is reused with a fresh access token");
    resumed.complete(req.id, 200, Json{{"pageMemberGroups", {{"mainContent", {{"widgets", Json::array()}}}}}}.dump(), 4001);
    req = request(resumed, Step::NegativeControl, 4002);
    resumed.complete(req.id, 403, "{}", 4002);
    require(resumed.status().step == Step::AppConfig, "Restored login retains token-acceptance and profile-selection flow");
    NativeAuthProbe rotated(Mode::ProfilesAndCatalog);
    require(memory.resume(rotated, 5000), "A rotated credential reopens");
    req = request(rotated, Step::Refresh, 5000);
    require(Json::parse(req.body)["source_token"] == "ROTATED_REFRESH", "Next launch uses the rotated refresh token");
    rotated.transportFailed(req.id);
    memory.observe(rotated);
    require(read(path) == rotatedFile && memory.saved(), "A network error never deletes saved login");

    for (const auto& response : std::vector<std::pair<long, std::string>>{
        {500, R"({"error":"server_error"})"}, {400, R"({"error":"invalid_request"})"},
        {200, "malformed"}, {200, R"({"access_token":"valid","expires_in":3600,"refresh_token":42})"}})
    {
        NativeAuthProbe temporary(Mode::ProfilesAndCatalog);
        require(memory.resume(temporary, 0), "Retry can reuse saved login");
        req = request(temporary, Step::Refresh);
        temporary.complete(req.id, response.first, response.second, 4000);
        memory.observe(temporary);
        require(memory.saved() && read(path) == rotatedFile, "Temporary/malformed responses retain the last usable saved credential");
    }
    NativeAuthProbe denied(Mode::ProfilesAndCatalog);
    require(memory.resume(denied, 0), "Rejected-login fixture opens");
    req = request(denied, Step::Refresh);
    denied.complete(req.id, 400, R"({"error":"invalid_grant"})", 4000);
    memory.observe(denied);
    require(!memory.saved() && memory.state() == LoginMemoryState::Rejected && !std::filesystem::exists(path),
        "Explicit revoked refresh authorization removes the rejected login");
    require(!denied.nextRequest(4001), "A revoked login never silently registers a new device");

    // Restore the encrypted fixture for tampering and wrong-console checks.
    write(path, file);
    RememberedLogin otherConsole(path, [](RememberedLogin::Key& value) { value.fill(0x99); return true; }, randomBytes);
    NativeAuthProbe wrong(Mode::ProfilesAndCatalog);
    require(!otherConsole.resume(wrong, 0) && wrong.status().step == Step::Idle && !wrong.nextRequest(0),
        "A different console key cannot restore or send stored credentials");
    for (std::size_t index : {std::size_t(0), std::size_t(8), std::size_t(20), std::size_t(24), file.size() - 1})
    {
        auto corrupt = file; corrupt[index] ^= 1; write(path, corrupt);
        NativeAuthProbe invalid(Mode::ProfilesAndCatalog);
        require(!memory.resume(invalid, 0) && !invalid.nextRequest(0),
            "Modified magic, nonce, length, ciphertext and tag are rejected before network access");
    }
    for (const auto& corrupt : {file.substr(0, 20), file.substr(0, file.size() - 1), file + "trailing", std::string(40000, 'x')})
    {
        write(path, corrupt);
        NativeAuthProbe invalid(Mode::ProfilesAndCatalog);
        require(!memory.resume(invalid, 0), "Truncated, oversized and trailing files are rejected");
    }
    write(path, file);
    RememberedLogin noKey(path, [](RememberedLogin::Key&) { return false; }, randomBytes);
    NativeAuthProbe unavailable(Mode::ProfilesAndCatalog);
    require(!noKey.resume(unavailable, 0) && read(path) == file && noKey.state() == LoginMemoryState::Unavailable,
        "Unavailable console encryption leaves saved data intact");
    RememberedLogin noRandom(path, key, [](unsigned char*, std::size_t) { return false; });
    NativeAuthProbe saveFailure(Mode::ProfilesAndCatalog); registerAccount(saveFailure);
    noRandom.observe(saveFailure);
    require(noRandom.state() == LoginMemoryState::SessionOnly && read(path) == file,
        "Random failure never overwrites a working encrypted credential");
    const auto unavailablePath = (root / "regular-file/child").string();
    write((root / "regular-file").string(), "fixture");
    RememberedLogin noStorage(unavailablePath, key, randomBytes);
    noStorage.observe(saveFailure);
    require(noStorage.state() == LoginMemoryState::SessionOnly && saveFailure.status().step == Step::Refresh,
        "SD write failure does not stop authenticated in-memory operation");
    write(path + ".tmp", "abandoned encrypted fixture");
    require(memory.forget() && !std::filesystem::exists(path) && !std::filesystem::exists(path + ".tmp"),
        "Forget removes both committed and interrupted-save files");
    require(memory.forget(), "Forgetting an already empty login is safe");
    std::cout << "PASS: " << checks << " encrypted sign-in storage, refresh/rotation, recovery and privacy checks.\n";
}
