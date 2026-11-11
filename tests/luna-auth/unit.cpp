#include "native_auth.hpp"
#include "native_profiles.hpp"
#include "native_session.hpp"
#include "launcher_text.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

using namespace luna::auth;
using Json = nlohmann::json;
namespace
{
unsigned checks = 0;
void require(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
constexpr const char* Serial = "123456781234423481231234567890ab";
const Json Code = {{"public_code", "ABCD-1234"}, {"private_code", "FAKE_PRIVATE"},
                  {"expires_in", 300}, {"polling_interval_in_seconds", "2"}};
const Json Bearer = {{"access_token", "FAKE_ACCESS"}, {"refresh_token", "FAKE_REFRESH"}, {"expires_in", "3600"}};
const Json Registered = {{"response", {{"success", {{"tokens", {{"bearer", Bearer}}}, {"customer_id", "FAKE_CUSTOMER"}}}}}};
const Json Page = {{"pageMemberGroups", {{"mainContent", {{"widgets", Json::array()}}}}}};
Request request(NativeAuthProbe& probe, Step step, std::int64_t time)
{
    auto result = probe.nextRequest(time);
    require(result.has_value(), "Expected a request");
    require(result->step == step, "Unexpected request step");
    std::vector<std::string> userAgents;
    for (const auto& header : result->headers)
        if (header.compare(0, 12, "User-Agent: ") == 0) userAgents.push_back(header);
    if (result->url.rfind("https://proxy-prod.us-east-1.tempo.digital.a2z.com/", 0) == 0)
    {
        require(userAgents.size() == 1 && userAgents[0].find("Mozilla/5.0 (X11; Linux x86_64)") != std::string::npos &&
            userAgents[0].find("Chrome/156.0.0.0") != std::string::npos && userAgents[0].find("LunaNX/") != std::string::npos,
            "Every Luna proxy read, including the invalid-token control, uses one identified browser-compatibility User-Agent");
        const auto payload = Json::parse(result->body);
        const bool presentationRead = result->url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getPage" ||
            result->url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getWidget";
        if (presentationRead)
        {
            const Json context = {{"browserMetadata", {{"browserClientRole", "browser"}, {"browserType", "Chrome"},
                {"browserVersion", "156.0.0.0"}, {"deviceModel", "unknown"}, {"deviceType", "unknown"},
                {"osName", "Linux"}, {"osVersion", "unknown"}}}, {"refMarker", nullptr}, {"referrer", nullptr}};
            require(payload.at("clientContext") == context,
                "Page/widget reads carry the browser's source-derived clientContext body, including string role and OS metadata");
            require(userAgents[0].find("Chrome/" + payload["clientContext"]["browserMetadata"]["browserVersion"].get<std::string>()) != std::string::npos,
                "HTTP and JSON browser versions cannot drift independently");
            require(payload.at("dynamicFeatures") == Json::array(),
                "Auth probe advertises no unimplemented streaming/controller features");
            if (step == Step::LaunchWidget)
                require(!payload.contains("inputContext"), "getWidget wrapper does not inject getPage's inputContext");
            else require(payload.at("inputContext") == Json{{"gamepadTypes", Json::array()}},
                "getPage includes the source-derived input context without claiming a forwarding controller");
        }
        else require(!payload.contains("clientContext") && !payload.contains("dynamicFeatures") && !payload.contains("inputContext"),
            "Generic app-config and profile-login request shapes stay unchanged");
    }
    else require(userAgents.empty(), "Browser compatibility profile does not change Amazon registration/refresh requests");
    return std::move(*result);
}
Json routeBody(const Request& req)
{
    auto body = Json::parse(req.body);
    // The request helper verifies the shared wrapper fields independently;
    // route-specific assertions below inspect only the selected navigation.
    for (const auto* field : {"clientContext", "dynamicFeatures", "inputContext"}) body.erase(field);
    return body;
}
void link(NativeAuthProbe& probe)
{
    probe.start(Serial, 0);
    auto code = request(probe, Step::CodePair, 0);
    const auto registration = Json::parse(code.body).at("code_data");
    require(code.url == "https://api.amazon.com/auth/create/codepair", "Fixed HTTPS auth origin");
    require(registration["device_model"] == "Nintendo Switch (LunaNX)", "Honest device model");
    require(registration["os_version"] == "Horizon", "Browser protocol compatibility never changes the registered Switch OS");
    require(registration["device_name"] == std::string("LunaNX Switch ") + Serial,
        "Name contains this attempt's complete random serial instead of a reusable name or template");
    require(registration["device_serial"] == Serial, "Fresh serial reaches registration");
    require(registration["device_type"] == "A1MPSLFC7L5AFK", "Source-derived generic registration");
    require(!probe.nextRequest(1), "No overlapping code request");
    probe.complete(code.id + 999, 200, Code.dump(), 0);
    require(probe.status().step == Step::CodePair, "Ignore stale completion");
    probe.complete(code.id, 200, Code.dump(), 0);
    require(probe.status().publicCode == "ABCD-1234", "Only public code exposed");
    require(!probe.nextRequest(1999), "Honor server polling interval");
    auto poll = request(probe, Step::Register, 2000);
    const auto payload = Json::parse(poll.body);
    require(payload["registration_data"] == registration, "Registration identity stays bound");
    require(payload["auth_data"]["code_pair"]["private_code"] == "FAKE_PRIVATE", "Private code goes only to register");
    require(payload["requested_token_type"] == Json::array({"bearer"}), "Bearer requested");
    require(probe.status().approvalPolls == 1 && !probe.nextRequest(2001) && probe.status().approvalPolls == 1,
        "Approval counter counts requests without overlapping or early polling");
    probe.complete(poll.id, 401, "not approved", 2000);
    require(probe.status().step == Step::Register && !probe.nextRequest(3999), "401 waits without an early poll");
    poll = request(probe, Step::Register, 4000);
    require(Json::parse(poll.body)["registration_data"] == registration,
        "Unique name and registration identity stay unchanged across approval polls");
    probe.complete(poll.id, 200, Registered.dump(), 4000);
    require(probe.status().registered && probe.status().publicCode.empty(), "Registered status omits codes and tokens");
    require(probe.status().step == Step::Refresh, "Refresh is tested before declaring success");
    require(probe.status().approvalPolls == 2 && probe.status().codeAgeSeconds == 4 && probe.status().codeLifetimeSeconds == 300,
        "Approval timing contains only age, server lifetime and request count");
}
void refresh(NativeAuthProbe& probe)
{
    auto token = request(probe, Step::Refresh, 4000);
    const auto payload = Json::parse(token.body);
    require(payload["source_token"] == "FAKE_REFRESH" && payload["source_token_type"] == "refresh_token", "Refresh uses refresh token");
    probe.complete(token.id, 200, Json{{"access_token", "FAKE_NEW_ACCESS"}, {"expires_in", 3600}}.dump(), 4100);
    require(probe.status().refreshed && probe.status().step == Step::LunaPage, "Refreshed access token ready for Luna");
}
Json tile(const char* id, const char* name, Json extra = Json::object(), const char* target = "PLAYER_SELECT")
{
    Json data = {{"profileId", id}, {"primaryText", name}, {"isLocked", false}};
    data.update(extra);
    return {{"type", "PLAYER_SELECTION_TILE"}, {"presentationData", data.dump()},
        {"actions", Json::array({Json{{"target", target}, {"payload", Json{{"profileId", id}}.dump()}}})}};
}
Json profilePage(Json tiles)
{
    // The public profile screen uses nested widgets with JSON presentationData.
    return {{"pageMemberGroups", {{"mainContent", {{"widgets", Json::array({
        Json{{"type", "COLLECTION"}, {"widgets", std::move(tiles)}}})}}}}}};
}
void config(NativeAuthProbe& probe)
{
    link(probe); refresh(probe);
    auto page = request(probe, Step::LunaPage, 4100);
    probe.complete(page.id, 200, Page.dump(), 4200);
    auto control = request(probe, Step::NegativeControl, 4200);
    probe.complete(control.id, 403, "control", 4300);
    require(probe.status().step == Step::AppConfig, "Catalog mode continues only after negative control passes");
}
void profiles(NativeAuthProbe& probe, Json tiles = Json::array({tile("FAKE_PROFILE_1", "Player One"), tile("FAKE_PROFILE_2", "Player Two")}))
{
    config(probe);
    auto app = request(probe, Step::AppConfig, 4300);
    require(app.url.find("/getAppConfig") != std::string::npos && Json::parse(app.body).empty(), "Source-derived empty app-config request");
    probe.complete(app.id, 200, Json{{"profilePageContext", {{"pageType", "FAKE_PICKER_TYPE"}, {"pageId", "FAKE_PICKER_ID"}}}}.dump(), 4400);
    auto picker = request(probe, Step::ProfilesPage, 4400);
    require(Json::parse(picker.body)["pageContext"] == Json{{"pageType", "FAKE_PICKER_TYPE"}, {"pageId", "FAKE_PICKER_ID"}}, "Picker context comes from server, not invented endpoint");
    probe.complete(picker.id, 200, profilePage(std::move(tiles)).dump(), 4500);
}
bool hasHeader(const Request& req, const std::string& value)
{
    for (const auto& header : req.headers) if (header == value) return true;
    return false;
}
Json navigableGame(const char* id = "FAKE_TILE_ID", const char* target = "/game/fake/FAKE_ASIN")
{
    return {{"type", "GAME_TILE_VERTICAL"}, {"presentationData", Json{{"gameId", id}, {"title", "Fake Game"}}.dump()},
        {"actions", Json::array({Json{{"target", target}, {"serviceToken", "FAKE_PAGE_TOKEN"}}})}};
}
void catalogReady(NativeAuthProbe& probe, Json tiles = Json::array({navigableGame()}))
{
    profiles(probe);
    require(probe.chooseProfile(0, 4600), "Select profile before game details");
    auto login = request(probe, Step::PlayerLogin, 4600);
    probe.complete(login.id, 200, Json{{"token", "FAKE_PROFILE_TOKEN"}}.dump(), 4700);
    auto catalog = request(probe, Step::Catalog, 4700);
    probe.complete(catalog.id, 200, profilePage(std::move(tiles)).dump(), 4800);
    require(probe.status().step == Step::CatalogReady, "Catalog ready for detail tests");
}
Json launchAction(const char* target = "DIRECT_LAUNCH")
{
    const auto payload = std::string(target) == "DIRECT_LAUNCH" ? Json{{"serviceToken", "FAKE_WIDGET_TOKEN"}} :
        Json{{"gameId", "FAKE_PROVISIONING_ID"}, {"playToken", "FAKE_PLAY_TOKEN"}, {"productStage", "LIVE"}};
    return {{"target", target}, {"payload", payload.dump()}, {"serviceToken", "FAKE_WRONG_TOP_LEVEL_TOKEN"}};
}
Json detailPage(Json actions = Json::array({launchAction()}))
{
    const Json product = {{"type", "PRODUCT_DETAIL"}, {"presentationData", Json{{"title", "Fake Detail Title"},
        {"subHeader", "A fake game description."}, {"backgroundImageLandscape", "https://example.invalid/fake.jpg"},
        {"videoUrl", "https://example.invalid/fake.mp4"}}.dump()}};
    const Json button = {{"type", "PRODUCT_DETAIL_PRIMARY_BUTTON"},
        {"presentationData", Json{{"primaryText", "Play\nnow"}}.dump()}, {"actions", std::move(actions)}};
    return profilePage(Json::array({product, button}));
}
Request openDetail(NativeAuthProbe& probe)
{
    require(probe.openGame(0, 4900), "Explicit A selection opens the selected detail link");
    return request(probe, Step::DetailPage, 4900);
}
Json launchWidget(Json action = launchAction("GAME_SESSION_ASYNC"))
{
    return {{"widget", {{"actions", Json::array({std::move(action)})}}}};
}
Json experimentalAction()
{
    return {{"target", "EXPERIMENTAL_CLIENT"}, {"serviceToken", "FAKE_NOTICE_TOKEN"},
        {"payload", Json{{"serviceToken", "FAKE_WRONG_NESTED_TOKEN"}}.dump()}};
}
Json noticePage(Json continuation = launchAction("GAME_SESSION_ASYNC"))
{
    return profilePage(Json::array({
        Json{{"type", "DIALOG_INFO"}, {"presentationData", Json{{"primaryText", "Experimental client"},
            {"secondaryText", "This is a fake warning. Read before continuing."}}.dump()}},
        Json{{"type", "DIALOG_PRIMARY_BUTTON"}, {"presentationData", Json{{"primaryText", "Continue"}}.dump()},
            {"actions", Json::array({std::move(continuation)})}},
        Json{{"type", "DIALOG_BUTTON"}, {"presentationData", Json{{"text", "Cancel"}}.dump()},
            {"actions", Json::array({Json{{"target", "DIALOG_DISMISS"}}})}}
    }));
}
Request openNotice(NativeAuthProbe& probe)
{
    catalogReady(probe);
    auto detail = openDetail(probe);
    probe.complete(detail.id, 200, detailPage(Json::array({experimentalAction()})).dump(), 5000);
    require(probe.status().detailParsing.action == LaunchAction::ExperimentalClient && !probe.nextRequest(5001) &&
        !probe.status().detailParsing.hasPlayToken, "Experimental client action waits for X without prefetch or autoplay");
    require(probe.inspectLaunch(5100), "X explicitly reads the experimental notice");
    auto page = request(probe, Step::ExperimentalPage, 5100);
    require(page.url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getPage" &&
        routeBody(page) == Json{{"featureScheme", "WEB_V1"}, {"serviceToken", "FAKE_NOTICE_TOKEN"}} &&
        hasHeader(page, "x-amz-profile-token: FAKE_PROFILE_TOKEN"),
        "Experimental modal uses top-level serviceToken with profile-bound getPage, not the direct-launch payload token");
    return page;
}
}
int main()
{
    NativeAuthProbe firstName, secondName;
    firstName.start(Serial, 0);
    // Same first 31 hex digits: a shortened serial suffix would still collide.
    secondName.start("123456781234423481231234567890ac", 0);
    auto firstRequest = request(firstName, Step::CodePair, 0);
    auto secondRequest = request(secondName, Step::CodePair, 0);
    const auto firstRegistration = Json::parse(firstRequest.body)["code_data"];
    const auto secondRegistration = Json::parse(secondRequest.body)["code_data"];
    require(firstRegistration["device_name"] != secondRegistration["device_name"],
        "New serials yield different names even when only their last hex digit differs");
    require(firstRegistration["device_name"].get<std::string>().size() <= 64 &&
        secondRegistration["device_name"] == "LunaNX Switch " + secondRegistration["device_serial"].get<std::string>(),
        "Unique registration names are bounded and use the exact registration serial");
    NativeAuthProbe success;
    link(success); refresh(success);
    auto page = request(success, Step::LunaPage, 4100);
    require(page.url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getPage", "Read-only fixed Luna endpoint");
    require(Json::parse(page.body)["pageContext"]["pageUri"] == "home", "Read a page, not a game launch");
    bool accessHeader = false;
    for (const auto& header : page.headers) if (header == "x-amz-access-token: FAKE_NEW_ACCESS") accessHeader = true;
    require(accessHeader && page.body.find("FAKE_") == std::string::npos, "Fresh token stays in intended header");
    success.complete(page.id, 200, Page.dump(), 4200);
    require(success.status().step == Step::NegativeControl && success.status().lunaAccepted, "A successful page alone does not establish authorization");
    auto control = request(success, Step::NegativeControl, 4200);
    require(control.body == page.body, "Valid and deliberately invalid-token checks use the same browser/context request body");
    for (const auto& header : control.headers) require(header.find("FAKE_NEW_ACCESS") == std::string::npos, "Control contains no real access token");
    success.complete(control.id, 403, "untrusted response text", 4300);
    require(success.status().step == Step::Complete && success.status().publicCode.empty(), "Valid accepted / invalid rejected proves token acceptance");
    require(!success.nextRequest(4400), "Completed login does not replay");

    NativeAuthProbe unproven;
    link(unproven); refresh(unproven);
    page = request(unproven, Step::LunaPage, 4100);
    unproven.complete(page.id, 200, Page.dump(), 4200);
    control = request(unproven, Step::NegativeControl, 4200);
    unproven.complete(control.id, 200, Page.dump(), 4300);
    require(unproven.status().error == Error::AuthorizationNotProven, "Guest endpoint cannot produce a false success");
    require(unproven.status().failedAt == Step::NegativeControl, "Failure reports its operation without response text");

    NativeAuthProbe largePage; link(largePage); refresh(largePage);
    page = request(largePage, Step::LunaPage, 4100);
    auto richPage = Page; richPage["padding"] = std::string(400000, 'x');
    largePage.complete(page.id, 200, richPage.dump(), 4200);
    require(largePage.status().step == Step::NegativeControl, "A rich Luna page can exceed the small auth-response limit");
    NativeAuthProbe hugePage; link(hugePage); refresh(hugePage);
    page = request(hugePage, Step::LunaPage, 4100);
    richPage["padding"] = std::string(MaxLunaResponse, 'x');
    hugePage.complete(page.id, 200, richPage.dump(), 4200);
    require(hugePage.status().error == Error::Response, "Luna page memory remains bounded");

    for (const auto& body : {Json::object(), Json{{"error", "FAKE_SECRET"}}, Json{{"errorCode", 42}},
        Json{{"pageMemberGroups", "wrong type"}}, Json{{"pageMemberGroups", Json::object()}}})
    {
        NativeAuthProbe invalid;
        link(invalid); refresh(invalid);
        page = request(invalid, Step::LunaPage, 4100);
        invalid.complete(page.id, 200, body.dump(), 4200);
        require(invalid.status().error == Error::Response && !invalid.status().lunaAccepted, "Reject fake success / application errors");
    }
    for (const auto& field : {"expires_in", "polling_interval_in_seconds"})
        for (const auto& bad : {Json(-1), Json(0), Json(1.5), Json(true), Json("2oops"), Json(999999)})
        {
            NativeAuthProbe invalid; invalid.start(Serial, 0);
            auto pair = request(invalid, Step::CodePair, 0);
            auto body = Code; body[field] = bad;
            invalid.complete(pair.id, 200, body.dump(), 0);
            require(invalid.status().error == Error::Response && invalid.status().publicCode.empty(), "Strict expiry and polling types/bounds");
        }
    for (const auto& body : {std::string("[]"), std::string("not JSON"), std::string(262145, 'x'),
        std::string("{\"public_code\":\"ABCD\",\"public_code\":\"EFGH\"}"),
        Json{{"public_code", "FAKE_SECRET\n"}, {"private_code", "secret"}}.dump()})
    {
        NativeAuthProbe invalid; invalid.start(Serial, 0);
        auto pair = request(invalid, Step::CodePair, 0);
        invalid.complete(pair.id, 200, body, 0);
        require(invalid.status().error == Error::Response && invalid.status().publicCode.empty(), "Malformed response is redacted");
    }
    NativeAuthProbe expired; expired.start(Serial, 0);
    auto pair = request(expired, Step::CodePair, 0);
    expired.complete(pair.id, 200, Code.dump(), 0);
    require(!expired.nextRequest(300000) && expired.status().error == Error::Expired, "Code deadline enforced before another poll");
    NativeAuthProbe cancelled; cancelled.start(Serial, 0);
    pair = request(cancelled, Step::CodePair, 0);
    cancelled.cancel(); cancelled.complete(pair.id, 200, Code.dump(), 0);
    require(cancelled.status().step == Step::Cancelled && !cancelled.nextRequest(5000), "Cancellation ignores in-flight completion");
    NativeAuthProbe failed; failed.start(Serial, 0);
    pair = request(failed, Step::CodePair, 0); failed.transportFailed(pair.id);
    require(failed.status().error == Error::Transport && !failed.nextRequest(1), "No automatic network replay");

    NativeAuthProbe catalog(Mode::ProfilesAndCatalog); profiles(catalog);
    require(catalog.status().step == Step::ChooseProfile && catalog.profiles().size() == 2, "Profiles load before any profile is selected");
    require(catalog.status().profileParsing.stage == ProfileParse::Complete && catalog.status().profileParsing.encodedActions == 2 &&
        catalog.status().profileParsing.parsed == 2, "Decode actual PDS JSON-text action payloads independently from presentation data");
    require(catalog.profiles()[0].name == "Player One" && !catalog.nextRequest(4500), "Explicit choice required; no silent default profile");
    require(!catalog.chooseProfile(99, 4500), "Out-of-range choice does not send requests");
    require(catalog.chooseProfile(1, 5000), "User can choose the second profile");
    auto login = request(catalog, Step::PlayerLogin, 5000);
    require(login.url.find("/player/playerLogin") != std::string::npos &&
        Json::parse(login.body) == Json{{"profileId", "FAKE_PROFILE_2"}}, "Only selected ID reaches exact profile-login endpoint");
    require(hasHeader(login, "x-amz-access-token: FAKE_NEW_ACCESS") &&
        !hasHeader(login, "x-amz-profile-token: FAKE_PROFILE_TOKEN"), "Login uses account token without stale profile token");
    require(!catalog.nextRequest(5001) && !catalog.chooseProfile(0, 5001), "No overlapping profile login or mid-request reselection");
    catalog.complete(login.id, 200, Json{{"token", "FAKE_PROFILE_TOKEN"}, {"errorCode", nullptr}}.dump(), 5100);
    auto games = request(catalog, Step::Catalog, 5100);
    require(hasHeader(games, "x-amz-profile-token: FAKE_PROFILE_TOKEN") &&
        hasHeader(games, "x-amz-access-token: FAKE_NEW_ACCESS"), "Catalog is bound to chosen profile and account");
    require(Json::parse(games.body)["pageContext"]["pageUri"] == "home", "Read catalog from verified home route");
    const Json gameTile = {{"type", "GAME_TILE"}, {"presentationData", Json{{"title", "Fake Game"}, {"gameId", "FAKE_GAME"}}.dump()}};
    auto horizontal = gameTile; horizontal["type"] = "GAME_TILE_HORIZONTAL";
    auto vertical = gameTile; vertical["type"] = "GAME_TILE_VERTICAL";
    vertical["presentationData"] = Json{{"title", "Fake Game Two"}, {"gameId", "FAKE_GAME_TWO"}}.dump();
    // This page contains only the actual directional aliases; the old parser
    // returned zero games even though these are normal game tiles in Luna.
    auto gamePage = profilePage(Json::array({horizontal, vertical, horizontal,
        Json{{"type", "PROMOTION"}, {"presentationData", "not a game"}}}));
    catalog.complete(games.id, 200, gamePage.dump(), 5200);
    require(catalog.status().step == Step::CatalogReady && catalog.games().size() == 2 &&
        catalog.games()[0].title == "Fake Game" && catalog.games()[1].title == "Fake Game Two",
        "Directional game-tile aliases parse and deduplicate; unrelated promotions ignored");
    require(catalog.status().catalogParsing.stage == CatalogParse::Complete && catalog.status().catalogParsing.widgets == 5 &&
        catalog.status().catalogParsing.tiles == 3 && catalog.status().catalogParsing.horizontal == 2 &&
        catalog.status().catalogParsing.vertical == 1 && catalog.status().catalogParsing.parsed == 2,
        "Catalog counters distinguish raw tiles, directional aliases and unique parsed games");
    require(!catalog.nextRequest(5300), "Browsing does not launch a game or auto-poll");
    catalog.returnToProfiles();
    require(catalog.games().empty() && catalog.status().step == Step::ChooseProfile, "Switching profiles clears old catalog");
    require(catalog.status().catalogParsing.stage == CatalogParse::None, "Profile switching clears old catalog diagnostics");
    require(catalog.chooseProfile(0, 5400), "Second selection supported in same RAM-only session");
    login = request(catalog, Step::PlayerLogin, 5400);
    for (const auto& h : login.headers) require(h.find("FAKE_PROFILE_TOKEN") == std::string::npos, "Prior profile token never leaks into next profile login");
    catalog.cancel(); catalog.complete(login.id, 200, Json{{"token", "FAKE_LATE_TOKEN"}}.dump(), 5500);
    require(catalog.profiles().empty() && catalog.games().empty() && catalog.status().step == Step::Cancelled, "Cancel erases account display data and ignores late login");

    const std::string fakeTdi = "00000000-0000-4000-8000-000000000001";
    const auto identityRequest = [&](NativeAuthProbe& auth)
    {
        profiles(auth);
        auth.chooseProfile(0, 4600); auto login = request(auth, Step::PlayerLogin, 4600);
        auth.complete(login.id, 200, Json{{"token", "FAKE_PROFILE_TOKEN"}}.dump(), 4700);
        require(auth.status().step == Step::ClientIdentity && !auth.status().clientIdentityReady,
            "Opt-in identity lookup occurs after profile login but before catalog/play-token reads");
        auto identity = request(auth, Step::ClientIdentity, 4700);
        require(identity.url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getIotConnectionInfo" &&
            Json::parse(identity.body) == Json{{"deviceRole", "STREAMING_CLIENT"},
                {"iotFeatureScheme", "TEMPO_CLIENT_PARENT_V4_MIGRATION"}} &&
            hasHeader(identity, "x-amz-profile-token: FAKE_PROFILE_TOKEN"),
            "Identity lookup follows source-derived profile-scoped streaming-client request");
        require(!auth.nextRequest(4701), "Identity lookup cannot overlap authenticated requests");
        return identity;
    };
    {
        NativeAuthProbe identityAuth(Mode::ProfilesAndCatalog, true);
        auto identity = identityRequest(identityAuth);
        identityAuth.complete(identity.id, 200, Json{{"tdi", fakeTdi}, {"url", "https://example.invalid/FAKE_UNUSED"},
            {"credentials", {{"secret", "FAKE_MQTT_SECRET"}}}}.dump(), 4750);
        require(identityAuth.status().clientIdentityReady && identityAuth.status().step == Step::Catalog,
            "Validated server-issued identity is ready before catalog requests");
        auto home = request(identityAuth, Step::Catalog, 4750);
        require(hasHeader(home, "x-amz-tdi: " + fakeTdi) && home.body.find(fakeTdi) == std::string::npos &&
            home.body.find("FAKE_MQTT_SECRET") == std::string::npos,
            "Only the identity header is forwarded, never IoT credentials, URLs or topics");
        identityAuth.complete(home.id, 200, profilePage(Json::array({navigableGame()})).dump(), 4800);
        auto details = openDetail(identityAuth);
        require(hasHeader(details, "x-amz-tdi: " + fakeTdi), "Identity binds the page that mints the fresh play token");
        identityAuth.complete(details.id, 200, detailPage(Json::array({launchAction("GAME_SESSION_ASYNC")})).dump(), 5000);
        NativeSession attempt(identityAuth); require(attempt.start(5100, true), "Identity lookup cannot start a game by itself");
        auto pin = attempt.nextRequest(5100);
        require(pin && hasHeader(*pin, "x-amz-tdi: " + fakeTdi), "Session adapter uses the same profile-bound client identity");
        attempt.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5120);
        const std::string identityOffer = "v=0\r\nm=video 1 UDP/TLS/RTP/SAVPF 108\r\n"
            "m=audio 1 UDP/TLS/RTP/SAVPF 111\r\nm=application 1 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "a=fingerprint:sha-256 FAKE\r\n";
        require(attempt.supplyOffer(identityOffer, 5130), "Identity-ready launch still requires a fresh offer");
        auto create = attempt.nextRequest(5130);
        require(create && hasHeader(*create, "x-amz-tdi: " + fakeTdi) &&
            Json::parse(create->body)["clientFeatures"]["dataUsage"] == "LIMITED" &&
            create->body.find("FAKE_MQTT_SECRET") == std::string::npos,
            "Provisioning forwards this client's identity without changing default quality or copying IoT secrets");
        attempt.cancel(5150);
        identityAuth.returnToCatalog(); identityAuth.returnToProfiles();
        require(!identityAuth.status().clientIdentityReady && identityAuth.chooseProfile(1, 5200),
            "Switching profiles discards the previous client identity");
        auto login = request(identityAuth, Step::PlayerLogin, 5200);
        require(!hasHeader(login, "x-amz-tdi: " + fakeTdi), "Old identity cannot leak into another profile's login");
    }
    for (const auto& invalidIdentity : {Json::object(), Json{{"tdi", "FAKE_INVALID_ID"}},
        Json{{"tdi", fakeTdi + "extra"}}, Json{{"tdi", "00000000-0000-4000-8000-00000000000\n"}},
        Json{{"tdi", fakeTdi}, {"errorCode", 1}}})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog, true); auto identity = identityRequest(auth);
        auth.complete(identity.id, 200, invalidIdentity.dump(), 4750);
        require(auth.status().step == Step::Failed && !auth.status().clientIdentityReady && !auth.nextRequest(4800),
            "Malformed or rejected identity cannot become a header or continue a diagnostic launch");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog, true); auto identity = identityRequest(auth);
        auth.cancel(); auth.complete(identity.id, 200, Json{{"tdi", fakeTdi}}.dump(), 4750);
        require(auth.status().step == Step::Cancelled && !auth.status().clientIdentityReady,
            "Cancelled identity lookup ignores its late response");
    }
    for (const auto& blocked : {Json{{"isLocked", true}}, Json{{"sourcePinExists", true}},
        Json{{"migrationStatus", "IN_PROGRESS"}}, Json{{"isHouseholdMismatch", true}}})
    {
        NativeAuthProbe guard(Mode::ProfilesAndCatalog);
        profiles(guard, Json::array({tile("FAKE_LOCKED", "Locked", blocked)}));
        require(!guard.chooseProfile(0, 4600) && !guard.nextRequest(4600), "PIN/migration/family guard prevents profile-login request");
        require(guard.status().profileBlock != ProfileBlock::None, "Local block is explainable without response text");
    }
    NativeAuthProbe import(Mode::ProfilesAndCatalog);
    profiles(import, Json::array({tile("FAKE_IMPORT", "Import", Json::object(), "PROFILE_IMPORT")}));
    require(!import.chooseProfile(0, 4600) && import.status().profileBlock == ProfileBlock::ImportRequired, "Import action is not silently replaced with login");
    for (const auto code : {26, 27, 28, 29})
    {
        NativeAuthProbe blocked(Mode::ProfilesAndCatalog); profiles(blocked);
        blocked.chooseProfile(0, 4600); login = request(blocked, Step::PlayerLogin, 4600);
        blocked.complete(login.id, 200, Json{{"errorCode", code}, {"token", "FAKE_MUST_NOT_USE"}}.dump(), 4700);
        require(blocked.status().step == Step::ChooseProfile && blocked.status().profileBlock != ProfileBlock::None &&
            !blocked.nextRequest(4800), "Server profile restriction takes precedence over token; no catalog request");
        require(!blocked.chooseProfile(0, 4800) && blocked.chooseProfile(1, 4800), "Rejected profile stays blocked, other profile remains selectable");
    }
    for (auto broken : {tile("FAKE_DUP", "Bad\x1b[2J"), tile("FAKE_MISMATCH", "Mismatch"), tile("FAKE_BAD_DATA", "Bad")})
    {
        const auto id = Json::parse(broken["presentationData"].get<std::string>())["profileId"];
        if (id == "FAKE_MISMATCH") broken["actions"][0]["payload"] = Json{{"profileId", "FAKE_OTHER"}}.dump();
        if (id == "FAKE_BAD_DATA") broken["presentationData"] = "{\"profileId\":\"x\",\"profileId\":\"y\"}";
        NativeAuthProbe invalid(Mode::ProfilesAndCatalog); profiles(invalid, Json::array({broken}));
        require(invalid.status().error == Error::Response && invalid.profiles().empty(), "Reject control sequences, identity mismatches and duplicate JSON fields");
    }
    NativeAuthProbe duplicate(Mode::ProfilesAndCatalog);
    profiles(duplicate, Json::array({tile("FAKE_DUP", "One"), tile("FAKE_DUP", "Two")}));
    require(duplicate.status().error == Error::Response && duplicate.profiles().empty(), "Ambiguous duplicate profile IDs cannot be selected");
    NativeAuthProbe sessionExpired(Mode::ProfilesAndCatalog); profiles(sessionExpired);
    require(!sessionExpired.nextRequest(910000) && sessionExpired.status().error == Error::Expired &&
        sessionExpired.profiles().empty(), "Profile selection deadline expires and clears RAM data");
    NativeAuthProbe nullable(Mode::ProfilesAndCatalog);
    profiles(nullable, Json::array({tile("FAKE_OPTIONAL", "Optional", {{"isLocked", nullptr},
        {"sourcePinExists", nullptr}, {"isHouseholdMismatch", nullptr}, {"migrationStatus", 0}})}));
    require(nullable.chooseProfile(0, 4600), "Nullable optional flags and zero migration state do not falsely lock a profile");
    auto actionIdOnly = tile("FAKE_ACTION_ID", "Action identity");
    auto metadata = Json::parse(actionIdOnly["presentationData"].get<std::string>());
    metadata.erase("profileId");
    actionIdOnly["presentationData"] = metadata.dump();
    NativeAuthProbe fromAction(Mode::ProfilesAndCatalog); profiles(fromAction, Json::array({actionIdOnly}));
    require(fromAction.chooseProfile(0, 4600) && fromAction.profiles()[0].id == "FAKE_ACTION_ID",
        "Selection action supplies identity even when optional presentation ID is absent");
    auto decoded = tile("FAKE_DECODED", "Decoded");
    decoded["actions"][0]["payload"] = Json::parse(decoded["actions"][0]["payload"].get<std::string>());
    NativeAuthProbe decodedPayload(Mode::ProfilesAndCatalog); profiles(decodedPayload, Json::array({decoded}));
    require(decodedPayload.chooseProfile(0, 4600) && decodedPayload.status().profileParsing.encodedActions == 0,
        "Already decoded action objects remain supported");
    for (const auto& bad : {Json("not JSON FAKE_SECRET"), Json("[]"), Json(42),
        Json("{\"profileId\":\"FAKE_FIRST\",\"profileId\":\"FAKE_SECOND\"}")})
    {
        auto broken = tile("FAKE_BAD_ACTION", "Bad action");
        broken["actions"][0]["payload"] = bad;
        NativeAuthProbe invalid(Mode::ProfilesAndCatalog); profiles(invalid, Json::array({broken}));
        require(invalid.status().error == Error::Response && invalid.status().profileParsing.stage == ProfileParse::Actions &&
            invalid.profiles().empty() && !invalid.nextRequest(4600),
            "Malformed, non-object or ambiguous action JSON fails at the named stage without issuing login");
    }
    auto conflicting = tile("FAKE_FIRST", "Conflicting");
    conflicting["actions"].push_back(Json{{"target", "PLAYER_SELECT"}, {"payload", Json{{"profileId", "FAKE_SECOND"}}.dump()}});
    NativeAuthProbe conflict(Mode::ProfilesAndCatalog); profiles(conflict, Json::array({conflicting}));
    require(conflict.status().error == Error::Response && conflict.status().profileParsing.stage == ProfileParse::Identity,
        "Conflicting decoded selection identities cannot be silently selected");
    auto addNew = Json{{"type", "PLAYER_SELECTION_TILE"}, {"presentationData", Json{{"primaryText", "Add new"}}.dump()},
        {"actions", Json::array({Json{{"target", "PLAYER_CREATE"}, {"payload", "{}"}}})}};
    NativeAuthProbe skipCreation(Mode::ProfilesAndCatalog);
    profiles(skipCreation, Json::array({addNew, tile("FAKE_EXISTING", "Existing")}));
    require(skipCreation.profiles().size() == 1 && skipCreation.status().profileParsing.tiles == 2 &&
        skipCreation.chooseProfile(0, 4600), "Add-new profile tile does not hide an existing selectable profile");
    NativeAuthProbe noProfiles(Mode::ProfilesAndCatalog); profiles(noProfiles, Json::array({addNew}));
    require(noProfiles.status().error == Error::Response && noProfiles.status().profileParsing.stage == ProfileParse::NoProfiles &&
        noProfiles.status().profileParsing.tiles == 1, "No selectable identities is diagnosed separately from malformed action payloads");
    for (const auto& badLogin : {Json::object(), Json{{"token", "FAKE_TOKEN\r\nInjected: header"}},
        Json{{"errorCode", 99}, {"token", "FAKE_REJECTED"}}})
    {
        NativeAuthProbe invalid(Mode::ProfilesAndCatalog); profiles(invalid);
        invalid.chooseProfile(0, 4600); login = request(invalid, Step::PlayerLogin, 4600);
        invalid.complete(login.id, 200, badLogin.dump(), 4700);
        require(invalid.status().error == Error::Response && invalid.games().empty() && !invalid.nextRequest(4800),
            "Missing/unsafe token or application error never reaches catalog");
    }
    NativeAuthProbe rejectedCatalog(Mode::ProfilesAndCatalog); profiles(rejectedCatalog);
    rejectedCatalog.chooseProfile(0, 4600); login = request(rejectedCatalog, Step::PlayerLogin, 4600);
    rejectedCatalog.complete(login.id, 200, Json{{"token", "FAKE_PROFILE_TOKEN"}}.dump(), 4700);
    games = request(rejectedCatalog, Step::Catalog, 4700);
    rejectedCatalog.complete(games.id, 403, "FAKE_SERVER_ERROR_SECRET", 4800);
    require(rejectedCatalog.status().failedAt == Step::Catalog && rejectedCatalog.status().error == Error::Http &&
        rejectedCatalog.profiles().empty() && rejectedCatalog.games().empty(), "Rejected catalog clears account data without retries or raw error text");
    for (const auto* type : {"GAME_TILE", "GAMEPLAY_TILE", "GAME_TILE_HORIZONTAL", "GAME_TILE_VERTICAL", "UNKNOWN_FUTURE_TILE"})
    {
        NativeAuthProbe tileType(Mode::ProfilesAndCatalog); profiles(tileType);
        tileType.chooseProfile(0, 4600); login = request(tileType, Step::PlayerLogin, 4600);
        tileType.complete(login.id, 200, Json{{"token", "FAKE_PROFILE_TOKEN"}}.dump(), 4700);
        auto catalogRequest = request(tileType, Step::Catalog, 4700);
        auto tileData = gameTile; tileData["type"] = type;
        tileType.complete(catalogRequest.id, 200, profilePage(Json::array({tileData})).dump(), 4800);
        const bool supported = std::string(type) != "UNKNOWN_FUTURE_TILE";
        require(tileType.status().step == Step::CatalogReady && tileType.games().size() == (supported ? 1u : 0u) &&
            tileType.status().catalogParsing.stage == CatalogParse::Complete,
            "Accept the four source-verified game aliases; an unrelated type is not silently reclassified as a game");
        require(tileType.status().catalogParsing.widgets == 2 && tileType.status().catalogParsing.tiles == (supported ? 1u : 0u),
            "An unsupported tile is visible as a widget count without claiming an empty entitled library");
    }
    NativeAuthProbe badTitle(Mode::ProfilesAndCatalog); profiles(badTitle);
    badTitle.chooseProfile(0, 4600); login = request(badTitle, Step::PlayerLogin, 4600);
    badTitle.complete(login.id, 200, Json{{"token", "FAKE_PROFILE_TOKEN"}}.dump(), 4700);
    games = request(badTitle, Step::Catalog, 4700);
    auto unsafeTile = gameTile;
    unsafeTile["presentationData"] = Json{{"gameId", "FAKE_GAME"}, {"title", "FAKE_SECRET\x1b[2J"}}.dump();
    badTitle.complete(games.id, 200, profilePage(Json::array({unsafeTile})).dump(), 4800);
    require(badTitle.status().error == Error::Response && badTitle.status().catalogParsing.stage == CatalogParse::Title &&
        badTitle.games().empty(), "Catalog failures name the parsing stage without printing title text or keeping partial games");
    for (const auto& badConfig : {Json::object(), Json{{"profilePageContext", {{"pageType", "FAKE_TYPE"}}}},
        Json{{"errorCode", 99}, {"profilePageContext", {{"pageType", "FAKE_TYPE"}, {"pageId", "FAKE_ID"}}}}})
    {
        NativeAuthProbe invalid(Mode::ProfilesAndCatalog); config(invalid);
        auto app = request(invalid, Step::AppConfig, 4300);
        invalid.complete(app.id, 200, badConfig.dump(), 4400);
        require(invalid.status().error == Error::Response && !invalid.nextRequest(4500), "Missing or rejected profile context cannot produce a guessed request");
    }
    require(responseLimit(Step::ProfilesPage) == MaxLunaResponse && responseLimit(Step::Catalog) == MaxLunaResponse &&
        responseLimit(Step::PlayerLogin) == MaxAuthResponse, "Transport and parser agree on per-operation response caps");
    NativeAuthProbe shortToken(Mode::ProfilesAndCatalog); link(shortToken);
    auto token = request(shortToken, Step::Refresh, 4000);
    shortToken.complete(token.id, 200, Json{{"access_token", "FAKE_SHORT_TOKEN"}, {"expires_in", 1}}.dump(), 4100);
    require(!shortToken.nextRequest(5100) && shortToken.status().error == Error::Expired,
        "Short token lifetime bounds initial Luna checks as well as later selection");
    for (const auto& entry : std::vector<std::pair<Json, AuthError>>{
        {Json{{"error", "InvalidValue"}, {"error_description", "The request has an invalid parameter : private_code"}}, AuthError::InvalidValue},
        {Json{{"error", {{"code", "InvalidCodePair"}, {"message", "FAKE_SECRET_DESCRIPTION"}}}}, AuthError::InvalidCodePair},
        {Json{{"response", {{"error", {{"code", "InvalidData"}, {"message", "FAKE_SECRET_DESCRIPTION"}}}}}}, AuthError::InvalidData},
        {Json{{"response", {{"error", {{"code", "ExpiredCodePair"}}}}}}, AuthError::ExpiredCodePair},
        // Observed using entirely fake codes against /auth/register; this is
        // not evidence of the error returned for Josh's approved code.
        {Json{{"response", {{"error", {{"code", "InvalidToken"}, {"message", "FAKE_REDACTED"}}}}}}, AuthError::InvalidToken},
        {Json{{"response", {{"error", {{"code", "DuplicateDeviceName"}, {"message", "FAKE_REDACTED"}}}}}}, AuthError::DuplicateDeviceName},
        {Json{{"response", {{"error", {{"code", "DuplicateDeviceNameError"}, {"message", "FAKE_REDACTED"}}}}}}, AuthError::DuplicateDeviceName},
        {Json{{"error", "FAKE_SECRET_CODE"}, {"error_description", "FAKE_SECRET_DESCRIPTION"}}, AuthError::Unknown},
        {Json{{"error", {{"code", Json::array({"InvalidValue"})}}}}, AuthError::Unknown},
        {Json{{"response", {{"error", "MissingValue"},
            {"error_description", "The request is missing a required parameter : FAKE_SECRET_PARAMETER"}}}}, AuthError::MissingValue}})
    {
        NativeAuthProbe rejected(Mode::ProfilesAndCatalog); rejected.start(Serial, 0);
        auto pairRequest = request(rejected, Step::CodePair, 0);
        rejected.complete(pairRequest.id, 200, Code.dump(), 0);
        auto pending = request(rejected, Step::Register, 2000);
        rejected.complete(pending.id, 401, "pending", 2000);
        require(rejected.status().authError == AuthError::None, "Normal pending approval is not labeled as an auth rejection");
        auto refused = request(rejected, Step::Register, 4000);
        rejected.complete(refused.id, 400, entry.first.dump(), 4000);
        require(rejected.status().step == Step::Failed && rejected.status().failedAt == Step::Register &&
            rejected.status().httpStatus == 400 && rejected.status().error == Error::Http,
            "Reported 401-to-400 sequence remains a registration failure");
        require(rejected.status().authError == entry.second, "Auth error shape maps to a fixed allowlisted category");
        require(rejected.status().authResponse == (entry.first.contains("response") ? AuthResponse::NestedError : AuthResponse::TopLevelError) &&
            rejected.status().authResponseBytes == entry.first.dump().size(), "Report only fixed response shape and byte count");
        require(rejected.status().approvalPolls == 2 && rejected.status().codeAgeSeconds == 4 && rejected.status().codeLifetimeSeconds == 300,
            "Registration HTTP failure preserves elapsed timing without code values");
        require(rejected.status().publicCode.empty() && !rejected.nextRequest(6000), "Rejected registration erases code and never automatically replays");
        const std::string diagnostics = std::string(authErrorLabel(rejected.status().authError)) +
            authParameterLabel(rejected.status().authParameter) + authResponseLabel(rejected.status().authResponse);
        require(diagnostics.find("FAKE_") == std::string::npos, "Server descriptions, unknown codes and parameters stay out of diagnostics");
        if (entry.second == AuthError::InvalidValue)
            require(rejected.status().authParameter == AuthParameter::PrivateCode, "Recognize only exact safe parameter name");
        if (entry.second == AuthError::MissingValue)
            require(rejected.status().authParameter == AuthParameter::Unknown, "Unknown parameter remains redacted");
    }
    for (const auto& body : {std::string(), std::string("not JSON FAKE_SECRET"), std::string(MaxAuthResponse + 1, 'x'),
        std::string("{\"error\":\"InvalidValue\",\"error\":\"MissingValue\"}")})
    {
        NativeAuthProbe rejected; rejected.start(Serial, 0);
        auto pairRequest = request(rejected, Step::CodePair, 0);
        rejected.complete(pairRequest.id, 400, body, 0);
        require(rejected.status().authError == AuthError::Unknown && rejected.status().error == Error::Http,
            "Malformed or excessive error bodies cannot hide HTTP failure or enter logs");
        require(rejected.status().authResponse == (body.empty() ? AuthResponse::Empty :
            body.size() > MaxAuthResponse ? AuthResponse::TooLarge : AuthResponse::Unparseable),
            "Distinguish empty, excessive and unparseable auth failures");
    }
    NativeAuthProbe rootFallback; rootFallback.start(Serial, 0);
    auto fallbackRequest = request(rootFallback, Step::CodePair, 0);
    rootFallback.complete(fallbackRequest.id, 400, Json{{"response", Json::object()}, {"error", "InvalidToken"}}.dump(), 0);
    require(rootFallback.status().authError == AuthError::InvalidToken && rootFallback.status().authResponse == AuthResponse::TopLevelError,
        "Unrelated response object does not mask a top-level error");
    NativeAuthProbe noErrorField; noErrorField.start(Serial, 0);
    fallbackRequest = request(noErrorField, Step::CodePair, 0);
    noErrorField.complete(fallbackRequest.id, 400, Json{{"FAKE_UNKNOWN_FIELD", "FAKE_SECRET"}}.dump(), 0);
    require(noErrorField.status().authError == AuthError::Unknown && noErrorField.status().authResponse == AuthResponse::NoErrorField,
        "JSON with unknown keys reports a fixed shape without emitting key names or values");
    for (const auto& entry : std::vector<std::pair<Json, std::string>>{
        {Json("FutureRegistrationError"), "FutureRegistrationError"},
        {Json("namespace.Future_Error:2"), "namespace.Future_Error:2"},
        {Json(4201), "error_number_4201"},
        {Json("ABCD-1234"), ""}, {Json("FAKE_PRIVATE"), ""}, {Json(Serial), ""},
        {Json("FutureError_FAKE_PRIVATE"), ""}, {Json("BadError\nFAKE_SECRET"), ""},
        {Json("Atza|FAKE_TOKEN"), ""}, {Json("amzn1.account.FAKE_ACCOUNT"), ""},
        {Json(std::string(81, 'A')), ""}})
    {
        NativeAuthProbe localOnly; localOnly.start(Serial, 0);
        auto codeRequest = request(localOnly, Step::CodePair, 0);
        localOnly.complete(codeRequest.id, 200, Code.dump(), 0);
        auto poll = request(localOnly, Step::Register, 2000);
        localOnly.complete(poll.id, 400, Json{{"response", {{"error", {{"code", entry.first},
            {"message", "FAKE_SECRET_SERVER_DESCRIPTION"}}}}}}.dump(), 2000);
        require(localOnly.localAuthErrorIdentifier() == entry.second,
            "Show only bounded error identifiers locally; exclude our codes, tokens, IDs and console controls");
        require(localOnly.status().authError == AuthError::Unknown && localOnly.status().authParameter == AuthParameter::None,
            "Unmapped local identifier never enters log-safe status categories");
        require(!localOnly.nextRequest(4000) && localOnly.status().publicCode.empty(),
            "Local diagnostic does not retry failed registration or retain the approval code");
    }
    // Browse requests use the public browser's real searchContext contract.
    const auto searchTiles = [](unsigned first, unsigned count)
    {
        auto tiles = Json::array();
        for (unsigned i = first; i < first + count; ++i)
        {
            const auto id = "FAKE_SEARCH_" + std::to_string(i);
            tiles.push_back(navigableGame(id.c_str()));
        }
        return tiles;
    };
    NativeAuthProbe browse(Mode::ProfilesAndCatalog); catalogReady(browse);
    for (const auto& invalid : {std::string(201, 'x'), std::string("bad\nquery"), std::string("bad\0query", 9),
        std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80")})
        require(!browse.browseGames(invalid, 0, 4900) && !browse.browsing().active && browse.games().size() == 1,
            "Invalid/control/non-UTF8 search never changes Home or credentials");
    require(!browse.browseGames("", 50, 4900) && !browse.browseGames("", 1, 4900) &&
        !browse.browseGames("", 10000, 4900), "Paging requires a previous result and bounded aligned offset");
    const std::string query = u8"Pokémon & \"test\" / ?";
    require(browse.browseGames(query, 0, 4900), "Unicode/quoted search accepted");
    auto search = request(browse, Step::SearchPage, 4900);
    require(routeBody(search) == Json{{"featureScheme", "WEB_V1"},
        {"pageContext", {{"pageType", "multistate_search_results"}, {"pageId", "default"}}},
        {"searchContext", {{"query", query}, {"sort", "RELEVANCE"}, {"pagination", {{"startIndex", 0}, {"pageSize", 50}}}}}},
        "Search text stays JSON-escaped in the body with browser page type and pagination");
    require(hasHeader(search, "x-amz-profile-token: FAKE_PROFILE_TOKEN") && responseLimit(Step::SearchPage) == MaxLunaResponse,
        "Search keeps profile scoping and the bounded Luna response limit");
    auto searchPage = profilePage(searchTiles(0, 50));
    searchPage["pageMemberGroups"]["recommendations"] = {{"widgets", Json::array({navigableGame("FAKE_UNRELATED")})}};
    browse.complete(search.id, 200, searchPage.dump(), 5000);
    require(browse.status().step == Step::CatalogReady && browse.browsing().query == query &&
        browse.games().size() == 50 && browse.browsing().hasNext && browse.browsing().offset == 0,
        "Only search mainContent enters results; full page offers Next");
    browse.browseGames(query, 0, 5010); search = request(browse, Step::SearchPage, 5010);
    browse.complete(search.id, 503, "FAKE_PRIVATE_RESPONSE", 5020);
    require(browse.games().size() == 50 && browse.browsing().query == query && browse.browsing().hasNext &&
        browse.browsing().error == Error::Http, "Failed replacement retains the existing result page and its query");
    require(!browse.browseGames(query, 150, 5100) && !browse.browseGames("changed", 50, 5100),
        "Cannot skip pages or reuse an offset with a changed query");
    require(browse.browseGames(query, 50, 5100), "Next page allowed after a full page");
    search = request(browse, Step::SearchPage, 5100);
    require(routeBody(search)["searchContext"]["pagination"]["startIndex"] == 50, "Next uses the requested offset");
    browse.complete(search.id, 200, profilePage(searchTiles(50, 3)).dump(), 5200);
    require(browse.games().size() == 3 && browse.games()[0].id == "FAKE_SEARCH_50" &&
        browse.browsing().offset == 50 && !browse.browsing().hasNext && !browse.browseGames(query, 100, 5300),
        "Short page replaces prior results and stops Next");
    require(browse.openGame(0, 5300), "Search result opens through existing detail path");
    auto searchDetail = request(browse, Step::DetailPage, 5300);
    require(routeBody(searchDetail)["serviceToken"] == "FAKE_PAGE_TOKEN", "Search result keeps its private detail token");
    browse.complete(searchDetail.id, 200, detailPage().dump(), 5400);
    browse.returnToCatalog();
    require(browse.games()[0].id == "FAKE_SEARCH_50" && browse.browsing().offset == 50,
        "Back from details preserves search and its page");
    require(browse.browseGames(query, 0, 5500), "Previous page allowed");
    search = request(browse, Step::SearchPage, 5500);
    browse.returnToHome();
    browse.complete(search.id, 200, profilePage(searchTiles(0, 50)).dump(), 5600);
    require(!browse.browsing().active && browse.games().size() == 1 && browse.games()[0].id == "FAKE_TILE_ID",
        "Back during request restores Home and ignores late result");
    require(browse.openGame(0, 5700), "Restored Home still has the original private detail links");
    searchDetail = request(browse, Step::DetailPage, 5700); browse.returnToCatalog();
    require(hasHeader(searchDetail, "x-amz-profile-token: FAKE_PROFILE_TOKEN"), "Back does not lose profile credentials");
    for (const auto& response : {std::string("not JSON"), std::string("{}"), std::string(MaxLunaResponse + 1, 'x'),
        std::string("{\"pageMemberGroups\":{},\"pageMemberGroups\":{}}"),
        Json{{"errorCode", 1}, {"pageMemberGroups", searchPage["pageMemberGroups"]}}.dump()})
    {
        require(browse.browseGames("broken", 0, 5800), "Can retry after going Home");
        search = request(browse, Step::SearchPage, 5800);
        browse.complete(search.id, 200, response, 5900);
        require(browse.status().step == Step::CatalogReady && browse.browsing().error == Error::Response &&
            browse.profiles().size() == 2, "Bad search result preserves signed-in session");
        browse.returnToHome();
        require(browse.games().size() == 1, "Bad search result never overwrites Home");
    }
    for (int http : {400, 404, 429, 500})
    {
        browse.browseGames("test", 0, 6000); search = request(browse, Step::SearchPage, 6000);
        browse.complete(search.id, http, "FAKE_SERVER_SECRET", 6100);
        require(browse.status().step == Step::CatalogReady && browse.browsing().error == Error::Http,
            "Ordinary browse HTTP errors remain local and do not sign out");
        browse.returnToHome();
    }
    browse.browseGames("test", 0, 6200); search = request(browse, Step::SearchPage, 6200);
    browse.transportFailed(search.id);
    require(browse.browsing().error == Error::Transport && browse.status().step == Step::CatalogReady,
        "Transport failure offers another search without losing Home");
    browse.returnToHome();
    browse.browseGames("test", 0, 6300); search = request(browse, Step::SearchPage, 6300);
    require(!browse.nextRequest(66300) && browse.status().step == Step::CatalogReady &&
        browse.browsing().error == Error::Transport, "Request deadline preserves still-valid login");
    browse.returnToHome();
    browse.browseGames("test", 0, 67000); search = request(browse, Step::SearchPage, 67000);
    browse.complete(search.id, 200, profilePage(searchTiles(0, 50)).dump(), 67100);
    browse.browseGames("test", 50, 67200); search = request(browse, Step::SearchPage, 67200);
    browse.complete(search.id, 200, profilePage(searchTiles(0, 50)).dump(), 67300);
    require(browse.games().size() == 50 && browse.browsing().offset == 0 && !browse.browsing().hasNext,
        "Server ignoring pagination cannot trap user in repeated pages");
    browse.browseGames("none", 0, 67400); search = request(browse, Step::SearchPage, 67400);
    browse.complete(search.id, 200, Page.dump(), 67500);
    require(browse.games().empty() && browse.browsing().query == "none" && !browse.browsing().hasNext &&
        browse.browsing().error == Error::None, "Empty results are successful and distinct from failure");
    browse.returnToHome(); browse.returnToProfiles();
    require(!browse.browsing().active && browse.games().empty(), "Profile change clears both catalogs and all search data");
    for (int http : {401, 403})
    {
        NativeAuthProbe rejectedSearch(Mode::ProfilesAndCatalog); catalogReady(rejectedSearch);
        rejectedSearch.browseGames("test", 0, 5000); search = request(rejectedSearch, Step::SearchPage, 5000);
        rejectedSearch.complete(search.id, http, "FAKE_SECRET", 5100);
        require(rejectedSearch.status().step == Step::Failed && rejectedSearch.games().empty() &&
            !rejectedSearch.browsing().active, "Authorization rejection clears both Home and search credentials");
    }
    NativeAuthProbe duplicateSearch(Mode::ProfilesAndCatalog); catalogReady(duplicateSearch);
    duplicateSearch.browseGames("test", 0, 5000); search = request(duplicateSearch, Step::SearchPage, 5000);
    auto duplicates = searchTiles(0, 49); duplicates.push_back(duplicates[0]);
    duplicateSearch.complete(search.id, 200, profilePage(duplicates).dump(), 5100);
    require(duplicateSearch.games().size() == 49 && duplicateSearch.browsing().hasNext,
        "Duplicate tile removal does not turn a full server page into an artificial last page");
    duplicateSearch.browseGames("test", 50, 5200); search = request(duplicateSearch, Step::SearchPage, 5200);
    duplicateSearch.complete(search.id, 200, Page.dump(), 5300);
    require(duplicateSearch.games().size() == 49 && duplicateSearch.browsing().offset == 0 && !duplicateSearch.browsing().hasNext,
        "Empty next page retains the last nonempty page and ends paging");
    const auto browseWidget = [](Json extra = Json::object())
    {
        Json data = {{"defaultSearch", "FAKE_BROWSE_DEFAULT"}, {"defaultSort", "TITLE_A_TO_Z"},
            {"resultsPageType", "multistate_browse_results"}, {"defaultSearchOnLoad", true}};
        data.update(extra);
        return Json{{"type", "BROWSE_PILL_FILTER_ROW_WITH_RESULTS"}, {"presentationData", data.dump()}};
    };
    NativeAuthProbe fullCatalog(Mode::ProfilesAndCatalog); catalogReady(fullCatalog, searchTiles(0, 40));
    require(fullCatalog.browseGames("", 0, 5000), "Catalog opens separately from a title search");
    auto catalogRead = request(fullCatalog, Step::BrowsePage, 5000);
    require(routeBody(catalogRead) == Json{{"featureScheme", "WEB_V1"},
        {"pageContext", {{"pageUri", "browse"}, {"pageId", "default"}}}} &&
        hasHeader(catalogRead, "x-amz-profile-token: FAKE_PROFILE_TOKEN") && responseLimit(Step::BrowsePage) == MaxLunaResponse,
        "Catalog asks for the profile-scoped browse page instead of empty search suggestions");
    auto browseLandingTiles = searchTiles(800, 20); browseLandingTiles.push_back(browseWidget());
    fullCatalog.complete(catalogRead.id, 200, profilePage(browseLandingTiles).dump(), 5100);
    require(fullCatalog.status().step == Step::SearchPage && fullCatalog.games().empty() && fullCatalog.status().catalogParsing.browse,
        "Twenty landing recommendations never substitute for the browse widget's results");
    catalogRead = request(fullCatalog, Step::SearchPage, 5100);
    require(routeBody(catalogRead) == Json{{"featureScheme", "WEB_V1"},
        {"pageContext", {{"pageId", "default"}, {"pageType", "multistate_browse_results"}}},
        {"searchContext", {{"query", "FAKE_BROWSE_DEFAULT"}, {"sort", "TITLE_A_TO_Z"}, {"filtersList", Json::array()}}}},
        "Catalog follows server-defined browse defaults with no invented query or search pagination");
    fullCatalog.complete(catalogRead.id, 200, profilePage(searchTiles(0, 105)).dump(), 5200);
    require(fullCatalog.games().size() == 105 && fullCatalog.status().catalogParsing.browse &&
        fullCatalog.browsing().query.empty() && !fullCatalog.browsing().hasNext &&
        !fullCatalog.browseGames("", 50, 5300), "Entire browse grid remains available for local scrolling past 20 or 50 games");
    fullCatalog.returnToHome();
    require(fullCatalog.games().size() == 40 && fullCatalog.games()[39].id == "FAKE_SEARCH_39",
        "Full catalog preserves original forty-game Home");
    fullCatalog.browseGames("", 0, 5400); catalogRead = request(fullCatalog, Step::BrowsePage, 5400);
    fullCatalog.complete(catalogRead.id, 200, profilePage(Json::array({Json{{"type", "GRID_FILTER_ROW"}}})).dump(), 5500);
    catalogRead = request(fullCatalog, Step::SearchPage, 5500);
    require(routeBody(catalogRead) == Json{{"featureScheme", "WEB_V1"},
        {"pageContext", {{"pageId", "default"}, {"pageType", "landing_library_unfiltered"}}},
        {"searchContext", {{"sort", "TITLE_A_TO_Z"}, {"filtersList", Json::array()}}}},
        "Legacy GridFilterRow follows the public browser's unfiltered-library contract");
    fullCatalog.returnToHome();
    fullCatalog.complete(catalogRead.id, 200, profilePage(searchTiles(0, 105)).dump(), 5600);
    require(fullCatalog.games().size() == 40 && !fullCatalog.browsing().active,
        "Back during catalog followup ignores late results and preserves Home");
    for (const auto& malformed : std::vector<Json>{
        {{"defaultSearch", 42}}, {{"defaultSearch", "bad\nvalue"}},
        {{"resultsPageType", "provision_game"}}, {{"defaultSort", "FAKE_UNKNOWN_SORT"}}})
    {
        fullCatalog.browseGames("", 0, 5700); catalogRead = request(fullCatalog, Step::BrowsePage, 5700);
        fullCatalog.complete(catalogRead.id, 200, profilePage(Json::array({browseWidget(malformed)})).dump(), 5800);
        require(fullCatalog.status().step == Step::CatalogReady && fullCatalog.browsing().error == Error::Response &&
            !fullCatalog.nextRequest(5800), "Unsupported browse configuration never invents a query or executes another action");
        require(fullCatalog.status().catalogParsing.stage!=CatalogParse::Json && fullCatalog.status().catalogParsing.filterWidget,
            "A valid JSON browse shell failing its configuration reports the context stage instead of a misleading JSON error");
        fullCatalog.returnToHome();
    }
    fullCatalog.browseGames("", 0, 5900); catalogRead = request(fullCatalog, Step::BrowsePage, 5900);
    fullCatalog.complete(catalogRead.id, 200, profilePage(Json::array({browseWidget(), browseWidget()})).dump(), 6000);
    require(fullCatalog.browsing().error == Error::Response, "Multiple browse contexts cannot silently select a different collection");
    fullCatalog.returnToHome();
    fullCatalog.browseGames("",0,6000); catalogRead=request(fullCatalog,Step::BrowsePage,6000);
    fullCatalog.complete(catalogRead.id,200,profilePage(Json::array({browseWidget({{"defaultSearchOnLoad",false}})})).dump(),6010);
    catalogRead=request(fullCatalog,Step::SearchPage,6010);
    require(routeBody(catalogRead)["searchContext"]["query"]=="FAKE_BROWSE_DEFAULT",
        "Explicit Catalog selection applies the server-declared default even when website auto-search is disabled");
    fullCatalog.returnToHome();
    auto presetBrowse=browseWidget({{"defaultSearchOnLoad",false},{"defaultSearch",nullptr}});
    presetBrowse["widgets"]=Json::array({Json{{"type","PILL_BUTTON"},{"actions",Json::array({Json{
        {"target","QUICK_SEARCH"},{"payload",Json{{"type","FAKE_OFFERED_PRESET"}}.dump()}}})}}});
    fullCatalog.browseGames("",0,6020); catalogRead=request(fullCatalog,Step::BrowsePage,6020);
    fullCatalog.complete(catalogRead.id,200,profilePage(Json::array({presetBrowse})).dump(),6030);
    catalogRead=request(fullCatalog,Step::SearchPage,6030);
    require(routeBody(catalogRead)["searchContext"]["query"]=="FAKE_OFFERED_PRESET",
        "A filter shell without a declared default follows its offered non-channel QUICK_SEARCH preset");
    fullCatalog.returnToHome();
    presetBrowse["widgets"][0]["actions"][0]["target"]="FAKE_UNSUPPORTED_ACTION";
    fullCatalog.browseGames("",0,6040); catalogRead=request(fullCatalog,Step::BrowsePage,6040);
    fullCatalog.complete(catalogRead.id,200,profilePage(Json::array({presetBrowse})).dump(),6050);
    require(fullCatalog.browsing().error==Error::Response && !fullCatalog.nextRequest(6050),
        "A filter shell without a known offered query stays recoverable instead of inventing a request");
    fullCatalog.returnToHome();
    fullCatalog.browseGames("", 0, 6100); catalogRead = request(fullCatalog, Step::BrowsePage, 6100);
    fullCatalog.complete(catalogRead.id, 200, profilePage(searchTiles(0, 70)).dump(), 6200);
    require(fullCatalog.games().size() == 70 && fullCatalog.status().step == Step::CatalogReady,
        "Browse pages that already contain their grid need no followup");
    fullCatalog.returnToHome();
    fullCatalog.browseGames("", 0, 6300); catalogRead = request(fullCatalog, Step::BrowsePage, 6300);
    fullCatalog.transportFailed(catalogRead.id);
    require(fullCatalog.status().step == Step::CatalogReady && fullCatalog.browsing().error == Error::Transport,
        "Browse landing transport errors preserve login");
    fullCatalog.returnToHome();
    NativeAuthProbe routedCatalog(Mode::ProfilesAndCatalog); profiles(routedCatalog);
    require(routedCatalog.chooseProfile(0,4500), "Catalog navigation fixture logs into its selected profile");
    auto routedRead=request(routedCatalog,Step::PlayerLogin,4500);
    routedCatalog.complete(routedRead.id,200,Json{{"token","FAKE_PROFILE_TOKEN"}}.dump(),4600);
    routedRead=request(routedCatalog,Step::Catalog,4600);
    auto routedHome=profilePage(searchTiles(0,40));
    routedHome["pageMemberGroups"]["navigation"]={{"widgets",Json::array({Json{{"type","NAV_ITEM"},
        {"actions",Json::array({Json{{"target","/browse"},{"serviceToken","FAKE_BROWSE_ROUTE"},
            {"payload",Json{{"quickSearch","FAKE_NAV_QUERY"}}.dump()}}})}}})}};
    routedCatalog.complete(routedRead.id,200,routedHome.dump(),4700);
    require(routedCatalog.browseGames("",0,5000), "Catalog retains the navigation supplied with Home");
    routedRead=request(routedCatalog,Step::BrowsePage,5000);
    require(routeBody(routedRead)==Json{{"featureScheme","WEB_V1"},{"serviceToken","FAKE_BROWSE_ROUTE"}},
        "Browse uses the server's navigation token instead of discarding it for a path-only request");
    routedCatalog.complete(routedRead.id,200,profilePage(Json::array({browseWidget({{"defaultSearchOnLoad",false}})})).dump(),5100);
    routedRead=request(routedCatalog,Step::SearchPage,5100);
    require(routeBody(routedRead)["searchContext"]["query"]=="FAKE_NAV_QUERY",
        "An explicit navigation quickSearch loads results even when the filter shell disables its automatic default");
    routedCatalog.complete(routedRead.id,200,profilePage(searchTiles(0,105)).dump(),5200);
    require(routedCatalog.games().size()==105,"Token-scoped browse results render the full returned grid");
    routedCatalog.returnToHome(); routedCatalog.returnToProfiles();
    require(routedCatalog.chooseProfile(0,5300),"Profile switching starts a fresh catalog scope");
    routedRead=request(routedCatalog,Step::PlayerLogin,5300);
    routedCatalog.complete(routedRead.id,200,Json{{"token","FAKE_OTHER_TOKEN"}}.dump(),5400);
    routedRead=request(routedCatalog,Step::Catalog,5400);
    routedCatalog.complete(routedRead.id,200,profilePage(searchTiles(0,40)).dump(),5500);
    routedCatalog.browseGames("",0,5600); routedRead=request(routedCatalog,Step::BrowsePage,5600);
    require(!routeBody(routedRead).contains("serviceToken"),"Previous profile's browse token is wiped on profile changes");
    fullCatalog.browseGames("", 0, 6400); catalogRead = request(fullCatalog, Step::BrowsePage, 6400);
    require(!fullCatalog.nextRequest(66400) && fullCatalog.status().step == Step::CatalogReady &&
        fullCatalog.browsing().error == Error::Transport, "Browse landing timeout remains recoverable");
    fullCatalog.returnToHome();
    fullCatalog.browseGames("", 0, 67000); catalogRead = request(fullCatalog, Step::BrowsePage, 67000);
    fullCatalog.complete(catalogRead.id, 403, "FAKE_PRIVATE_ERROR", 67100);
    require(fullCatalog.status().step == Step::Failed && fullCatalog.games().empty() && !fullCatalog.browsing().active,
        "Browse authorization rejection clears both collections");
    const auto accessReply=[](const Request& r,const char* access="ENTITLED")
    {
        Json entries=Json::array(); const auto body=Json::parse(r.body);
        for (const auto& id:body.at("productIdList"))
            entries.push_back({{"productId",id},{"status",access}});
        return Json{{"productEntitlementStatuses",entries},{"failedEntitlementSources",Json::object()}};
    };
    NativeAuthProbe included(Mode::ProfilesAndCatalog); catalogReady(included,searchTiles(0,40));
    require(included.browseGames("query",0,5000,true),"Included-only search starts with explicit access filtering");
    auto includedRead=request(included,Step::SearchPage,5000);
    included.complete(includedRead.id,200,profilePage(searchTiles(0,60)).dump(),5100);
    require(included.status().step==Step::CatalogAccess && included.games().empty(),
        "Unverified results never become visible as included games before access checks finish");
    unsigned batches=0;
    while(included.status().step==Step::CatalogAccess)
    {
        includedRead=request(included,Step::CatalogAccess,5200+batches);
        const auto body=Json::parse(includedRead.body);
        require(includedRead.url=="https://proxy-prod.us-east-1.tempo.digital.a2z.com/getProductEntitlementStatuses" &&
            body["productIdList"].size()<=25 && body["stage"]=="Release" && hasHeader(includedRead,"x-amz-profile-token: FAKE_PROFILE_TOKEN"),
            "Access requests use the browser's read-only endpoint and account/profile scope in bounded batches");
        auto reply=accessReply(includedRead);
        for (auto& item:reply["productEntitlementStatuses"])
        {
            const auto id=item["productId"].get<std::string>(); const auto index=std::stoi(id.substr(12));
            if (index%2) item["status"]=index%3?"NOT_ENTITLED":"RENTAL_AVAILABLE";
        }
        std::reverse(reply["productEntitlementStatuses"].begin(),reply["productEntitlementStatuses"].end());
        included.complete(includedRead.id,200,reply.dump(),5300+batches++);
    }
    require(batches==3 && included.games().size()==30 && included.browsing().hasNext &&
        included.status().catalogParsing.accessChecked==60 && included.status().catalogParsing.accessIncluded==30 &&
        included.status().catalogParsing.accessHidden==30,"Only ENTITLED products survive, regardless of response ordering or subscription branding");
    for (const auto& g:included.games()) require(std::stoi(g.id.substr(12))%2==0,"Included view contains no confirmed unavailable or rent-to-access titles");
    require(included.browseGames("query",50,5400,true),"Included search retains paging based on raw results");
    includedRead=request(included,Step::SearchPage,5400);
    included.complete(includedRead.id,200,profilePage(searchTiles(0,60)).dump(),5500);
    require(included.status().step==Step::CatalogReady && included.games().size()==30 && !included.browsing().hasNext && !included.nextRequest(5500),
        "Repeated raw pages stop paging even when the displayed previous page was filtered");
    require(included.browseGames("query",0,5600,false),"Show all explicitly opts out of access filtering");
    includedRead=request(included,Step::SearchPage,5600);
    included.complete(includedRead.id,200,profilePage(searchTiles(0,60)).dump(),5700);
    require(included.status().step==Step::CatalogReady && included.games().size()==60 && !included.browsing().includedOnly && !included.nextRequest(5700),
        "Show all restores all returned titles without access checks or account mutations");
    require(included.browseGames("query",0,5800,true) && included.games().empty(),
        "Switching from Show all to Included removes the unverified old grid before any new response");
    includedRead=request(included,Step::SearchPage,5800); included.transportFailed(includedRead.id);
    require(included.games().empty() && included.browsing().includedOnly && included.browsing().error==Error::Transport,
        "A failed mode switch never labels the previous unfiltered list as Included");
    included.returnToHome();
    require(included.games().size()==40,"Filtering preserves the original Home and navigation");
    included.browseGames("query",0,5810,true); includedRead=request(included,Step::SearchPage,5810);
    auto staged=profilePage(searchTiles(0,3)); staged["pageContext"]={{"productStage","FAKE_SERVER_STAGE"}};
    included.complete(includedRead.id,200,staged.dump(),5820); includedRead=request(included,Step::CatalogAccess,5820);
    require(Json::parse(includedRead.body)["stage"]=="FAKE_SERVER_STAGE","Access checks preserve the server's product stage instead of sending an empty enum");
    included.complete(includedRead.id,400,Json{{"__type","ValidationException"},
        {"message","Value at productIdList failed: member length must be less than or equal to 1 FAKE_PRIVATE_TEXT"}}.dump(),5830);
    require(included.status().step==Step::CatalogAccess && included.status().catalogParsing.accessSingleFallback &&
        included.status().catalogParsing.accessFieldHints==6,"A confirmed list validation failure can reduce only the read-only access batch size");
    for (unsigned i=0;i<3;++i)
    {
        includedRead=request(included,Step::CatalogAccess,5840+i);
        require(Json::parse(includedRead.body)["productIdList"].size()==1,"Validation fallback uses the source-verified single-product request shape");
        included.complete(includedRead.id,200,accessReply(includedRead).dump(),5850+i);
    }
    require(included.games().size()==3 && included.status().catalogParsing.accessIncluded==3,"Single-product fallback completes all checks before publishing the included grid");
    included.returnToHome();
    for (unsigned failure=0;failure<7;++failure)
    {
        included.browseGames("query",0,6000,true); auto r=request(included,Step::SearchPage,6000);
        included.complete(r.id,200,profilePage(searchTiles(0,3)).dump(),6100);
        r=request(included,Step::CatalogAccess,6100); auto reply=accessReply(r);
        if (failure==0) reply["productEntitlementStatuses"].erase(0);
        if (failure==1) reply["productEntitlementStatuses"][0]["productId"]="FAKE_UNKNOWN_PRODUCT";
        if (failure==2) reply["productEntitlementStatuses"][1]=reply["productEntitlementStatuses"][0];
        if (failure==3) reply["errorCode"]=1;
        if (failure==4) reply["failedEntitlementSources"]={{"THIRD_PARTY",true}};
        if (failure==5) included.transportFailed(r.id);
        else included.complete(r.id,failure==6?500:200,reply.dump(),6200);
        require(included.status().step==Step::CatalogReady && included.games().empty() && included.browsing().error!=Error::None && !included.nextRequest(6200),
            "Incomplete, duplicate, mismatched and failed access checks remain recoverable errors without showing unverified games");
        included.returnToHome(); require(included.games().size()==40,"An access failure cannot erase Home or sign-in");
    }
    included.browseGames("query",0,6300,true); includedRead=request(included,Step::SearchPage,6300);
    included.complete(includedRead.id,200,profilePage(searchTiles(0,50)).dump(),6400);
    for(unsigned batch=0;batch<2;++batch)
    {
        includedRead=request(included,Step::CatalogAccess,6400+batch);
        included.complete(includedRead.id,200,accessReply(includedRead,"NOT_ENTITLED").dump(),6410+batch);
    }
    require(included.games().empty() && included.browsing().hasNext && included.browsing().error==Error::None &&
        included.browseGames("query",50,6500,true),"A search page with no included games can still advance to later raw results");
    includedRead=request(included,Step::SearchPage,6500);
    included.complete(includedRead.id,200,profilePage(searchTiles(50,2)).dump(),6600);
    includedRead=request(included,Step::CatalogAccess,6600);
    included.returnToHome(); included.complete(includedRead.id,200,accessReply(includedRead).dump(),6700);
    require(included.games().size()==40 && !included.browsing().active && !included.nextRequest(6700),
        "Back during access checks drops pending candidates and ignores late replies");
    NativeAuthProbe historyRoute(Mode::ProfilesAndCatalog); catalogReady(historyRoute);
    for (const auto& path : {"", "/game/fake/ASIN", "https://evil.invalid/game/foo", "game/foo?token=FAKE_SECRET",
        "game/../foo", "settings/account", "game/", "game//foo"})
        require(!historyRoute.openGamePath(path, "Fake title", 4900) && historyRoute.status().step == Step::CatalogReady,
            "Stored history routes cannot send arbitrary navigation or change a ready session");
    require(!historyRoute.openGamePath("game/fake/FAKE_ASIN", "bad\nname", 4900), "Saved title controls are rejected");
    require(historyRoute.openGamePath("game/absent-from-home/FAKE_OLD_ASIN", "A recently played game", 4900),
        "A recent game can open even when absent from today's Home response");
    auto oldGame = request(historyRoute, Step::DetailPage, 4900);
    require(routeBody(oldGame) == Json{{"featureScheme", "WEB_V1"},
        {"pageContext", {{"pageId", "default"}, {"pageUri", "game/absent-from-home/FAKE_OLD_ASIN"}}}} &&
        hasHeader(oldGame, "x-amz-profile-token: FAKE_PROFILE_TOKEN"),
        "Recently Played rereads the authenticated game page instead of saving or replaying a service/play token");
    historyRoute.complete(oldGame.id, 200, detailPage().dump(), 5000);
    require(historyRoute.status().step == Step::DetailReady && !historyRoute.status().detailParsing.hasPlayToken &&
        !historyRoute.nextRequest(5001), "Opening history still requires explicit Play and the existing launch notices/checks");
    historyRoute.returnToCatalog();
    require(historyRoute.games().size() == 1 && historyRoute.currentGamePath().empty(),
        "Back from a recent-only game preserves the original Home and drops the transient path");
    require(historyRoute.openGame(0, 5100) && historyRoute.currentGamePath() == "game/fake/FAKE_ASIN",
        "Ordinary game selection exposes only its normalized navigation path for history");
    historyRoute.returnToCatalog();
    historyRoute.browseGames("query", 0, 5200);
    require(!historyRoute.openGamePath("game/fake/FAKE_ASIN", "Fake title", 5200),
        "History cannot replace a pending catalog request");
    // Profile-management writes are synthetic: no Amazon account is touched.
    using PM=ProfileManager;
    const auto managementTile=[](const char* id,const char* name,Json flags=Json::object())
    {
        auto t=tile(id,name,flags);
        t["actions"].push_back({{"target","PLAYER_DETAILS_EDIT"},{"serviceToken",std::string("FAKE_EDIT_")+id}});
        return t;
    };
    const auto managementPicker=[&](Json extra=Json::array(),bool locked=false,const char* name="Player One")
    {
        auto tiles=Json::array({managementTile("FAKE_PROFILE_1",name),managementTile("FAKE_PROFILE_2","Player Two")});
        for (const auto& t:extra) tiles.push_back(t);
        tiles.push_back({{"type","PLAYER_SELECTION_TILE"},{"presentationData",Json{{"primaryText","Add profile"}}.dump()},
            {"actions",Json::array({Json{{"target","PLAYER_CREATE"},{"serviceToken","FAKE_CREATE_ROUTE"}}})}});
        return profilePage(Json::array({Json{{"type","PLAYER_SELECTION_COLLECTION"},
            {"presentationData",Json{{"creationAndRemovalLockEnabled",locked}}.dump()},{"widgets",tiles}}}));
    };
    const auto managementForm=[](const char* name="Player One",const char* avatar="avatar_1")
    {
        auto p=profilePage(Json::array({Json{{"type","PLAYER_DETAILS_FORM"},
            {"presentationData",Json{{"profileName",name},{"avatarId",avatar},{"displayName","FakeGamerTag"}}.dump()}},
            Json{{"type","BASIC_BUTTON"},{"actions",Json::array({Json{{"target","PLAYER_DETAILS_SAVE"}}})}}}));
        p["pageMemberGroups"]["avatarCarousel"]={{"widgets",Json::array()}};
        for (const auto* a:{"avatar_1","avatar_2"})
            p["pageMemberGroups"]["avatarCarousel"]["widgets"].push_back({{"type","PLAYER_AVATAR_TILE"},
                {"presentationData",Json{{"avatarId",a},{"displayName","Fake avatar"},
                    {"avatarUrl","https://m.media-amazon.com/images/I/fixture-avatar.png"}}.dump()}});
        return p;
    };
    const auto pmRequest=[&](PM& manager,std::int64_t time)
    {
        auto r=manager.nextRequest(time);
        require(r && r->step==Step::ProfileManagement && r->id>=(std::uint64_t{1}<<60),
            "Management requests use independent IDs and a bounded response class");
        require(r->url.rfind("https://proxy-prod.us-east-1.tempo.digital.a2z.com/",0)==0 &&
            responseLimit(r->step)==MaxLunaResponse,"Profile reads/writes stay at fixed authenticated origin with bounded responses");
        require(!manager.nextRequest(time),"Management is single-flight");
        return *r;
    };
    const auto startManagement=[&](NativeAuthProbe& account,PM& manager,Json picker)
    {
        profiles(account);
        require(manager.begin(5000),"Manage Profiles starts only at the authenticated profile chooser");
        auto r=pmRequest(manager,5000);
        require(routeBody(r)==Json{{"featureScheme","WEB_V1"},{"pageContext",{{"pageType","FAKE_PICKER_TYPE"},{"pageId","FAKE_PICKER_ID"}}}},
            "Management refreshes the server's configured profile page");
        manager.complete(r.id,200,picker.dump(),5100);
        require(manager.status().state==PM::State::Picker,"Profile picker routes parsed");
    };
    const auto openManagedEdit=[&](PM& manager,Json returnedForm=Json())
    {
        require(manager.edit(0,5200),"An unlocked server-advertised edit route may be opened");
        auto r=pmRequest(manager,5200);
        require(r.url.find("/player/playerLogin")!=std::string::npos && routeBody(r)==Json{{"profileId","FAKE_PROFILE_1"}} &&
            !hasHeader(r,"x-amz-profile-token: FAKE_PROFILE_TOKEN"),"Editing logs into the actual target profile without reusing another profile token");
        manager.complete(r.id,200,Json{{"token","FAKE_TARGET_PROFILE_TOKEN"}}.dump(),5300);
        r=pmRequest(manager,5300);
        require(routeBody(r)["serviceToken"]=="FAKE_EDIT_FAKE_PROFILE_1" && hasHeader(r,"x-amz-profile-token: FAKE_TARGET_PROFILE_TOKEN"),
            "Only the server's edit route and authenticated target token read the form");
        manager.complete(r.id,200,(returnedForm.is_null()?managementForm():returnedForm).dump(),5400);
        require(manager.status().state==PM::State::Editing && !manager.canSave(),"Opening a form does not write unchanged details");
    };
    const auto editPanel=[&](const char* route="FAKE_DETAILS_ROUTE")
    {
        auto page=profilePage(Json::array({Json{{"type","EDIT_PROFILE_PANEL"},{"presentationData",Json::object()}}}));
        page["pageMemberGroups"]["editProfileChangeButton"]={{"widgets",Json::array({Json{{"type","BASIC_BUTTON"},
            {"actions",Json::array({Json{{"target","PLAYER_DETAILS_EDIT"},{"serviceToken",route}}})}}})}};
        for (const auto* name:{"editProfilePinButton","editProfileRemoveButton"})
            page["pageMemberGroups"][name]={{"widgets",Json::array({Json{{"type","BASIC_BUTTON"},
                {"actions",Json::array({Json{{"target",std::string(name)=="editProfilePinButton"?"PLAYER_PIN_MANAGEMENT":"PLAYER_REMOVE"},
                    {"serviceToken","FAKE_NEVER_FOLLOW_PIN_OR_DELETE"}}})}}})}};
        return page;
    };
    NativeAuthProbe panelAccount(Mode::ProfilesAndCatalog); PM panelManager(panelAccount);
    startManagement(panelAccount,panelManager,managementPicker());
    require(panelManager.edit(0,5200),"Panel edit begins with the selected profile's advertised route");
    auto panelRead=pmRequest(panelManager,5200);
    panelManager.complete(panelRead.id,200,Json{{"token","FAKE_TARGET_PROFILE_TOKEN"}}.dump(),5300);
    panelRead=pmRequest(panelManager,5300); panelManager.complete(panelRead.id,200,editPanel().dump(),5400);
    require(panelManager.status().state==PM::State::LoadingForm && !panelManager.status().writeAttempted && !panelManager.canSave(),
        "An EditProfilePanel is followed as a read instead of being mistaken for a broken form");
    panelRead=pmRequest(panelManager,5400);
    require(routeBody(panelRead)["serviceToken"]=="FAKE_DETAILS_ROUTE" && hasHeader(panelRead,"x-amz-profile-token: FAKE_TARGET_PROFILE_TOKEN"),
        "Only the panel's editProfileChangeButton route is followed, retaining target authentication and excluding PIN/deletion routes");
    panelManager.complete(panelRead.id,200,managementForm().dump(),5500);
    require(panelManager.status().state==PM::State::Editing && !panelManager.canSave(),"The panel followup opens the real name/avatar editor without changing the account");
    panelManager.setName("Panel saved name"); panelManager.save(5600); panelRead=pmRequest(panelManager,5600);
    panelManager.complete(panelRead.id,200,"{}",5700); panelRead=pmRequest(panelManager,5700);
    panelManager.complete(panelRead.id,200,managementPicker(Json::array(),false,"Panel saved name").dump(),5800);
    panelRead=pmRequest(panelManager,5800); panelManager.complete(panelRead.id,200,Json{{"token","FAKE_VERIFY_TOKEN"}}.dump(),5900);
    panelRead=pmRequest(panelManager,5900); panelManager.complete(panelRead.id,200,editPanel().dump(),6000);
    panelRead=pmRequest(panelManager,6000);
    require(hasHeader(panelRead,"x-amz-profile-token: FAKE_VERIFY_TOKEN"),"Read-back panel traversal uses the newly obtained profile token");
    panelManager.complete(panelRead.id,200,managementForm("Panel saved name").dump(),6100);
    require(panelManager.status().state==PM::State::Saved,"Saving through an edit panel is verified against the final form, not panel display text");
    NativeAuthProbe loopAccount(Mode::ProfilesAndCatalog); PM loopManager(loopAccount);
    startManagement(loopAccount,loopManager,managementPicker()); loopManager.edit(0,5200);
    auto loopRead=pmRequest(loopManager,5200); loopManager.complete(loopRead.id,200,Json{{"token","FAKE_PROFILE_TOKEN"}}.dump(),5300);
    for (unsigned i=0;i<4;++i)
    {
        loopRead=pmRequest(loopManager,5400+i); const auto token="FAKE_LOOP_"+std::to_string(i);
        loopManager.complete(loopRead.id,200,editPanel(token.c_str()).dump(),5500+i);
    }
    require(loopManager.status().state==PM::State::Failed && !loopManager.status().writeAttempted && !loopManager.nextRequest(5600),
        "Broken panels cannot cause an unbounded chain of requests or an account mutation");
    NativeAuthProbe guidanceAccount(Mode::ProfilesAndCatalog); PM guidance(guidanceAccount);
    startManagement(guidanceAccount,guidance,managementPicker()); guidance.create(5200);
    auto guidanceRead=pmRequest(guidance,5200);
    auto guidancePage=profilePage(Json::array({Json{{"type","PROFILE_TYPE_GUIDANCE"},{"presentationData",Json{{"options",Json::array({
        Json{{"type","CHILD"}},Json{{"type","ADULT"}}})}}.dump()}},Json{{"type","BASIC_BUTTON"},
        {"actions",Json::array({Json{{"target","PLAYER_CREATE"},{"serviceToken","FAKE_AFTER_ADULT_GUIDANCE"}}})}}}));
    guidance.complete(guidanceRead.id,200,guidancePage.dump(),5300);
    require(guidance.status().state==PM::State::LoadingForm && !guidance.status().writeAttempted,
        "An offered adult option can traverse the profile-type guidance page without a mutation");
    guidanceRead=pmRequest(guidance,5300);
    require(routeBody(guidanceRead)["serviceToken"]=="FAKE_AFTER_ADULT_GUIDANCE","Adult creation follows the guidance page's explicit PLAYER_CREATE action");
    guidance.complete(guidanceRead.id,200,managementForm().dump(),5400);
    guidanceRead=pmRequest(guidance,5400);
    require(guidanceRead.url.find("generatePlayerName")!=std::string::npos,"Guided creation enters the regular name-token flow");
    for (const auto& name:{std::string(),std::string(51,'a'),std::string("<bad>"),std::string("bad\nname"),std::string("\xc0\xaf")})
        require(!PM::validName(name),"Profile names follow browser length/character/UTF8 bounds");
    require(PM::validName(u8"Pokémon Player") && PM::validName(std::string(50,'a')),"Unicode profile names within browser limits are valid");
    NativeAuthProbe managedAccount(Mode::ProfilesAndCatalog); PM manager(managedAccount);
    startManagement(managedAccount,manager,managementPicker());
    require(manager.profiles().size()==2 && manager.canCreate(),"Create tile is a route, not a fabricated profile");
    openManagedEdit(manager);
    require(manager.setName(" New name "),"Editor trims the submitted profile name"); manager.selectAvatar(1);
    require(manager.canSave() && !manager.nextRequest(5500),"Editing fields never sends a write without explicit Save");
    require(manager.save(5500) && !manager.save(5500) && !manager.back(5500),"Save is one-shot and cannot be cancelled or resubmitted in flight");
    auto writeProfile=pmRequest(manager,5500);
    require(writeProfile.url.find("/player/setPlayer")!=std::string::npos && routeBody(writeProfile)==Json{{"avatarId","avatar_2"},
        {"displayName","FakeGamerTag"},{"profileName","New name"}} && hasHeader(writeProfile,"x-amz-profile-token: FAKE_TARGET_PROFILE_TOKEN"),
        "Edit sends only the proposed name/avatar and unchanged gamer tag, under the target's token");
    manager.complete(writeProfile.id,200,"{}",5600);
    require(manager.status().state==PM::State::Verifying && managedAccount.profiles()[0].name=="Player One",
        "HTTP success alone cannot optimistically change the profile list or report Saved");
    auto verify=pmRequest(manager,5600);
    manager.complete(verify.id,200,managementPicker(Json::array(),false,"New name").dump(),5700);
    verify=pmRequest(manager,5700);
    manager.complete(verify.id,200,Json{{"token","FAKE_FRESH_TARGET_TOKEN"}}.dump(),5800);
    verify=pmRequest(manager,5800);
    require(hasHeader(verify,"x-amz-profile-token: FAKE_FRESH_TARGET_TOKEN"),"Verification obtains fresh target authorization");
    manager.complete(verify.id,200,managementForm("New name","avatar_2").dump(),5900);
    require(manager.status().state==PM::State::Saved && !manager.nextRequest(5900),"Saved requires read-back matching both profile name and avatar ID");
    manager.close();
    require(managedAccount.status().step==Step::ProfilesPage,"Leaving management reloads the ordinary profile chooser");
    auto ordinaryPicker=request(managedAccount,Step::ProfilesPage,6000);
    managedAccount.complete(ordinaryPicker.id,200,managementPicker(Json::array(),false,"New name").dump(),6100);
    require(managedAccount.profiles()[0].name=="New name","Only refreshed server data updates the ordinary profile picker");
    NativeAuthProbe nullableFormAccount(Mode::ProfilesAndCatalog); PM nullableForm(nullableFormAccount);
    startManagement(nullableFormAccount,nullableForm,managementPicker());
    auto sourceForm=managementForm();
    auto formData=Json::parse(sourceForm["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["presentationData"].get<std::string>());
    formData["profileId"]=nullptr; formData["profileType"]=nullptr; formData["profileName"]=nullptr;
    sourceForm["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["presentationData"]=formData.dump();
    sourceForm["pageMemberGroups"]["profilePrivacyFooter"]={{"widgets",Json::array({Json{{"type","BASIC_CHECKBOX"}}})}};
    for (auto& w:sourceForm["pageMemberGroups"]["avatarCarousel"]["widgets"])
    {
        auto data=Json::parse(w["presentationData"].get<std::string>());
        w["id"]="item_player_avatar_"+data["avatarId"].get<std::string>();
        data.erase("avatarId"); data["displayName"]=nullptr; w["presentationData"]=data.dump();
    }
    openManagedEdit(nullableForm,sourceForm);
    require(nullableForm.form().name.empty() && nullableForm.form().avatars[0].id=="avatar_1" &&
        nullableForm.diagnostics().stage==PM::ParseStage::Complete && nullableForm.diagnostics().avatars==2,
        "Avatar widget suffixes, optional null metadata and the dormant child/teen footer match the adult browser form");
    require(nullableForm.setName("Chosen profile name") && nullableForm.canSave() && !nullableForm.nextRequest(5500),
        "A profile with no optional profileName can be named explicitly, with no write before Save");
    NativeAuthProbe createAccount(Mode::ProfilesAndCatalog); PM creator(createAccount);
    startManagement(createAccount,creator,managementPicker());
    require(creator.create(5200),"Create starts only through the server-advertised route");
    auto createRead=pmRequest(creator,5200);
    require(routeBody(createRead)["serviceToken"]=="FAKE_CREATE_ROUTE","Creation reads the server's form before collecting changes");
    creator.complete(createRead.id,200,managementForm().dump(),5300);
    createRead=pmRequest(creator,5300);
    require(createRead.url.find("/player/generatePlayerName")!=std::string::npos && routeBody(createRead).empty(),
        "Creation requests the browser's generated gamer tag and name token");
    creator.complete(createRead.id,200,Json{{"displayName","GeneratedTag"},{"nameToken","FAKE_NAME_TOKEN_SECRET"}}.dump(),5400);
    require(creator.setName("New Player"),"New profile name is editable"); creator.selectAvatar(1);
    require(creator.save(5500),"Explicit Save may create a prepared adult profile");
    writeProfile=pmRequest(creator,5500);
    require(writeProfile.url.find("/player/createPlayer")!=std::string::npos && routeBody(writeProfile)==Json{
        {"avatarId","avatar_2"},{"displayName","GeneratedTag"},{"nameToken","FAKE_NAME_TOKEN_SECRET"},{"profileName","New Player"},{"profileType","ADULT"}},
        "Create uses the generated name token and explicitly supported adult type");
    creator.complete(writeProfile.id,200,"{}",5600);
    verify=pmRequest(creator,5600);
    creator.complete(verify.id,200,managementPicker(Json::array({managementTile("FAKE_PROFILE_NEW","New Player")})).dump(),5700);
    verify=pmRequest(creator,5700);
    require(routeBody(verify)["profileId"]=="FAKE_PROFILE_NEW","New profile verification finds a new server identity, not a same-name existing profile");
    creator.complete(verify.id,200,Json{{"token","FAKE_NEW_PROFILE_TOKEN"}}.dump(),5800);
    verify=pmRequest(creator,5800);
    creator.complete(verify.id,200,managementForm("New Player","avatar_2").dump(),5900);
    require(creator.status().state==PM::State::Saved,"Created profile is verified by rereading its authenticated name/avatar");
    NativeAuthProbe pickerActionsAccount(Mode::ProfilesAndCatalog); PM pickerActions(pickerActionsAccount);
    auto actualPicker=managementPicker();
    auto& actualTiles=actualPicker["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["widgets"];
    actualTiles[0]["actions"][1]["target"]="PLAYER_EDIT";
    actualTiles[0]["actions"][1]["payload"]=Json{{"profileId","FAKE_PROFILE_1"},{"profileType","ADULT"}}.dump();
    auto sourcePinData=Json::parse(actualTiles[0]["presentationData"].get<std::string>());
    sourcePinData["sourcePinExists"]=true; actualTiles[0]["presentationData"]=sourcePinData.dump();
    actualTiles[2]["actions"][0]["target"]="PLAYER_ADD";
    startManagement(pickerActionsAccount,pickerActions,actualPicker);
    require(pickerActions.profiles()[0].editable && pickerActions.canCreate() && pickerActions.diagnostics().sourcePins==1,
        "Browser picker PLAYER_EDIT/PLAYER_ADD routes are supported; a source-PIN setup prompt alone does not lock editing");
    openManagedEdit(pickerActions);
    pickerActions.back(5500); auto pickerRefresh=pmRequest(pickerActions,5500);
    pickerActions.complete(pickerRefresh.id,200,actualPicker.dump(),5600);
    require(pickerActions.create(5700),"PLAYER_ADD opens the server's create/import selection page");
    auto options=pmRequest(pickerActions,5700);
    pickerActions.complete(options.id,200,profilePage(Json::array({Json{{"type","BASIC_BUTTON"},
        {"actions",Json::array({Json{{"target","PLAYER_CREATE"},{"serviceToken","FAKE_ADULT_CREATE"}}})}}})).dump(),5800);
    options=pmRequest(pickerActions,5800);
    require(routeBody(options)["serviceToken"]=="FAKE_ADULT_CREATE" && !pickerActions.status().writeAttempted,
        "Creation follows the offered PLAYER_CREATE option without a mutation or importing an existing profile");
    pickerActions.complete(options.id,200,managementForm().dump(),5900);
    options=pmRequest(pickerActions,5900);
    require(options.url.find("/player/generatePlayerName")!=std::string::npos,"Create options reach the normal adult form flow");
    NativeAuthProbe noRouteAccount(Mode::ProfilesAndCatalog); PM noRoute(noRouteAccount);
    auto noRoutePicker=managementPicker();
    noRoutePicker["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["widgets"][0]["actions"].erase(1);
    startManagement(noRouteAccount,noRoute,noRoutePicker);
    require(!noRoute.edit(0,5200) && noRoute.status().issue==PM::Issue::Unsupported && noRoute.status().blockedBy==PM::MissingRoute,
        "Missing edit routes are reported separately from PIN or account restrictions");
    NativeAuthProbe wrongEditAccount(Mode::ProfilesAndCatalog); PM wrongEdit(wrongEditAccount);
    profiles(wrongEditAccount); require(wrongEdit.begin(5000),"Edit identity mismatch fixture begins with account authorization");
    auto wrongEditRead=pmRequest(wrongEdit,5000); auto wrongEditPicker=actualPicker;
    wrongEditPicker["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["widgets"][0]["actions"][1]["payload"]=
        Json{{"profileId","FAKE_DIFFERENT_PROFILE"}}.dump();
    wrongEdit.complete(wrongEditRead.id,200,wrongEditPicker.dump(),5100);
    require(wrongEdit.status().state==PM::State::Failed && !wrongEdit.edit(0,5200) && !wrongEdit.nextRequest(5200),
        "An advertised edit action for a different profile cannot log in or write to either identity");
    for (bool ambiguous:{false,true})
    {
        NativeAuthProbe a(Mode::ProfilesAndCatalog); PM add(a); startManagement(a,add,actualPicker);
        require(add.create(5200),"Add-profile option validation starts from an advertised picker route");
        auto r=pmRequest(add,5200);
        auto choices=Json::array({Json{{"type","BASIC_BUTTON"},{"actions",Json::array({Json{
            {"target","PROFILE_SELECT"},{"serviceToken","FAKE_IMPORT_ONLY"}}})}}});
        if (ambiguous) for (const auto* token:{"FAKE_CREATE_ONE","FAKE_CREATE_TWO"})
            choices.push_back({{"type","BASIC_BUTTON"},{"actions",Json::array({Json{{"target","PLAYER_CREATE"},{"serviceToken",token}}})}});
        add.complete(r.id,200,profilePage(choices).dump(),5300);
        require(add.status().state==PM::State::Failed && !add.status().writeAttempted && !add.nextRequest(5300),
            "Import-only or ambiguous create choices never fabricate an adult-create route or perform an account write");
    }
    for (const auto& protection:std::vector<Json>{{{"isLocked",true}},{{"isHouseholdMismatch",true}},
        {{"migrationStatus",1}},{{"profileType","CHILD"}},{{"profileType","TEEN"}}})
    {
        NativeAuthProbe a(Mode::ProfilesAndCatalog); PM protectedManager(a);
        auto picker=managementPicker(); picker["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["widgets"][0]=managementTile("FAKE_PROFILE_1","Player One",protection);
        startManagement(a,protectedManager,picker);
        require(!protectedManager.edit(0,5200) && !protectedManager.nextRequest(5200),"Protected/importing/child profiles do not enter unsupported edit flows");
    }
    NativeAuthProbe lockedAccount(Mode::ProfilesAndCatalog); PM lockedManager(lockedAccount);
    startManagement(lockedAccount,lockedManager,managementPicker(Json::array(),true));
    require(!lockedManager.canCreate() && !lockedManager.create(5200) && !lockedManager.nextRequest(5200),
        "Account creation lock cannot be bypassed by an add-profile tile");
    for (const auto& failure:std::vector<std::pair<int,PM::Issue>>{{20,PM::Issue::Limit},{22,PM::Issue::NameRejected},{28,PM::Issue::Protected},{30,PM::Issue::Protected}})
    {
        NativeAuthProbe a(Mode::ProfilesAndCatalog); PM rejected(a); startManagement(a,rejected,managementPicker()); openManagedEdit(rejected);
        rejected.setName("Changed name"); rejected.save(5500); auto r=pmRequest(rejected,5500);
        rejected.complete(r.id,200,Json{{"errorCode",failure.first},{"message","FAKE_PRIVATE_SERVER_TEXT"}}.dump(),5600);
        require(rejected.status().issue==failure.second && rejected.status().state==PM::State::Failed && !rejected.nextRequest(5600),
            "API errors map to fixed categories without printing server text or retrying a mutation");
    }
    NativeAuthProbe uncertainAccount(Mode::ProfilesAndCatalog); PM uncertain(uncertainAccount);
    startManagement(uncertainAccount,uncertain,managementPicker()); openManagedEdit(uncertain);
    uncertain.setName("Changed name"); uncertain.save(5500); writeProfile=pmRequest(uncertain,5500);
    uncertain.transportFailed(writeProfile.id);
    require(uncertain.status().issue==PM::Issue::Uncertain && !uncertain.nextRequest(5600) && !uncertain.save(5600),
        "A lost mutation response reports uncertain outcome and never sends a duplicate write");
    NativeAuthProbe mismatchAccount(Mode::ProfilesAndCatalog); PM mismatch(mismatchAccount);
    startManagement(mismatchAccount,mismatch,managementPicker()); openManagedEdit(mismatch);
    mismatch.setName("Changed name"); mismatch.selectAvatar(1); mismatch.save(5500); writeProfile=pmRequest(mismatch,5500);
    mismatch.complete(writeProfile.id,200,"{}",5600); verify=pmRequest(mismatch,5600);
    mismatch.complete(verify.id,200,managementPicker(Json::array(),false,"Changed name").dump(),5700); verify=pmRequest(mismatch,5700);
    mismatch.complete(verify.id,200,Json{{"token","FAKE_TARGET_TOKEN"}}.dump(),5800); verify=pmRequest(mismatch,5800);
    mismatch.complete(verify.id,200,managementForm("Changed name","avatar_1").dump(),5900);
    require(mismatch.status().issue==PM::Issue::Uncertain && mismatch.status().state==PM::State::Failed,
        "Name-only readback cannot falsely confirm an avatar change");
    for (unsigned kind=0;kind<6;++kind)
    {
        NativeAuthProbe a(Mode::ProfilesAndCatalog); PM unsupported(a); startManagement(a,unsupported,managementPicker());
        unsupported.edit(0,5200); auto r=pmRequest(unsupported,5200);
        unsupported.complete(r.id,200,Json{{"token","FAKE_TARGET_TOKEN"}}.dump(),5300); r=pmRequest(unsupported,5300);
        auto form=managementForm();
        auto& list=form["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
        if (kind==0) list.push_back({{"type","LEGAL_TERMS"}});
        if (kind==1) list.push_back({{"type","BASIC_CHECKBOX"}});
        if (kind==2) list[0]["presentationData"]=Json{{"profileId","FAKE_OTHER_PROFILE"},{"profileName","Player One"},
            {"displayName","FakeTag"},{"avatarId","avatar_1"}}.dump();
        if (kind==3) list[1]["actions"]=Json::array();
        if (kind==4)
            form["pageMemberGroups"]["avatarCarousel"]["widgets"][0]["id"]="item_player_avatar_wrong_identity";
        if (kind==5)
        {
            auto data=Json::parse(list[0]["presentationData"].get<std::string>());
            data["profileType"]="CHILD"; list[0]["presentationData"]=data.dump();
        }
        unsupported.complete(r.id,200,form.dump(),5400);
        require(unsupported.status().state==PM::State::Failed && !unsupported.canSave() && !unsupported.save(5500),
            "Visible consent, profile/avatar identity mismatch, child type or missing server Save action cannot expose a mutation");
    }
    NativeAuthProbe cancelledManagerAccount(Mode::ProfilesAndCatalog); PM cancelledManager(cancelledManagerAccount);
    startManagement(cancelledManagerAccount,cancelledManager,managementPicker()); openManagedEdit(cancelledManager);
    cancelledManager.selectAvatar(9999);
    require(cancelledManager.form().selectedAvatar==0 && !cancelledManager.edit(9999,5500),"Only server-offered profiles and avatar IDs may be selected");
    cancelledManager.setName("Not saved");
    require(cancelledManager.back(5500) && !cancelledManager.save(5500),"Back discards a draft before any account write");
    auto cancelledManagerRead=pmRequest(cancelledManager,5500);
    cancelledManager.back(5500);
    cancelledManager.complete(cancelledManagerRead.id,200,managementPicker().dump(),5600);
    require(!cancelledManager.active() && cancelledManagerAccount.status().step==Step::ProfilesPage,
        "Cancelling a pending picker ignores its late completion and refreshes ordinary profiles");
    for (const auto& response:{std::string("not JSON"),std::string("{\"error\":\"FAKE_PRIVATE_SERVER_TEXT\"}"),
        std::string("{\"errorCode\":22,\"errorCode\":0}"),std::string(MaxLunaResponse+1,'x')})
    {
        NativeAuthProbe a(Mode::ProfilesAndCatalog); PM unknown(a); startManagement(a,unknown,managementPicker()); openManagedEdit(unknown);
        unknown.setName("Changed name"); unknown.save(5500); auto r=pmRequest(unknown,5500);
        unknown.complete(r.id,200,response,5600);
        require(unknown.status().issue==PM::Issue::Uncertain && !unknown.nextRequest(5700),
            "Malformed/duplicate/oversized mutation responses remain uncertain without automatic retries");
    }
    NativeAuthProbe missingCreatedAccount(Mode::ProfilesAndCatalog); PM missingCreated(missingCreatedAccount);
    startManagement(missingCreatedAccount,missingCreated,managementPicker());
    missingCreated.create(5200); createRead=pmRequest(missingCreated,5200);
    missingCreated.complete(createRead.id,200,managementForm().dump(),5300); createRead=pmRequest(missingCreated,5300);
    missingCreated.complete(createRead.id,200,Json{{"displayName","GeneratedTag"},{"nameToken","FAKE_NAME_TOKEN"}}.dump(),5400);
    missingCreated.setName("Player One"); missingCreated.save(5500); writeProfile=pmRequest(missingCreated,5500);
    missingCreated.complete(writeProfile.id,200,"{}",5600); verify=pmRequest(missingCreated,5600);
    missingCreated.complete(verify.id,200,managementPicker().dump(),5700);
    require(missingCreated.status().issue==PM::Issue::Uncertain && !missingCreated.nextRequest(5800),
        "An existing same-name profile cannot falsely verify creation");
    NativeAuthProbe details(Mode::ProfilesAndCatalog); catalogReady(details);
    require(details.games()[0].detailsAvailable && !details.nextRequest(4800) && !details.openGame(99, 4800),
        "Details wait for a valid explicit selection");
    auto detailRequest = openDetail(details);
    require(detailRequest.url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getPage" &&
        routeBody(detailRequest) == Json{{"featureScheme", "WEB_V1"}, {"serviceToken", "FAKE_PAGE_TOKEN"}},
        "Game page uses the server navigation token rather than inventing a route from gameId");
    require(hasHeader(detailRequest, "x-amz-profile-token: FAKE_PROFILE_TOKEN") &&
        hasHeader(detailRequest, "x-amz-access-token: FAKE_NEW_ACCESS") && !details.nextRequest(4901),
        "Detail request remains profile/account bound and single-flight");
    details.complete(detailRequest.id, 200, detailPage().dump(), 5000);
    require(details.status().step == Step::DetailReady && details.detail().title == "Fake Detail Title" &&
        details.detail().description == "A fake game description." && details.detail().buttonText == "Play now" &&
        details.detail().hasArtwork && details.detail().hasTrailer, "Parse source-derived detail fields and multiline button labels");
    for (const auto* type : {"GAME_DESCRIPTION", "GAME_SPECIFICATIONS", "GAME_SPECIFICATIONS_V2"})
        for (const bool beforeProduct : {false, true})
        {
            NativeAuthProbe prose(Mode::ProfilesAndCatalog); catalogReady(prose);
            auto read = openDetail(prose); auto page = detailPage();
            auto& widgets = page["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
            Json description{{"type", type}, {"presentationData", Json{{"gameDescriptionHtml",
                "<p>A <strong>real</strong> synopsis &amp; details.</p><script>UNTRUSTED_SCRIPT</script><p>Explore &#x1F319; &quot;Luna&quot;.</p>"}}.dump()}};
            if (beforeProduct) widgets.insert(widgets.begin(), description); else widgets.push_back(description);
            widgets.push_back(Json{{"type", "TRAILER_TILE"}, {"presentationData", Json{{"videoUrl", "https://m.media-amazon.com/images/S/fixture-trailer.mp4"}}.dump()}});
            widgets.push_back(widgets.back());
            widgets.push_back(Json{{"type", "TRAILER_TILE"}, {"presentationData", Json{{"videoUrl", "https://untrusted.invalid/private.mp4"}}.dump()}});
            prose.complete(read.id, 200, page.dump(), 5000);
            require(prose.status().step == Step::DetailReady && prose.detail().description ==
                "A real synopsis & details. Explore \xf0\x9f\x8c\x99 \"Luna\".",
                "Read separate description widgets in either order, strip markup/scripts, decode entities and preserve UTF-8");
            require(prose.detail().trailers == std::vector<std::string>{"https://m.media-amazon.com/images/S/fixture-trailer.mp4"},
                "Read and deduplicate source-derived trailer tiles; reject unknown hosts without blocking Play");
        }
    {
        NativeAuthProbe prose(Mode::ProfilesAndCatalog); catalogReady(prose);
        auto read = openDetail(prose); auto page = detailPage();
        auto& widgets = page["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
        widgets.push_back(Json{{"type", "GAME_DESCRIPTION"}, {"presentationData", Json{{"gameDescription", "A plain synopsis."}}.dump()}});
        widgets.push_back(Json{{"type", "TRAILER_TILE"}, {"presentationData", "malformed optional JSON"}});
        prose.complete(read.id, 200, page.dump(), 5000);
        require(prose.status().step == Step::DetailReady && prose.detail().description == "A plain synopsis.",
            "Plain descriptions and malformed optional trailer widgets preserve the valid game page");
    }
    {
        auto game = navigableGame();
        auto data = Json::parse(game["presentationData"].get<std::string>());
        data["imageLandscape"] = "https://m.media-amazon.com/images/I/fixture-landscape.jpg";
        data["imagePortrait"] = "https://m.media-amazon.com/images/I/fixture-portrait.jpg";
        game["presentationData"] = data.dump();
        NativeAuthProbe media(Mode::ProfilesAndCatalog); catalogReady(media, Json::array({game}));
        require(media.games()[0].artwork == data["imageLandscape"], "Catalog retains known-source landscape artwork without launch credentials");
        auto read = openDetail(media);
        auto page = detailPage();
        auto& widgets = page["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
        auto product = Json::parse(widgets[0]["presentationData"].get<std::string>());
        product["backgroundImageLandscape"] = "https://m.media-amazon.com/images/I/fixture-hero.jpg";
        widgets[0]["presentationData"] = product.dump();
        for (unsigned i = 0; i < 15; ++i)
            widgets.push_back(Json{{"type", "SCREENSHOT_TILE"}, {"presentationData",
                Json{{"imageUrl", "https://m.media-amazon.com/images/I/fixture-" + std::to_string(i) + ".jpg"}}.dump()}});
        media.complete(read.id, 200, page.dump(), 5000);
        require(media.status().step == Step::DetailReady && media.detail().artwork == product["backgroundImageLandscape"] &&
            media.detail().screenshots.size() == 12 && !media.status().detailParsing.hasPlayToken,
            "Detail retains bounded source-defined screenshots/hero without executing a launch");
        media.returnToCatalog();
        require(media.detail().artwork.empty() && media.detail().screenshots.empty(), "Navigation clears prior game's display media");
    }
    for (const auto& bad : std::vector<Json>{nullptr, 42, Json::object(), "http://m.media-amazon.com/images/I/fixture.jpg",
        "https://m.media-amazon.com.evil.invalid/images/I/fixture.jpg", "https://127.0.0.1/private",
        "https://m.media-amazon.com/images/I/fixture.jpg?access_token=FAKE_ACCESS_SECRET"})
    {
        auto game = navigableGame();
        auto data = Json::parse(game["presentationData"].get<std::string>());
        data["imageLandscape"] = bad; data["imagePortrait"] = "https://m.media-amazon.com/images/I/fallback.jpg";
        game["presentationData"] = data.dump();
        NativeAuthProbe media(Mode::ProfilesAndCatalog); catalogReady(media, Json::array({game}));
        require(media.games()[0].artwork == data["imagePortrait"], "Bad optional artwork falls back without breaking catalog navigation");
        auto read = openDetail(media); auto page = detailPage();
        auto& widgets = page["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
        widgets.push_back(Json{{"type", "SCREENSHOT_TILE"}, {"presentationData", "malformed"}});
        widgets.push_back(Json{{"type", "SCREENSHOT_TILE"}, {"presentationData", Json{{"imageUrl", bad}}.dump()}});
        media.complete(read.id, 200, page.dump(), 5000);
        require(media.status().step == Step::DetailReady && media.detail().screenshots.empty() && media.detail().artwork.empty(),
            "Malformed or untrusted screenshots cannot block a valid detail/launch-action response");
    }
    require(details.status().detailParsing.action == LaunchAction::DirectLaunch && !details.status().detailParsing.hasPlayToken &&
        !details.nextRequest(5001), "DirectLaunch requires another explicit read, without starting a game");
    require(details.inspectLaunch(5100), "Explicit X permits launch-widget inspection");
    auto widgetRequest = request(details, Step::LaunchWidget, 5100);
    require(widgetRequest.url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getWidget" &&
        routeBody(widgetRequest) == Json{{"featureScheme", "WEB_V1"}, {"serviceToken", "FAKE_WIDGET_TOKEN"}} &&
        hasHeader(widgetRequest, "x-amz-profile-token: FAKE_PROFILE_TOKEN"),
        "DirectLaunch uses payload.serviceToken, never the unrelated top-level token");
    details.complete(widgetRequest.id, 200, launchWidget().dump(), 5200);
    require(details.status().step == Step::DetailReady && details.status().detailParsing.action == LaunchAction::GameSession &&
        details.status().detailParsing.hasGameId && details.status().detailParsing.hasPlayToken &&
        !details.inspectLaunch(5201) && !details.nextRequest(5201),
        "Launch credentials are observed privately, with no provisioning request or replay");
    require(details.detail().title.find("TOKEN") == std::string::npos && details.detail().buttonText.find("TOKEN") == std::string::npos,
        "Launch tokens do not enter local display metadata");
    details.returnToCatalog();
    require(details.status().step == Step::CatalogReady && details.games().size() == 1 && details.detail().title.empty() &&
        !details.status().detailParsing.hasPlayToken && !details.inspectLaunch(5300),
        "Back preserves the catalog but drops the previous game's launch state");
    detailRequest = openDetail(details);
    details.returnToCatalog();
    auto secondDetailRequest = openDetail(details);
    details.complete(detailRequest.id, 200, detailPage().dump(), 5000);
    require(details.status().step == Step::DetailPage && details.status().detailParsing.products == 0,
        "A cancelled detail response cannot overwrite a later selection");
    details.complete(secondDetailRequest.id, 200, detailPage(Json::array({launchAction("GAME_SESSION_ASYNC")})).dump(), 5000);
    require(details.status().detailParsing.hasPlayToken && !details.nextRequest(5001),
        "An inline GAME_SESSION_ASYNC payload needs no widget fetch and still does not launch");
    details.returnToCatalog(); details.returnToProfiles();
    require(details.status().step == Step::ChooseProfile && details.games().empty() && details.detail().title.empty(),
        "Profile change drops navigation links and detail metadata");

    auto pathTile = navigableGame(); pathTile["actions"][0].erase("serviceToken");
    NativeAuthProbe pathOnly(Mode::ProfilesAndCatalog); catalogReady(pathOnly, Json::array({pathTile}));
    detailRequest = openDetail(pathOnly);
    require(Json::parse(detailRequest.body)["pageContext"] == Json{{"pageId", "default"}, {"pageUri", "game/fake/FAKE_ASIN"}},
        "Without a token, use the validated server path with browser-style normalization");
    auto noLink = navigableGame(); noLink.erase("actions");
    NativeAuthProbe duplicateLink(Mode::ProfilesAndCatalog);
    catalogReady(duplicateLink, Json::array({noLink, navigableGame()}));
    require(duplicateLink.games().size() == 1 && duplicateLink.games()[0].detailsAvailable,
        "A duplicate tile can supply a missing navigation link without duplicate games");
    for (const auto* target : {"https://evil.invalid/game", "//evil.invalid/game", "/game/../account", "/game/%2e%2e",
        "/game/name?token=FAKE_SECRET", "/game/name#fragment", "/game/\\name", "PURCHASE", "DIRECT_LAUNCH"})
    {
        auto badLink = navigableGame("FAKE_TILE_ID", target);
        // A valid-looking later action must not bypass an unsupported first action.
        badLink["actions"].push_back(navigableGame()["actions"][0]);
        NativeAuthProbe unsafeLink(Mode::ProfilesAndCatalog); catalogReady(unsafeLink, Json::array({badLink}));
        require(!unsafeLink.games()[0].detailsAvailable && !unsafeLink.openGame(0, 4900) && !unsafeLink.nextRequest(4900),
            "Reject external/ambiguous/escaped navigation and never substitute a later action");
    }
    for (const auto* target : {"PURCHASE", "PCON_PIN_CHALLENGE", "PLAYER_SETUP", "UNKNOWN_FUTURE_ACTION"})
    {
        NativeAuthProbe gated(Mode::ProfilesAndCatalog); catalogReady(gated); detailRequest = openDetail(gated);
        gated.complete(detailRequest.id, 200, detailPage(Json::array({launchAction(target), launchAction("GAME_SESSION_ASYNC")})).dump(), 5000);
        require(gated.status().detailParsing.action == LaunchAction::Other && !gated.status().detailParsing.hasPlayToken &&
            !gated.inspectLaunch(5100) && !gated.nextRequest(5100),
            "Purchase, setup, parental and unknown actions never fall through to a later launch action");
        require(gated.detail().primaryActionTarget == target,
            "The exact bounded primary target is available locally even when its action is unsupported");
    }
    for (const auto& fixture : std::vector<std::pair<std::string, std::string>>{
        {"GAME_WITH_SAVE_SELECT", "GAME_WITH_SAVE_SELECT"}, {"DIALOG_OPEN", "DIALOG_OPEN"},
        {"FAKE_PROFILE_TOKEN", "UNKNOWN_ACTION"}, {"FAKE_NEW_ACCESS", "UNKNOWN_ACTION"},
        {"FUTURE_FAKE_PROFILE_TOKEN", "UNKNOWN_ACTION"}, {"FAKE_PAGE_TOKEN", "UNKNOWN_ACTION"},
        {"BAD_TARGET\x1b[2J", "UNKNOWN_ACTION"}, {"BAD_TARGET\n", "UNKNOWN_ACTION"},
        {"https://example.invalid/FAKE_SECRET", "UNKNOWN_ACTION"}, {std::string(81, 'A'), "UNKNOWN_ACTION"}})
    {
        auto unknownButtonPage = detailPage(Json::array({Json{{"target", fixture.first}, {"payload", "FAKE_SECRET_PAYLOAD"}}}));
        NativeAuthProbe unknownButton(Mode::ProfilesAndCatalog); catalogReady(unknownButton); detailRequest = openDetail(unknownButton);
        unknownButton.complete(detailRequest.id, 200, unknownButtonPage.dump(), 5000);
        require(unknownButton.detail().buttonText == "Play now" && unknownButton.detail().primaryActionTarget == fixture.second &&
            unknownButton.status().detailParsing.primaryAction == LaunchAction::Other && !unknownButton.inspectLaunch(5100) &&
            !unknownButton.nextRequest(5100),
            "Play label does not authorize guessing a launch route; unsafe/sensitive target metadata is redacted locally");
        unknownButton.returnToCatalog();
        require(unknownButton.detail().primaryActionTarget.empty(), "Back drops the previous button's diagnostic identifier");
    }
    auto pageGate = detailPage(); pageGate["actions"] = Json::array({launchAction("PCON_PIN_CHALLENGE")});
    // Wire-shaped reproduction of the three targets Josh observed on hardware.
    // Every value is synthetic; no captured tokens or account IDs are used.
    auto observedActions = detailPage();
    observedActions["actions"] = Json::array({
        Json{{"target", "DIALOG_SURVEY_GAME"}, {"payload", Json{{"pageType", "multistate_post_game_dialog"}}.dump()}},
        Json{{"target", "DIALOG_CONTROLLER_UNRECOGNIZED"}, {"serviceToken", "FAKE_CONTROLLER_DIALOG_TOKEN"}},
        Json{{"target", "SOFTWARE_DECODE"}, {"serviceToken", "FAKE_SOFTWARE_DIALOG_TOKEN"}}});
    NativeAuthProbe observedPage(Mode::ProfilesAndCatalog); catalogReady(observedPage); detailRequest = openDetail(observedPage);
    observedPage.complete(detailRequest.id, 200, observedActions.dump(), 5000);
    require(observedPage.status().detailParsing.pageActions == 3 && observedPage.status().detailParsing.blockingPageActions == 0 &&
        observedPage.status().detailParsing.action == LaunchAction::DirectLaunch && observedPage.inspectLaunch(5100),
        "Observed post-game survey and ignored service actions do not block the Play action read");
    widgetRequest = request(observedPage, Step::LaunchWidget, 5100);
    require(Json::parse(widgetRequest.body)["serviceToken"] == "FAKE_WIDGET_TOKEN" &&
        widgetRequest.body.find("FAKE_CONTROLLER_DIALOG_TOKEN") == std::string::npos &&
        widgetRequest.body.find("FAKE_SOFTWARE_DIALOG_TOKEN") == std::string::npos,
        "Reading Play never executes or fetches the omitted page actions' service tokens");
    observedPage.complete(widgetRequest.id, 200, launchWidget().dump(), 5200);
    require(observedPage.status().detailParsing.hasPlayToken && !observedPage.nextRequest(5201),
        "Observed page-action fixture reaches payload inspection without provisioning");
    auto unsupportedOsPage = observedActions;
    unsupportedOsPage["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][1]["actions"] =
        Json::array({Json{{"target", "OS_UNSUPPORTED"}, {"serviceToken", "FAKE_UNSUPPORTED_OS_DIALOG_TOKEN"}},
            launchAction("GAME_SESSION_ASYNC")});
    NativeAuthProbe unsupportedOs(Mode::ProfilesAndCatalog); catalogReady(unsupportedOs); detailRequest = openDetail(unsupportedOs);
    unsupportedOs.complete(detailRequest.id, 200, unsupportedOsPage.dump(), 5000);
    require(unsupportedOs.detail().buttonText == "Play now" && unsupportedOs.detail().primaryActionTarget == "OS_UNSUPPORTED" &&
        unsupportedOs.status().detailParsing.action == LaunchAction::UnsupportedOs &&
        unsupportedOs.status().detailParsing.primaryAction == LaunchAction::UnsupportedOs &&
        unsupportedOs.status().detailParsing.blockingPageActions == 0 && !unsupportedOs.status().detailParsing.hasPlayToken &&
        !unsupportedOs.inspectLaunch(5100) && !unsupportedOs.nextRequest(5100),
        "Browser compatibility is not a bypass: an OS_UNSUPPORTED reply remains blocked without fetching its token or a later action");
    require(std::string(launchActionLabel(LaunchAction::UnsupportedOs)) == "os_unsupported",
        "An OS rejection has a fixed diagnostic label instead of remaining an unidentified action");
    for (const auto& pageAction : {Json{{"target", "GET_ENTITLEMENT_STATUSES_FOR_EXTERNAL_PRODUCTS"},
            {"payload", Json{{"shouldExecuteActionOnLoad", true}}.dump()}},
        Json{{"target", "GET_ENTITLEMENT_STATUSES_FOR_EXTERNAL_PRODUCTS"}, {"payload", Json{{"shouldExecuteActionOnLoad", false}}.dump()}},
        Json{{"target", "GET_ENTITLEMENT_STATUSES_FOR_EXTERNAL_PRODUCTS"}, {"payload", "not JSON FAKE_SECRET"}},
        Json{{"target", "PROFILE_PIN_ENTRY_OPEN"}, {"payload", Json{{"shouldExecuteActionOnLoad", false}}.dump()}},
        Json{{"target", "UNKNOWN_FUTURE_ACTION"}}, Json{{"target", "FAKE_PROFILE_TOKEN"}},
        Json{{"target", "FAKE_SECRET\x1b[2J"}}})
    {
        auto blockedPage = detailPage(); blockedPage["actions"] = Json::array({pageAction});
        NativeAuthProbe pendingAction(Mode::ProfilesAndCatalog); catalogReady(pendingAction); detailRequest = openDetail(pendingAction);
        pendingAction.complete(detailRequest.id, 200, blockedPage.dump(), 5000);
        require(pendingAction.status().detailParsing.action == LaunchAction::PageAction &&
            pendingAction.status().detailParsing.pageActions == 1 && pendingAction.status().detailParsing.blockingPageActions == 1 &&
            pendingAction.status().detailParsing.primaryAction == LaunchAction::DirectLaunch &&
            !pendingAction.inspectLaunch(5100) && !pendingAction.nextRequest(5100),
            "Active entitlement workflows, PIN and unknown/malformed page actions remain blocked even with a valid Play button");
        if (pageAction["target"] == "FAKE_PROFILE_TOKEN" || pageAction["target"] == "FAKE_SECRET\x1b[2J")
            require(pendingAction.detail().pageActionTargets == std::vector<std::string>{"UNKNOWN_ACTION"},
                "Local action metadata rejects actual credentials and console escapes");
    }
    auto mixedPage = detailPage();
    mixedPage["actions"] = Json::array({Json{{"target", "DIALOG_SURVEY_GAME"}}, Json{{"target", "PROFILE_PIN_ENTRY_OPEN"}}});
    NativeAuthProbe mixedActions(Mode::ProfilesAndCatalog); catalogReady(mixedActions); detailRequest = openDetail(mixedActions);
    mixedActions.complete(detailRequest.id, 200, mixedPage.dump(), 5000);
    require(mixedActions.status().detailParsing.pageActions == 2 && mixedActions.status().detailParsing.blockingPageActions == 1 &&
        mixedActions.status().detailParsing.action == LaunchAction::PageAction && !mixedActions.inspectLaunch(5100),
        "A harmless action cannot hide another unresolved page action");
    auto ambiguousDetail = detailPage();
    ambiguousDetail["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"].push_back(
        detailPage()["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][1]);
    for (const auto& fixture : {pageGate, ambiguousDetail})
    {
        NativeAuthProbe gated(Mode::ProfilesAndCatalog); catalogReady(gated); detailRequest = openDetail(gated);
        gated.complete(detailRequest.id, 200, fixture.dump(), 5000);
        require((gated.status().detailParsing.action == LaunchAction::PageAction || gated.status().detailParsing.action == LaunchAction::Ambiguous) &&
            !gated.inspectLaunch(5100) && !gated.status().detailParsing.hasPlayToken && !gated.nextRequest(5100),
            "Page-level gates and multiple primary buttons remain explicit unsupported cases");
    }
    auto badPresentation = detailPage();
    badPresentation["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["presentationData"] = "[]";
    for (const auto& bad : {std::string("not JSON FAKE_SECRET"), Page.dump(), badPresentation.dump(), std::string(MaxLunaResponse + 1, 'x'),
        detailPage(Json::array({Json{{"target", "GAME_SESSION_ASYNC"}, {"payload", "{\"gameId\":\"FAKE_ID\"}"}}})).dump()})
    {
        NativeAuthProbe invalidDetail(Mode::ProfilesAndCatalog); catalogReady(invalidDetail); detailRequest = openDetail(invalidDetail);
        invalidDetail.complete(detailRequest.id, 200, bad, 5000);
        require(invalidDetail.status().step == Step::CatalogReady && invalidDetail.status().detailParsing.error == Error::Response &&
            !invalidDetail.status().detailParsing.hasPlayToken && invalidDetail.games().size() == 1 && !invalidDetail.nextRequest(5100),
            "Malformed/oversized/missing detail data returns safely to catalog without losing login or retrying");
    }
    for (const auto& bad : {launchWidget(launchAction("PCON_PIN_CHALLENGE")), Json{{"errorCode", 28}},
        launchWidget(Json{{"target", "GAME_SESSION_ASYNC"}, {"payload", "{\"gameId\":\"FAKE_ID\",\"gameId\":\"FAKE_OTHER\",\"playToken\":\"FAKE_SECRET\"}"}})})
    {
        NativeAuthProbe invalidWidget(Mode::ProfilesAndCatalog); catalogReady(invalidWidget); detailRequest = openDetail(invalidWidget);
        invalidWidget.complete(detailRequest.id, 200, detailPage().dump(), 5000);
        invalidWidget.inspectLaunch(5100); widgetRequest = request(invalidWidget, Step::LaunchWidget, 5100);
        invalidWidget.complete(widgetRequest.id, 200, bad.dump(), 5200);
        require(invalidWidget.status().step == Step::DetailReady && invalidWidget.status().detailParsing.error == Error::Response &&
            !invalidWidget.status().detailParsing.hasPlayToken && !invalidWidget.inspectLaunch(5300) && !invalidWidget.nextRequest(5300),
            "Unexpected or malformed widget replies cannot claim a play token, repeat requests or bypass gates");
    }
    NativeAuthProbe abortedWidget(Mode::ProfilesAndCatalog); catalogReady(abortedWidget); detailRequest = openDetail(abortedWidget);
    abortedWidget.complete(detailRequest.id, 200, detailPage().dump(), 5000);
    abortedWidget.inspectLaunch(5100); widgetRequest = request(abortedWidget, Step::LaunchWidget, 5100);
    abortedWidget.returnToCatalog(); abortedWidget.complete(widgetRequest.id, 200, launchWidget().dump(), 5200);
    require(abortedWidget.status().step == Step::CatalogReady && !abortedWidget.status().detailParsing.hasPlayToken,
        "Back invalidates a pending launch-widget response");
    NativeAuthProbe transportDetail(Mode::ProfilesAndCatalog); catalogReady(transportDetail); detailRequest = openDetail(transportDetail);
    transportDetail.transportFailed(detailRequest.id);
    require(transportDetail.status().step == Step::CatalogReady && transportDetail.status().detailParsing.error == Error::Transport &&
        !transportDetail.nextRequest(5000), "Detail transport failures preserve login and do not retry automatically");
    for (const long http : {404L, 401L, 403L})
    {
        NativeAuthProbe rejectedDetail(Mode::ProfilesAndCatalog); catalogReady(rejectedDetail); detailRequest = openDetail(rejectedDetail);
        rejectedDetail.complete(detailRequest.id, http, "FAKE_SERVER_SECRET", 5000);
        require(http == 404 ? rejectedDetail.status().step == Step::CatalogReady && rejectedDetail.status().detailParsing.error == Error::Http :
            rejectedDetail.status().step == Step::Failed && rejectedDetail.games().empty(),
            "Ordinary detail HTTP errors preserve catalog; authorization rejection clears the session");
    }
    NativeAuthProbe expiredDetail(Mode::ProfilesAndCatalog); catalogReady(expiredDetail);
    require(!expiredDetail.openGame(0, 904100) && expiredDetail.status().error == Error::Expired && expiredDetail.games().empty(),
        "Opening details cannot extend the access-token lifetime");
    require(responseLimit(Step::DetailPage) == MaxLunaResponse && responseLimit(Step::LaunchWidget) == MaxLunaResponse,
        "Detail/widget HTTP buffers and parsers share bounded page limits");
    NativeAuthProbe experimental(Mode::ProfilesAndCatalog); auto noticeRequest = openNotice(experimental);
    experimental.complete(noticeRequest.id, 200, noticePage().dump(), 5200);
    require(experimental.status().step == Step::ExperimentalReady && experimental.status().noticeParsing.complete &&
        experimental.notice().text.find("fake warning") != std::string::npos && experimental.notice().choices.size() == 2 &&
        experimental.notice().choices[0].label == "Continue" && experimental.notice().choices[1].label == "Cancel" &&
        !experimental.nextRequest(5201) && !experimental.status().detailParsing.hasPlayToken,
        "Complete notice and server choices are exposed locally; no automatic acceptance or launch payload");
    require(!experimental.chooseNoticeButton(99, 5201) && !experimental.chooseNoticeButton(1, 5201) &&
        !experimental.status().noticeParsing.accepted, "Invalid and dismiss/unknown choices cannot accept or inspect a launch action");
    require(experimental.chooseNoticeButton(0, 5300) && experimental.status().step == Step::DetailReady &&
        experimental.status().noticeParsing.accepted && experimental.status().detailParsing.hasPlayToken &&
        experimental.status().detailParsing.action == LaunchAction::GameSession && !experimental.nextRequest(5301),
        "Manual continuation can inspect inline launch payload, without provisioning or autoplay");
    experimental.returnToCatalog();
    require(experimental.notice().text.empty() && experimental.notice().choices.empty() && !experimental.status().noticeParsing.accepted,
        "Leaving the selected game clears experimental notice/acceptance state");

    NativeAuthProbe automaticClient(Mode::ProfilesAndCatalog); noticeRequest = openNotice(automaticClient);
    automaticClient.complete(noticeRequest.id, 200, noticePage().dump(), 5200);
    require(automaticClient.acknowledgeUnsupportedClient(5300) && automaticClient.status().noticeParsing.accepted &&
        automaticClient.status().detailParsing.hasPlayToken && !automaticClient.nextRequest(5301) &&
        !automaticClient.acknowledgeUnsupportedClient(5301),
        "The explicit launcher policy can acknowledge the known client warning once without allocating a game itself");
    NativeAuthProbe automaticDirect(Mode::ProfilesAndCatalog); noticeRequest = openNotice(automaticDirect);
    automaticDirect.complete(noticeRequest.id, 200, noticePage(launchAction()).dump(), 5200);
    require(automaticDirect.acknowledgeUnsupportedClient(5300) && automaticDirect.status().step == Step::LaunchWidget,
        "Known client-warning continuation may fetch its direct-launch widget through the existing validation");
    auto ambiguousClient = noticePage();
    ambiguousClient["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][2]["actions"] =
        Json::array({launchAction("GAME_SESSION_ASYNC")});
    NativeAuthProbe ambiguousWarning(Mode::ProfilesAndCatalog); noticeRequest = openNotice(ambiguousWarning);
    ambiguousWarning.complete(noticeRequest.id, 200, ambiguousClient.dump(), 5200);
    require(!ambiguousWarning.acknowledgeUnsupportedClient(5300) && !ambiguousWarning.status().noticeParsing.accepted,
        "Multiple launch continuations do not qualify for automatic acknowledgment");

    NativeAuthProbe noticeDirect(Mode::ProfilesAndCatalog); noticeRequest = openNotice(noticeDirect);
    noticeDirect.complete(noticeRequest.id, 200, noticePage(launchAction()).dump(), 5200);
    require(noticeDirect.chooseNoticeButton(0, 5300), "Manual notice continuation permits a direct-launch read");
    widgetRequest = request(noticeDirect, Step::LaunchWidget, 5300);
    require(Json::parse(widgetRequest.body)["serviceToken"] == "FAKE_WIDGET_TOKEN",
        "Continuation uses its own direct-launch widget token rather than the notice token");
    noticeDirect.complete(widgetRequest.id, 200, launchWidget().dump(), 5400);
    require(noticeDirect.status().noticeParsing.accepted && noticeDirect.status().detailParsing.hasPlayToken && !noticeDirect.nextRequest(5401),
        "Explicit notice selection plus widget read can finish inspection without a session-creation call");

    NativeAuthProbe noticeBack(Mode::ProfilesAndCatalog); noticeRequest = openNotice(noticeBack);
    noticeBack.returnToDetail(); noticeBack.complete(noticeRequest.id, 200, noticePage().dump(), 5200);
    require(noticeBack.status().step == Step::DetailReady && noticeBack.notice().text.empty() && !noticeBack.status().noticeParsing.accepted,
        "Back cancels a pending modal and ignores its late reply");
    require(noticeBack.inspectLaunch(5300), "Back preserves the original notice token for an explicit reopen");
    noticeRequest = request(noticeBack, Step::ExperimentalPage, 5300);
    noticeBack.complete(noticeRequest.id, 200, noticePage().dump(), 5400);
    noticeBack.returnToDetail();
    require(!noticeBack.chooseNoticeButton(0, 5500) && !noticeBack.nextRequest(5500),
        "Returning from a loaded notice drops its choices; A cannot act on stale actions");

    auto unknownNotice = noticePage();
    unknownNotice["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"].push_back(
        Json{{"type", "UNKNOWN_NOTICE_WIDGET"}, {"presentationData", "{}"}});
    auto pinNotice = noticePage(); pinNotice["actions"] = Json::array({Json{{"target", "PROFILE_PIN_ENTRY_OPEN"}}});
    auto checkboxNotice = noticePage();
    checkboxNotice["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0] =
        Json{{"type", "BASIC_DIALOG"}, {"presentationData", Json{{"title", "Read this"},
            {"description", "Fake terms"}, {"checkboxText", "An unimplemented required checkbox"}}.dump()}};
    auto noTextNotice = noticePage(); noTextNotice["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"].erase(0);
    auto legalNotice = checkboxNotice;
    legalNotice["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0]["presentationData"] =
        Json{{"title", "Experimental client"}, {"description", "Fake legal requirement"}, {"legalTerms", Json::array({"FAKE_TERMS"})}}.dump();
    auto secondaryNotice = noticePage();
    secondaryNotice["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][1]["actions"].push_back(
        Json{{"target", "UNKNOWN_REQUIRED_ACTION"}});
    for (const auto& fixture : {unknownNotice, pinNotice, checkboxNotice, noTextNotice, legalNotice, secondaryNotice})
    {
        NativeAuthProbe incomplete(Mode::ProfilesAndCatalog); noticeRequest = openNotice(incomplete);
        incomplete.complete(noticeRequest.id, 200, fixture.dump(), 5200);
        require(incomplete.status().step == Step::ExperimentalReady && !incomplete.status().noticeParsing.complete &&
            !incomplete.chooseNoticeButton(0, 5300) && !incomplete.status().noticeParsing.accepted && !incomplete.nextRequest(5300),
            "Unknown content, PIN actions, checkbox forms and absent warning text cannot be silently skipped");
        require(!incomplete.acknowledgeUnsupportedClient(5300),
            "Automatic acknowledgment never skips unknown widgets, PIN actions, checkboxes, legal terms or required secondary actions");
    }
    for (const auto* target : {"OS_UNSUPPORTED", "PROFILE_PIN_ENTRY_OPEN", "PURCHASE", "EXPERIMENTAL_CLIENT", "UNKNOWN_ACTION"})
    {
        auto blockedChoice = noticePage(Json{{"target", target}, {"serviceToken", "FAKE_DO_NOT_FETCH"}});
        blockedChoice["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][1]["actions"].push_back(launchAction("GAME_SESSION_ASYNC"));
        NativeAuthProbe gatedNotice(Mode::ProfilesAndCatalog); noticeRequest = openNotice(gatedNotice);
        gatedNotice.complete(noticeRequest.id, 200, blockedChoice.dump(), 5200);
        require(!gatedNotice.chooseNoticeButton(0, 5300) && !gatedNotice.status().detailParsing.hasPlayToken && !gatedNotice.nextRequest(5300),
            "Notice button's unsupported first action never falls through to a later game launch action");
    }
    for (const auto& fixture : {std::string("FAKE_INVALID_JSON"), Json{{"errorCode", 28}}.dump(),
        Json{{"pageMemberGroups", {{"otherContent", {{"widgets", Json::array()}}}}}}.dump(),
        std::string(MaxLunaResponse + 1, 'x')})
    {
        NativeAuthProbe badNotice(Mode::ProfilesAndCatalog); noticeRequest = openNotice(badNotice);
        badNotice.complete(noticeRequest.id, 200, fixture, 5200);
        require(badNotice.status().step == Step::DetailReady && badNotice.status().detailParsing.error == Error::Response &&
            !badNotice.chooseNoticeButton(0, 5300) && !badNotice.nextRequest(5300),
            "Malformed, rejected or oversized modal responses preserve detail login state without accepting or retrying");
    }
    NativeAuthProbe noticeTransport(Mode::ProfilesAndCatalog); noticeRequest = openNotice(noticeTransport);
    noticeTransport.transportFailed(noticeRequest.id);
    require(noticeTransport.status().step == Step::DetailReady && noticeTransport.status().detailParsing.error == Error::Transport &&
        !noticeTransport.nextRequest(5200), "Notice transport failure cannot auto-retry or accept");
    NativeAuthProbe noticeExpired(Mode::ProfilesAndCatalog); noticeRequest = openNotice(noticeExpired);
    noticeExpired.complete(noticeRequest.id, 200, noticePage().dump(), 5200);
    require(!noticeExpired.chooseNoticeButton(0, 904100) && noticeExpired.status().error == Error::Expired &&
        noticeExpired.notice().choices.empty(), "Notice acceptance cannot extend the original token lifetime");
    require(responseLimit(Step::ExperimentalPage) == MaxLunaResponse, "Modal transport and parser share bounded page-response size");

    // Normal-solo native provisioning uses synthetic launch and auth data only.
    const auto readyNative = [&](NativeAuthProbe& auth, Json extra = Json::object())
    {
        catalogReady(auth);
        auto read = openDetail(auth);
        auto action = launchAction("GAME_SESSION_ASYNC");
        auto payload = Json::parse(action["payload"].get<std::string>());
        payload.update(extra); action["payload"] = payload.dump();
        auth.complete(read.id, 200, detailPage(Json::array({action})).dump(), 5000);
    };
    const std::string fakeOffer = "v=0\r\nm=video 1 UDP/TLS/RTP/SAVPF 108\r\n"
        "m=audio 1 UDP/TLS/RTP/SAVPF 111\r\nm=application 1 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "a=fingerprint:sha-256 FAKE\r\n";
    const auto allow = [&](NativeSession& session)
    {
        require(session.start(5100), "Explicit native solo start");
        auto pin = session.nextRequest(5100);
        require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos,
            "Native launch asks Luna for parental authorization first");
        require(Json::parse(pin->body) == Json{{"actions", {"LAUNCH_GAME_MATURE"}}},
            "Unknown ESRB rating uses the browser's conservative mapping");
        require(hasHeader(*pin, "x-amz-access-token: FAKE_NEW_ACCESS") &&
            hasHeader(*pin, "x-amz-profile-token: FAKE_PROFILE_TOKEN"), "Native operations bind refreshed account and selected profile");
        require(!session.supplyOffer(fakeOffer, 5101) && !session.nextRequest(5101), "Cannot race parental check or overlap requests");
        session.complete(pin->id + 1, 200, "{}", 5101);
        require(session.status().state == NativeSession::State::CheckingPin, "Ignore stale native completions");
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5200);
        require(session.status().state == NativeSession::State::AwaitOffer, "Only ALLOW advances to fresh offer");
        require(session.supplyOffer(fakeOffer, 5300), "Fresh peer offer starts one provisioning owner");
    };
    const auto createNative = [&](NativeSession& session)
    {
        allow(session);
        auto create = session.nextRequest(5300);
        require(create && create->url.find("/createGameSessionJob") != std::string::npos, "One native create request");
        const auto body = Json::parse(create->body);
        require(body["gameId"] == "FAKE_PROVISIONING_ID" && body["playToken"] == "FAKE_PLAY_TOKEN", "Use selected native launch credentials");
        const auto offer = Json::parse(body["signalingRequest"].get<std::string>());
        require(body["clientFeatures"] == Json{{"dataUsage", "LIMITED"}, {"routing", {"TURN"}}},
            "Default 720p request includes the browser's LIMITED data-usage hint and TURN routing");
        require(offer["sdp"] == fakeOffer && offer["extras"]["clientContext"]["osName"] == "Horizon",
            "Extended signaling retains this Switch offer and honest streaming metadata");
        require(!body.contains("transferRequest") && !body.contains("revokeProfileActivityRequest") &&
            !body.contains("pconToken") && !body.contains("regionLatencies") && !body.contains("buildId") &&
            !body.contains("gameBuildConfigurationId"), "No invented takeover, PIN proof, build selection or latency measurements");
        require(!session.supplyOffer(fakeOffer, 5400) && !session.start(5400) && !session.nextRequest(5400),
            "Repeated input cannot start a second create or replace its original offer");
        return *create;
    };
    const auto created = [](const char* id = "FAKE_JOB") { return Json{{"gameSessionJobId", id}, {"startPollingAfterMs", 5000}}.dump(); };
    const auto queued = [] { return Json{{"gameSessionJobId", "FAKE_JOB"}, {"status", "ACTIVE_QUEUED"},
        {"positionInLine", 96}, {"expectedWaitTimeSeconds", 240}, {"retryAfterMs", 5000}}.dump(); };
    const auto allocatedNative = [&](NativeSession& session)
    {
        require(!session.canEnd() && !session.endGame(5000), "No termination before this client owns a session");
        const auto create = createNative(session);
        session.complete(create.id, 200, created(), 5500);
        auto poll = session.nextRequest(10500);
        const Json game = {{"sessionId", "FAKE_OWNED_SESSION"}, {"connectionId", "FAKE_CONNECTION"},
            {"signalingResponse", Json{{"type", "answer"}, {"sdp", "v=0\r\n"}}.dump()}};
        session.complete(poll->id, 200, Json{{"gameSessionJobId", "FAKE_JOB"},
            {"status", "COMPLETED_SUCCEEDED"}, {"gameSession", game}}.dump(), 10600);
        require(session.takeAnswer().has_value() && session.canEnd(), "Only the authenticated allocated session can be ended");
        session.cancel(10700);
        require(session.canEnd() && !session.nextRequest(10700) && session.status().remoteWorkMayRemain,
            "Local disconnect/cancel never dispatches remote termination");
    };
    for (const auto& response : std::vector<std::pair<long, std::string>>{
        {200, "{}"}, {200, R"({"failedIds":[]})"}, {200, R"({"failedIds":null})"},
        {200, R"({"failedIds":["FAKE_OWNED_SESSION"]})"}, {200, R"({"failedIds":false})"},
        {200, R"({"errorCode":9})"}, {200, R"({"error":"FAKE_PRIVATE_ERROR"})"},
        {200, R"({"__type":"FAKE_ERROR"})"}, {401, "{}"}, {500, "{}"},
        {200, "[]"}, {200, ""}, {200, R"({"failedIds":[],"failedIds":["FAKE_SESSION"]})"}})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); allocatedNative(session);
        require(session.endGame(11000) && !session.endGame(11000), "One explicit End Game operation, never duplicate on repeated input");
        const auto end = session.nextRequest(11000);
        require(end && end->url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/terminateGameSession" &&
            Json::parse(end->body) == Json{{"timeout",10000},{"sessionIds",{"FAKE_OWNED_SESSION"}},
                {"terminationReason","OVERLAY_EXIT"}}, "End uses the observed fixed route and exactly this allocated session ID");
        require(hasHeader(*end,"x-amz-profile-token: FAKE_PROFILE_TOKEN") &&
            !session.nextRequest(11001), "Termination remains profile bound and cannot overlap requests");
        session.complete(end->id + 1, 200, "{}", 11050);
        require(session.status().state == NativeSession::State::Ending, "Unknown termination completions cannot claim success");
        session.complete(end->id, response.first, response.second, 11100);
        const bool success = response.first == 200 && (response.second == "{}" ||
            response.second == R"({"failedIds":[]})" || response.second == R"({"failedIds":null})");
        require(session.status().state == (success ? NativeSession::State::Ended : NativeSession::State::EndFailed) &&
            session.status().remoteWorkMayRemain == !success && !session.nextRequest(11200),
            "Only a valid termination acknowledgment clears remote work; failures never replay or auto-launch a replacement");
        require(session.canEnd() == !success, "An ambiguous failure permits only an explicit user retry; success consumes the target");
    }
    for (const unsigned failure : {0u, 1u, 2u, 3u})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); allocatedNative(session);
        require(session.endGame(11000), "Explicit end begins independently of the provisioning deadline");
        const auto end = session.nextRequest(11000);
        if (failure == 0) session.transportFailed(end->id, 11100);
        else if (failure == 1) session.cancel(11100);
        else if (failure == 2) session.nextRequest(31000);
        else session.complete(end->id,200,"{}",31000);
        require(session.status().state == NativeSession::State::EndFailed && session.status().remoteWorkMayRemain,
            "Lost, cancelled and expired termination requests never imply the remote game ended");
        session.complete(end->id,200,"{}",32000);
        require(session.status().state == NativeSession::State::EndFailed, "Late termination acknowledgment cannot start a replacement");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); allocatedNative(session);
        auth.returnToCatalog(); auth.returnToProfiles();
        require(!session.canEnd() && !session.endGame(11000) && !session.nextRequest(11000),
            "Signing out of the launching profile disables its retained session controls");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); allocatedNative(session);
        require(session.endGame(3'600'000), "A long game can be ended after the original launch deadline");
        const auto refresh = session.nextRequest(3'600'000);
        require(refresh && refresh->url == "https://api.amazon.com/auth/token",
            "Ending a long session refreshes expired account credentials before termination");
        session.complete(refresh->id,200,Json{{"access_token","FAKE_END_REFRESHED"},{"expires_in",3600}}.dump(),3'600'100);
        const auto end = session.nextRequest(3'600'100);
        require(end && end->url.find("/terminateGameSession") != std::string::npos &&
            hasHeader(*end,"x-amz-access-token: FAKE_END_REFRESHED"), "Termination uses the freshly refreshed account token");
        session.complete(end->id,200,"{}",3'600'200);
        require(session.status().state == NativeSession::State::Ended, "Refresh and termination complete as one explicit operation");
    }
    for (const bool beforeDispatch : {false,true})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); allocatedNative(session); session.endGame(11000);
        const auto end = beforeDispatch ? std::optional<Request>{} : session.nextRequest(11000);
        auth.returnToCatalog(); auth.returnToProfiles();
        if (beforeDispatch) require(!session.nextRequest(11100), "Profile changes prevent dispatching another profile's termination");
        else session.complete(end->id,200,"{}",11100);
        require(session.status().state == NativeSession::State::EndFailed && session.status().remoteWorkMayRemain,
            "Profile changes invalidate both pending and undispatched termination operations");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); const auto create = createNative(session);
        session.complete(create.id, 200, created(), 5500);
        require(!session.nextRequest(10499), "Respect initial polling delay");
        auto poll = session.nextRequest(10500); require(poll.has_value(), "Poll after server delay");
        require(Json::parse(poll->body) == Json{{"gameSessionJobId", "FAKE_JOB"}}, "Poll the original native job only");
        session.complete(poll->id, 200, queued(), 10600);
        require(session.status().position == 96 && session.status().waitSeconds == 240 && !session.nextRequest(15599),
            "Queue position, ETA, and retry delay reach native UI");
        poll = session.nextRequest(15600);
        const std::string sdp = "v=0\r\nc=IN IP4 ec2-3-132-215-235.compute-1.amazonaws.com\r\n"
            "a=candidate:1 1 UDP 123 ec2-3-132-215-235.compute-1.amazonaws.com 33438 typ host\r\n";
        const Json game = {{"sessionId", "FAKE_SESSION"}, {"connectionId", "FAKE_CONNECTION"}, {"publicIp", "3.132.215.235"},
            {"protocol", "WEB_RTC"}, {"transport", "UDP"}, {"signalingResponse", Json{{"type", "answer"}, {"sdp", sdp}}.dump()}};
        session.complete(poll->id, 200, Json{{"gameSessionJobId", "FAKE_JOB"}, {"status", "COMPLETED_SUCCEEDED"}, {"gameSession", game}}.dump(), 15700);
        require(session.status().state == NativeSession::State::Ready && !session.status().position, "Success clears queue status");
        const auto answer = session.takeAnswer();
        require(answer && answer->find("ec2-") == std::string::npos && answer->find("3.132.215.235") != std::string::npos,
            "Authenticated matching EC2 candidate becomes numeric without reverse DNS");
        require(!session.takeAnswer() && !session.nextRequest(15800), "Exactly one answer delivery, no further provisioning");
    }
    for (const auto& requirement : std::vector<std::pair<const char*, NativeSession::Block>>{
        {"dialogTermsAndConditionsToken", NativeSession::Block::Terms}, {"dialogAgeRatingConfirmationToken", NativeSession::Block::AgeRating},
        {"dialogMouseKeyboardRequiredToken", NativeSession::Block::MouseKeyboard}, {"transferRequest", NativeSession::Block::UnsupportedLaunch},
        {"partyId", NativeSession::Block::UnsupportedLaunch}, {"saveSlotId", NativeSession::Block::UnsupportedLaunch}})
    {
        for (const bool hardware : {false, true})
        {
            NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth, Json{{requirement.first, "FAKE_REQUIREMENT"}});
            NativeSession session(auth); require(session.start(5100, hardware), "Preflight attempt starts explicitly");
            require(session.status().state == NativeSession::State::Blocked && session.status().block == requirement.second &&
                !session.nextRequest(5200) && !session.supplyOffer(fakeOffer, 5300), "No allocation can skip required consent, PIN, transfer or save-slot handling");
        }
    }

    const auto eulaPage = []
    {
        auto page = Page;
        const Json dialog = {{"type", "BASIC_DIALOG"}, {"actions", Json::array()},
            {"presentationData", Json{{"primaryText", "Terms of Service"}, {"secondaryText",
                "By clicking Accept you agree to <a href=\"http://example.invalid/terms\">Terms of Use</a>."},
                {"legalTerms", "<p>Entire synthetic agreement, including its final clause.</p>"}}.dump()}};
        const Json cancel = {{"type", "DIALOG_BUTTON"}, {"presentationData", Json{{"text", "Cancel"}}.dump()},
            {"actions", Json::array({Json{{"target", "DIALOG_DISMISS"}}})}};
        const Json accept = {{"type", "DIALOG_PRIMARY_BUTTON"}, {"presentationData", Json{{"text", "Accept"}}.dump()},
            {"actions", Json::array({Json{{"target", "ACCEPT_EULA"},
                {"payload", Json{{"productId", "FAKE_PROVISIONING_ID"}}.dump()}}})}};
        page["pageMemberGroups"]["mainContent"]["widgets"] = Json::array({dialog,
            Json{{"type", "DIALOG_BUTTON_CAROUSEL"}, {"widgets", Json::array({cancel, accept})}}});
        return page;
    };
    const auto openEula = [&](NativeAuthProbe& auth, NativeSession& session, Json page, bool software = false)
    {
        Json extra = {{"dialogAcceptEulaToken", "FAKE_EULA_TOKEN"}};
        if (software) extra["dialogSoftwareDecodeToken"] = "FAKE_SOFTWARE_TOKEN";
        readyNative(auth, extra);
        require(session.start(5100, !software), "Explicit launch can retrieve required game EULA");
        auto pin = session.nextRequest(5100);
        require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos,
            "EULA retrieval cannot bypass parental authorization");
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5200);
        require(session.status().state == NativeSession::State::EulaPage && !session.supplyOffer(fakeOffer, 5200),
            "No allocation before EULA retrieval and consent");
        auto read = session.nextRequest(5300);
        require(read && read->step == Step::NativeNotice && read->url ==
            "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getPage" &&
            Json::parse(read->body)["serviceToken"] == "FAKE_EULA_TOKEN" &&
            hasHeader(*read, "x-amz-profile-token: FAKE_PROFILE_TOKEN"), "EULA uses selected game's profile-bound page token");
        require(!session.chooseEulaButton(1, 5300), "Cannot accept an unread EULA");
        session.complete(read->id, 200, page.dump(), 5400);
        require(session.status().state == NativeSession::State::EulaNotice && !session.nextRequest(5500) &&
            !session.supplyOffer(fakeOffer, 5500), "A displayed EULA never triggers acceptance or provisioning");
    };
    for (const bool software : {false, true})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        openEula(auth, session, eulaPage(), software);
        require(session.status().softwareNotice.complete &&
            session.softwareNotice().text.find("http://example.invalid/terms") != std::string::npos &&
            session.softwareNotice().text.find("final clause") != std::string::npos &&
            session.softwareNotice().text.find("<a") == std::string::npos,
            "EULA preserves all inline legal text and external link destinations as readable text");
        require(!session.chooseEulaButton(99, 5500) && !session.chooseSoftwareNoticeButton(0, 5500),
            "Wrong notice handler and invalid selection cannot accept an EULA");
        require(session.chooseEulaButton(1, 5600), "Only explicit EULA button selection schedules acceptance");
        auto accept = session.nextRequest(5600);
        require(accept && accept->url == "https://proxy-prod.us-east-1.tempo.digital.a2z.com/writeCustomerPreferences" &&
            hasHeader(*accept, "x-amz-profile-token: FAKE_PROFILE_TOKEN") &&
            Json::parse(accept->body) == Json{{"timeout", 10000}, {"preferences", Json::array({Json{
                {"preferenceName", "EULA_ACCEPTED"},
                {"preferenceValue", Json{{"isAccepted", true}, {"productId", "FAKE_PROVISIONING_ID"}}.dump()}}})}},
            "Accept writes only the source-derived EULA preference for this game");
        require(!session.nextRequest(5601) && !session.chooseEulaButton(1, 5601), "Acceptance cannot be double-submitted");
        session.complete(accept->id, 200, Json{{"failedPreferences", Json::array()}}.dump(), 5700);
        require(session.status().eulaAccepted && session.status().state == NativeSession::State::CheckingPin,
            "Confirmed preference write resumes the original launch with a fresh permission check");
        auto pin = session.nextRequest(5800);
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5900);
        if (software)
        {
            require(session.status().state == NativeSession::State::SoftwarePage, "EULA consent cannot skip software-decoding notice");
            auto warning = session.nextRequest(6000);
            session.complete(warning->id, 200, noticePage().dump(), 6100);
            require(session.chooseSoftwareNoticeButton(0, 6200), "Software warning still requires its own explicit confirmation");
            pin = session.nextRequest(6200);
            session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 6300);
        }
        require(session.status().state == NativeSession::State::AwaitOffer && session.supplyOffer(fakeOffer, 6400),
            "Accepted EULA resumes without a second Play press or EULA loop");
        auto create = session.nextRequest(6400);
        require(create && create->url.find("/createGameSessionJob") != std::string::npos &&
            !session.nextRequest(6401), "Exactly one provisioning request follows completed consent checks");
    }
    for (const bool button : {false, true})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        openEula(auth, session, eulaPage());
        if (button) require(session.chooseEulaButton(0, 5600), "EULA dismiss is supported");
        else session.cancel(5600);
        require(session.status().state == NativeSession::State::Cancelled && !session.nextRequest(5700) &&
            !session.status().eulaAccepted, "Back/decline never saves consent or allocates a game");
    }
    for (const auto& response : {Json::object(), Json{{"failedPreferences", Json::array({"EULA_ACCEPTED"})}},
        Json{{"failedPreferences", "invalid"}}, Json{{"errorCode", 3}}})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        openEula(auth, session, eulaPage()); session.chooseEulaButton(1, 5600);
        auto write = session.nextRequest(5600); session.complete(write->id, 200, response.dump(), 5700);
        require(session.status().eulaAccepted == response.empty(), "Preference errors cannot masquerade as EULA acceptance");
        if (!response.empty()) require(!session.nextRequest(5800), "Rejected preference is never replayed");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        auto wrong = eulaPage();
        wrong["pageMemberGroups"]["mainContent"]["widgets"][1]["widgets"][1]["actions"][0]["payload"] =
            Json{{"productId", "FAKE_OTHER_GAME"}}.dump();
        openEula(auth, session, wrong);
        require(!session.chooseEulaButton(1, 5600) && !session.nextRequest(5700), "EULA action cannot change the selected game");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        openEula(auth, session, eulaPage()); session.chooseEulaButton(1, 5600);
        auto write = session.nextRequest(5600); session.transportFailed(write->id, 5700);
        require(!session.status().eulaAccepted && !session.nextRequest(5800), "Uncertain acceptance write is not retried or treated as consent");
        session.complete(write->id, 200, "{}", 5900);
        require(!session.status().eulaAccepted, "Late acceptance response cannot revive a failed attempt");
    }
    require(luna::launcher::consentText("<a href=\"javascript:alert(1)\">terms</a>").empty(),
        "Non-web consent links remain unsupported");
    require(luna::launcher::consentText("<p>Terms &amp; conditions — café.</p>") == "Terms & conditions — café.",
        "EULA rendering preserves international text and decodes entities");
    for (const auto* challenge : {"PIN", "DENY"})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        readyNative(auth, Json{{"dialogAcceptEulaToken", "FAKE_EULA_TOKEN"}});
        session.start(5100, true); auto pin = session.nextRequest(5100);
        session.complete(pin->id, 200, Json{{"challenge", challenge}}.dump(), 5200);
        require(session.status().state == NativeSession::State::Blocked && !session.nextRequest(5300) &&
            !session.chooseEulaButton(1, 5400), "EULA cannot bypass PIN or parental denial even with hardware decode");
    }
    for (const bool unknownWidget : {false, true})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        auto page = eulaPage();
        if (unknownWidget) page["pageMemberGroups"]["mainContent"]["widgets"].push_back(Json{{"type", "UNKNOWN_LEGAL_WIDGET"}});
        else page["pageMemberGroups"]["mainContent"]["widgets"][1]["widgets"][1]["actions"].push_back(Json{{"target", "UNKNOWN_WRITE"}});
        openEula(auth, session, page);
        require(!session.status().softwareNotice.complete && !session.chooseEulaButton(1, 5600) &&
            !session.nextRequest(5700), "Unknown EULA widgets or additional actions cannot be silently ignored");
        require(session.chooseEulaButton(0, 5800), "An unsupported EULA can still be cancelled");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        openEula(auth, session, eulaPage()); session.chooseEulaButton(1, 5600);
        auto write = session.nextRequest(5600); session.cancel(5650);
        session.complete(write->id, 200, "{}", 5700);
        require(!session.status().eulaAccepted && !session.nextRequest(5800) &&
            session.status().state == NativeSession::State::Cancelled,
            "Cancelling an in-flight acceptance never revives the launch on a late response");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        openEula(auth, session, eulaPage());
        require(!session.chooseEulaButton(1, 5100 + 2 * 60 * 60 * 1000) && !session.nextRequest(8'000'000),
            "Expired EULA attempt cannot write acceptance");
    }

    for (const auto* challenge : {"ALLOW", "PIN", "DENY"})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog);
        readyNative(auth, Json{{"dialogSoftwareDecodeToken", "FAKE_SOFTWARE_TOKEN"}});
        NativeSession session(auth);
        require(session.start(5100, true), "Initialized hardware decoder preflight starts one explicit attempt");
        auto pin = session.nextRequest(5100);
        require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos &&
            !session.supplyOffer(fakeOffer, 5101), "Hardware capability still requires parental authorization before an offer");
        session.complete(pin->id, 200, Json{{"challenge", challenge}}.dump(), 5200);
        if (std::string(challenge) == "ALLOW")
        {
            require(session.status().state == NativeSession::State::AwaitOffer &&
                !session.status().softwareNotice.accepted && !session.nextRequest(5250),
                "Hardware makes the software-only warning inapplicable without accepting or fetching it");
            require(session.supplyOffer(fakeOffer, 5300), "Hardware proceeds with its fresh offer after authorization");
            auto create = session.nextRequest(5300);
            require(create && create->url.find("/createGameSessionJob") != std::string::npos &&
                create->body.find("FAKE_SOFTWARE_TOKEN") == std::string::npos && !session.nextRequest(5350),
                "Hardware creates exactly one session without replaying a warning action");
        }
        else require(session.status().state == NativeSession::State::Blocked &&
            !session.supplyOffer(fakeOffer, 5300) && !session.nextRequest(5300),
            "Hardware capability cannot bypass a PIN challenge or denied launch");
    }

    const auto readSoftwareNotice = [&](NativeAuthProbe& auth, NativeSession& session, Json page = noticePage())
    {
        readyNative(auth, Json{{"buildId", "FAKE_RETAIL_BUILD"}, {"dialogSoftwareDecodeToken", "FAKE_SOFTWARE_TOKEN"}});
        require(session.start(5100), "Native launch with software warning starts explicitly");
        auto pin = session.nextRequest(5100);
        require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos,
            "Parental authorization precedes software warning retrieval");
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5200);
        require(session.status().state == NativeSession::State::SoftwarePage && !session.supplyOffer(fakeOffer, 5200),
            "No offer or create can bypass a pending software warning");
        auto warning = session.nextRequest(5300);
        require(warning && warning->step == Step::NativeNotice && warning->url ==
            "https://proxy-prod.us-east-1.tempo.digital.a2z.com/getPage", "Warning follows the browser's fixed authenticated page route");
        const auto body = Json::parse(warning->body);
        require(body["serviceToken"] == "FAKE_SOFTWARE_TOKEN" && body["featureScheme"] == "WEB_V1" &&
            body["clientContext"]["browserMetadata"]["browserType"] == "Chrome" &&
            body["inputContext"] == Json{{"gamepadTypes", Json::array()}} && body["dynamicFeatures"] == Json::array(),
            "Warning uses this game's private service token and the shared page request context");
        require(hasHeader(*warning, "x-amz-access-token: FAKE_NEW_ACCESS") &&
            hasHeader(*warning, "x-amz-profile-token: FAKE_PROFILE_TOKEN"), "Warning remains account/profile bound");
        require(!session.nextRequest(5301) && !session.chooseSoftwareNoticeButton(0, 5301), "Warning cannot be acknowledged before its response");
        session.complete(warning->id, 200, page.dump(), 5400);
        require(session.status().state == NativeSession::State::SoftwareNotice && !session.status().remoteWorkMayRemain &&
            !session.nextRequest(5500), "Warning waits for manual action without allocating a session");
        return *warning;
    };
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session);
        require(session.status().softwareNotice.complete && !session.status().softwareNotice.accepted &&
            session.softwareNotice().text.find("Read before continuing") != std::string::npos &&
            session.softwareNotice().choices.size() == 2, "Entire supported warning and its choices are available locally");
        require(!session.chooseSoftwareNoticeButton(99, 5600), "Out-of-range selection cannot accept the warning");
        require(session.chooseSoftwareNoticeButton(0, 5600) && session.status().softwareNotice.accepted,
            "Manual selection acknowledges the warning for this attempt only");
        auto pin = session.nextRequest(5600);
        require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos && !session.supplyOffer(fakeOffer, 5600),
            "Continuation rechecks parental authorization before creating an offer");
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5700);
        require(session.status().state == NativeSession::State::AwaitOffer && session.supplyOffer(fakeOffer, 5800),
            "Acknowledged warning does not loop; fresh offer follows authorization");
        auto create = session.nextRequest(5800);
        require(create && create->url.find("/createGameSessionJob") != std::string::npos &&
            !session.chooseSoftwareNoticeButton(0, 5900) && !session.nextRequest(5900), "Exactly one create after explicit warning continuation");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session);
        require(session.chooseSoftwareNoticeButton(1, 5600) && session.status().state == NativeSession::State::Cancelled &&
            !session.nextRequest(5700), "Luna's dismiss button cancels rather than acknowledging the warning");
    }
    {
        auto changed = launchAction("GAME_SESSION_ASYNC");
        auto payload = Json::parse(changed["payload"].get<std::string>()); payload["playToken"] = "FAKE_RENEWED_PLAY_TOKEN";
        changed["payload"] = payload.dump();
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, noticePage(changed));
        require(session.chooseSoftwareNoticeButton(0, 5600), "Same-game continuation may renew the private play token");
        auto pin = session.nextRequest(5600); session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5700);
        session.supplyOffer(fakeOffer, 5800); auto create = session.nextRequest(5800);
        require(create && Json::parse(create->body)["playToken"] == "FAKE_RENEWED_PLAY_TOKEN", "Creation uses the explicitly selected authenticated continuation token");
    }
    for (const auto* field : {"gameId", "productStage"})
    {
        auto changed = launchAction("GAME_SESSION_ASYNC"); auto payload = Json::parse(changed["payload"].get<std::string>());
        payload[field] = "FAKE_OTHER_VALUE"; changed["payload"] = payload.dump();
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, noticePage(changed));
        require(!session.chooseSoftwareNoticeButton(0, 5600) && session.status().state == NativeSession::State::Failed &&
            !session.nextRequest(5700), "Warning continuation cannot silently change game or product stage");
    }
    for (const auto* gate : {"transferRequest", "dialogTermsAndConditionsToken", "dialogAcceptEulaToken"})
    {
        auto changed = launchAction("GAME_SESSION_ASYNC"); auto payload = Json::parse(changed["payload"].get<std::string>());
        payload[gate] = "FAKE_REQUIREMENT"; changed["payload"] = payload.dump();
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, noticePage(changed));
        require(!session.chooseSoftwareNoticeButton(0, 5600) && session.status().state == NativeSession::State::Blocked &&
            !session.nextRequest(5700), "Acknowledging software decode cannot bypass requirements in the continuation");
    }

    const auto softwarePreferencePage = [](Json preference = Json{{"modalId", "software_decode"}, {"shouldDismissDialog", false}})
    {
        auto warning = noticePage();
        warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0] =
            Json{{"type", "BASIC_DIALOG"}, {"presentationData", Json{{"title", "Software decoding"},
                {"description", "A synthetic performance warning."}, {"checkboxText", "Do not show this warning again"}}.dump()},
                {"actions", Json::array({Json{{"target", "DO_NOT_SHOW_AGAIN"}, {"payload", preference.dump()}}})}};
        return warning;
    };
    for (bool dismiss : {false, true})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth);
        readSoftwareNotice(auth, session, softwarePreferencePage(Json{{"modalId", "software_decode"}, {"shouldDismissDialog", dismiss}}));
        const auto& diag = session.status().softwareNotice;
        require(diag.complete && diag.checkboxes == 1 && diag.checkboxPreferenceSupported &&
            diag.checkboxAction == NoticeAction::DoNotShowAgain && diag.unsupported == 0 && !diag.accepted,
            "Known software-warning suppression preference can remain unchecked without blocking its warning");
        require(session.softwareNotice().text.find("[ ] Do not show this warning again") != std::string::npos &&
            session.softwareNotice().text.find("Preference left unchecked") != std::string::npos && !session.nextRequest(5501),
            "The full unchecked preference is readable and cannot automatically allocate or acknowledge");
        require(session.chooseSoftwareNoticeButton(0, 5600), "Continue explicitly acknowledges the warning with suppression left off");
        auto pin = session.nextRequest(5600);
        require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos && !session.supplyOffer(fakeOffer, 5600),
            "Optional warning preference never replaces the post-warning parental check");
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5700);
        require(session.supplyOffer(fakeOffer, 5800), "Fresh offer follows explicitly acknowledged warning and parental ALLOW");
        auto create = session.nextRequest(5800);
        require(create && create->url.find("/createGameSessionJob") != std::string::npos,
            "An unchecked optional suppression preference permits the original native launch path");
    }
    for (int fixture = 0; fixture < 7; ++fixture)
    {
        auto warning = softwarePreferencePage();
        auto& dialog = warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0];
        auto payload = Json::parse(dialog["actions"][0]["payload"].get<std::string>());
        if (fixture == 0) payload["modalId"] = "accept_terms_and_conditions_modal_name";
        if (fixture == 1) payload["shouldExecuteActionOnLoad"] = true;
        if (fixture == 2) payload["shouldDismissDialog"] = "FAKE_NON_BOOLEAN";
        if (fixture == 3) dialog["actions"][0]["target"] = "UNKNOWN_CONSENT_ACTION";
        if (fixture == 4) dialog["actions"].push_back(Json{{"target", "PROFILE_PIN_ENTRY_OPEN"}});
        if (fixture == 5) payload["modalId"] = 0;
        if (fixture == 6) dialog["actions"].clear();
        if (fixture != 6) dialog["actions"][0]["payload"] = payload.dump();
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, warning);
        require(!session.status().softwareNotice.complete && !session.status().softwareNotice.checkboxPreferenceSupported &&
            session.status().softwareNotice.firstIssue == NoticeIssue::Checkbox && !session.chooseSoftwareNoticeButton(0, 5600) &&
            !session.nextRequest(5700), "Other modal preferences, automatic actions, malformed data and extra requirements remain blocked");
    }
    {
        auto warning = softwarePreferencePage();
        auto& dialog = warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][0];
        auto data = Json::parse(dialog["presentationData"].get<std::string>()); data["legalTerms"] = Json::array({"Fake required legal content"});
        dialog["presentationData"] = data.dump();
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, warning);
        require(session.status().softwareNotice.checkboxPreferenceSupported && !session.status().softwareNotice.complete &&
            session.status().softwareNotice.firstIssue == NoticeIssue::LegalTerms && !session.chooseSoftwareNoticeButton(0, 5600),
            "Recognizing an optional checkbox cannot suppress unrelated legal requirements");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); auto notice = openNotice(auth);
        auth.complete(notice.id, 200, softwarePreferencePage().dump(), 5200);
        require(!auth.status().noticeParsing.complete && !auth.status().noticeParsing.checkboxPreferenceSupported &&
            !auth.chooseNoticeButton(0, 5300), "Software checkbox exception is unavailable to experimental-client or other notice contexts");
    }
    {
        auto warning = softwarePreferencePage(); auto& widgets = warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
        const auto additional = widgets[0]; widgets.push_back(additional);
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, warning);
        require(session.status().softwareNotice.checkboxes == 2 && !session.status().softwareNotice.complete &&
            !session.chooseSoftwareNoticeButton(0, 5600), "Multiple checkbox controls require additional UI support and cannot be silently skipped");
    }
    for (int fixture = 0; fixture < 5; ++fixture)
    {
        auto warning = noticePage(); auto& widgets = warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"];
        if (fixture == 0) widgets[1]["actions"].push_back(Json{{"target", "UNKNOWN_REQUIRED_ACTION"}});
        if (fixture == 1) widgets.push_back(Json{{"type", "UNSUPPORTED_FORM"}});
        if (fixture == 2) warning["actions"] = Json::array({Json{{"target", "PCON_PIN_CHALLENGE"}}});
        if (fixture == 3) widgets[0] = Json{{"type", "BASIC_DIALOG"}, {"presentationData", Json{{"title", "Warning"}, {"checkboxText", "Required consent"}}.dump()}};
        if (fixture == 4) widgets[0] = Json{{"type", "BASIC_DIALOG"}, {"presentationData", Json{{"title", "Warning"},
            {"legalTerms", Json::array({"Fake legal content"})}}.dump()}};
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, warning);
        require(!session.status().softwareNotice.complete && !session.chooseSoftwareNoticeButton(0, 5600) &&
            !session.nextRequest(5700), "Unknown extra actions, forms, root gates and unread legal controls disable continuation");
        const auto& diag = session.status().softwareNotice;
        if (fixture == 0)
            require(diag.firstIssue == NoticeIssue::SecondaryAction && diag.secondaryActions == 1 &&
                diag.firstIssueButton == 1 && diag.firstIssueAction == 2 &&
                diag.firstUnsupportedAction == NoticeAction::Unknown, "Unknown secondary action has fixed reason/type and one-based button/action positions");
        if (fixture == 1) require(diag.firstIssue == NoticeIssue::Widget && diag.unsupportedWidgets == 1,
            "Unknown widgets are distinguishable from action and consent issues");
        if (fixture == 3) require(diag.firstIssue == NoticeIssue::Checkbox && diag.checkboxes == 1 && diag.legalTerms == 0,
            "Unsupported checkbox is identified without logging its text");
        if (fixture == 4) require(diag.firstIssue == NoticeIssue::LegalTerms && diag.checkboxes == 0 && diag.legalTerms == 1,
            "Unimplemented legal content is identified without logging its content");

    }
    for (const auto* target : {"LOCAL_STORAGE_TOGGLE", "FAKE_PLAY_TOKEN"})
    {
        auto warning = noticePage(); warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][1]["actions"].push_back(
            Json{{"target", target}, {"payload", Json{{"key", "FAKE_PRIVATE_PREFERENCE"}, {"enabled", true}}.dump()}});
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, warning);
        const auto& diag = session.status().softwareNotice;
        const bool known = std::string(target) == "LOCAL_STORAGE_TOGGLE";
        require(diag.firstUnsupportedAction == (known ? NoticeAction::LocalStorageToggle : NoticeAction::Unknown) &&
            std::string(noticeActionLabel(diag.firstUnsupportedAction)) == (known ? "LOCAL_STORAGE_TOGGLE" : "unknown"),
            "Terminal action identifiers come exclusively from a static public allowlist, not arbitrary server strings");
        require(session.softwareNotice().unsupportedActionTargets.size() == 1 &&
            session.softwareNotice().unsupportedActionTargets[0].find("FAKE_PLAY_TOKEN") == std::string::npos &&
            !session.chooseSoftwareNoticeButton(0, 5600) && !session.nextRequest(5700),
            "Known credentials are redacted even from local action hints; additional diagnostics never bypass the gate");
    }
    {
        auto warning = noticePage(); warning["pageMemberGroups"]["mainContent"]["widgets"][0]["widgets"][1]["actions"].push_back(Json{{"target", "DIALOG_DISMISS"}});
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); readSoftwareNotice(auth, session, warning);
        require(session.status().softwareNotice.complete, "A secondary dismiss must not make the software warning unsupported");
        require(session.chooseSoftwareNoticeButton(0, 5600), "A supported continue action may also close its software warning");
        auto pin = session.nextRequest(5600); session.complete(pin->id, 200, Json{{"challenge", "PIN"}}.dump(), 5700);
        require(session.status().block == NativeSession::Block::ParentalPin && !session.supplyOffer(fakeOffer, 5800),
            "Rechecking parental authorization after acknowledgement still honors a new PIN requirement");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); NativeSession session(auth); const auto warning = readSoftwareNotice(auth, session);
        session.cancel(5600); session.complete(warning.id, 200, noticePage().dump(), 5700);
        require(session.status().state == NativeSession::State::Cancelled && !session.chooseSoftwareNoticeButton(0, 5800) &&
            !session.nextRequest(5800), "Cancel invalidates stale warning results and acknowledgements");
    }
    require(responseLimit(Step::NativeNotice) == MaxLunaResponse, "Software notice shares the bounded page-response limit");
    // Diagnose rejected launch fields without exposing their values.
    unsigned requirementBit = 1;
    for (const auto& field : std::vector<std::pair<const char*, NativeSession::Requirement>>{
        {"transferRequest", NativeSession::Requirement::Transfer},
        {"revokeProfileActivityRequest", NativeSession::Requirement::RevokeActivity},
        {"partyId", NativeSession::Requirement::Party}, {"streamerMode", NativeSession::Requirement::StreamerMode},
        {"buildId", NativeSession::Requirement::Build}, {"gameBuildConfigurationId", NativeSession::Requirement::BuildConfiguration},
        {"saveSlotId", NativeSession::Requirement::SaveSlot}, {"pconToken", NativeSession::Requirement::ParentalProof}})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth, Json{{field.first, "FAKE_PRIVATE_REQUIREMENT_VALUE"}});
        NativeSession session(auth); session.start(5100);
        const auto& status = session.status();
        if (field.second == NativeSession::Requirement::Build || field.second == NativeSession::Requirement::BuildConfiguration)
        {
            require(status.state == NativeSession::State::CheckingPin && status.unsupportedFields == 0 &&
                status.requirement == NativeSession::Requirement::None,
                "Retail build metadata does not imply the browser's direct-build launch mode");
            auto pin = session.nextRequest(5200);
            require(pin && pin->url.find("/isPinChallengeRequired") != std::string::npos,
                "Retail metadata still requires server parental authorization");
        }
        else
        {
            require(status.requirement == field.second && status.requirementKind == NativeSession::ValueKind::String &&
                status.unsupportedFields == requirementBit, "Each blocked field has a distinct safe enum/mask");
            require(std::string(NativeSession::requirementLabel(status.requirement)) == field.first &&
                std::string(NativeSession::valueKindLabel(status.requirementKind)) == "string",
                "Diagnostics expose only fixed field/type labels, never the payload value");
            require(!session.nextRequest(5200) && !status.remoteWorkMayRemain,
                "Adding field diagnostics cannot allocate a game or bypass the guard");
        }
        requirementBit <<= 1;
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog);
        readyNative(auth, Json{{"buildId", "FAKE_RETAIL_BUILD"}, {"gameBuildConfigurationId", "FAKE_RETAIL_CONFIG"}});
        NativeSession session(auth); const auto create = createNative(session);
        const auto body = Json::parse(create.body);
        require(!body.contains("buildId") && !body.contains("gameBuildConfigurationId"),
            "Normal launch drops both metadata fields exactly as the retail browser path does");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog);
        readyNative(auth, Json{{"buildId", "FAKE_RETAIL_BUILD"}, {"dialogTermsAndConditionsToken", "FAKE_TERMS_TOKEN"}});
        NativeSession session(auth); session.start(5100);
        require(session.status().state == NativeSession::State::Blocked && session.status().block == NativeSession::Block::Terms &&
            !session.nextRequest(5200), "Accepting retail metadata cannot skip required terms");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog);
        readyNative(auth, Json{{"buildId", "FAKE_RETAIL_BUILD"}, {"transferRequest", true}});
        NativeSession session(auth); session.start(5100);
        require(session.status().state == NativeSession::State::Blocked && session.status().requirement == NativeSession::Requirement::Transfer &&
            session.status().unsupportedFields == 1 && !session.nextRequest(5200),
            "Retail metadata does not authorize takeover or alter diagnostic mask positions");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog);
        readyNative(auth, Json{{"buildId", "FAKE_RETAIL_BUILD"}});
        NativeSession session(auth); session.start(5100); auto pin = session.nextRequest(5200);
        session.complete(pin->id, 200, Json{{"challenge", "PIN"}}.dump(), 5300);
        require(session.status().state == NativeSession::State::Blocked && session.status().block == NativeSession::Block::ParentalPin &&
            !session.supplyOffer(fakeOffer, 5400) && !session.nextRequest(5400), "Retail metadata never bypasses parental PIN challenges");
    }
    for (const auto& fixture : std::vector<std::pair<Json, NativeSession::ValueKind>>{
        {true, NativeSession::ValueKind::Boolean}, {0, NativeSession::ValueKind::ZeroNumber},
        {0.0, NativeSession::ValueKind::ZeroNumber}, {12, NativeSession::ValueKind::Number},
        {Json::array(), NativeSession::ValueKind::Array}, {Json::object(), NativeSession::ValueKind::Object}})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth, Json{{"saveSlotId", fixture.first}});
        NativeSession session(auth); session.start(5100);
        require(session.status().state == NativeSession::State::Blocked &&
            session.status().requirementKind == fixture.second && !session.nextRequest(5200),
            "Value shape, including zero defaults, is diagnosed without changing launch authorization");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth, Json{{"partyId", "FAKE_PARTY"}, {"saveSlotId", 0}});
        NativeSession session(auth); session.start(5100);
        require(session.status().unsupportedFields == (4u | 64u) && session.status().requirement == NativeSession::Requirement::Party,
            "All blocked fields are counted while first-field diagnostics remain deterministic");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth, Json{{"transferRequest", false},
            {"revokeProfileActivityRequest", nullptr}, {"partyId", ""}, {"streamerMode", false}});
        NativeSession session(auth); session.start(5100);
        require(session.status().state == NativeSession::State::CheckingPin && session.status().unsupportedFields == 0 &&
            session.status().requirement == NativeSession::Requirement::None && session.status().requirementKind == NativeSession::ValueKind::None,
            "Null, false and empty optional fields do not produce unsupported-requirement diagnostics");
    }
    for (const auto* challenge : {"PIN", "DENY", "UNKNOWN"})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth);
        NativeSession session(auth); session.start(5100); auto pin = session.nextRequest(5100);
        session.complete(pin->id, 200, Json{{"challenge", challenge}}.dump(), 5200);
        require(session.status().state != NativeSession::State::AwaitOffer && !session.nextRequest(5300), "Never treat PIN, DENY or unknown as ALLOW");
    }
    for (const auto* malformed : {"{\"gameSessionJobId\":\"FAKE_JOB\",\"gameSessionJobId\":\"OTHER\"}", "[]", "{}"})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        const auto create = createNative(session); session.complete(create.id, 200, malformed, 5500);
        require(session.status().state == NativeSession::State::Failed && session.status().remoteWorkMayRemain &&
            !session.nextRequest(5600), "Malformed/ambiguous create cannot be replayed or declared cancelled");
    }
    for (const auto mode : {luna::video::Mode::P720_30, luna::video::Mode::P720_60,
        luna::video::Mode::P1080_30, luna::video::Mode::P1080_60})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        session.start(5100); auto pin = session.nextRequest(5100);
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5200);
        require(session.supplyOffer(fakeOffer, 5300, mode), "Selected runtime mode binds provisioning quality hint");
        auto request = session.nextRequest(5300);
        require(request && Json::parse(request->body)["clientFeatures"]["dataUsage"] ==
            (luna::video::configuration(mode).height == 720 ? "LIMITED" : "STANDARD"),
            "Data-usage hint follows effective resolution independently of FPS");
    }
    for (const auto mode : {luna::video::Mode::P720_30, luna::video::Mode::P720_60})
    {
        Json baseline;
        for (const auto quality : {NativeSession::Quality::MatchMode, NativeSession::Quality::Standard})
        {
            NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
            session.start(5100); auto pin = session.nextRequest(5100);
            session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5200);
            require(session.supplyOffer(fakeOffer, 5300, mode, quality), "Explicit diagnostic quality preserves normal preflight");
            auto create = session.nextRequest(5300); auto body = Json::parse(create->body);
            if (quality == NativeSession::Quality::MatchMode) baseline = body;
            else
            {
                require(body["clientFeatures"]["dataUsage"] == "STANDARD" &&
                    Json::parse(body["signalingRequest"].get<std::string>())["sdp"] == fakeOffer,
                    "STANDARD diagnostic leaves the runtime's selected-mode SDP intact");
                body["clientFeatures"]["dataUsage"] = "LIMITED";
                require(body == baseline && !session.nextRequest(5301),
                    "Quality experiment changes exactly one provisioning field with no automatic retry");
            }
        }
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        session.start(5100); auto pin = session.nextRequest(5100);
        session.complete(pin->id, 200, Json{{"challenge", "ALLOW"}}.dump(), 5200);
        require(!session.supplyOffer(fakeOffer, 5300, luna::video::Mode::P720_30,
            static_cast<NativeSession::Quality>(99)) && !session.nextRequest(5301), "Unknown quality cannot create a job");
    }
    struct HttpCase
    {
        std::string body;
        NativeSession::HttpError error;
        NativeSession::ErrorBody shape;
        NativeSession::ErrorField hint;
    };
    for (const auto& item : std::vector<HttpCase>{
        {Json{{"__type", "com.amazon.example#ValidationException"},
            {"message", "Value at 'clientFeatures.dataUsage' must not be null: FAKE_ACCESS_SECRET"}}.dump(),
            NativeSession::HttpError::Validation, NativeSession::ErrorBody::Object, NativeSession::ErrorField::ClientFeatures},
        {Json{{"response", {{"error", {{"code", "InvalidRequestException"},
            {"message", "Invalid signalingRequest: FAKE_PLAY_TOKEN"}}}}}}.dump(),
            NativeSession::HttpError::InvalidRequest, NativeSession::ErrorBody::Object, NativeSession::ErrorField::SignalingRequest},
        {Json{{"error", "FAKE_ACCESS_SECRET"}, {"message", "FAKE_REFRESH_SECRET"}}.dump(),
            NativeSession::HttpError::Unknown, NativeSession::ErrorBody::Object, NativeSession::ErrorField::None},
        {Json{{"type", "NotAuthorizedException"}, {"field", "playToken"}}.dump(),
            NativeSession::HttpError::Unauthorized, NativeSession::ErrorBody::Object, NativeSession::ErrorField::PlayToken},
        {Json{{"__type", "ValidationException"}, {"message", "mygameIdValue invalid"}}.dump(),
            NativeSession::HttpError::Validation, NativeSession::ErrorBody::Object, NativeSession::ErrorField::None},
        {"", NativeSession::HttpError::Unknown, NativeSession::ErrorBody::Empty, NativeSession::ErrorField::None},
        {"<html>FAKE_ACCESS_SECRET</html>", NativeSession::HttpError::Unknown, NativeSession::ErrorBody::Unparseable, NativeSession::ErrorField::None},
        {"[]", NativeSession::HttpError::Unknown, NativeSession::ErrorBody::Unparseable, NativeSession::ErrorField::None},
        {"{\"__type\":\"ValidationException\",\"__type\":\"FAKE_ACCESS_SECRET\"}",
            NativeSession::HttpError::Unknown, NativeSession::ErrorBody::Unparseable, NativeSession::ErrorField::None},
        {std::string(MaxAuthResponse + 1, 'x'), NativeSession::HttpError::Unknown, NativeSession::ErrorBody::TooLarge, NativeSession::ErrorField::None}})
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        auto create = createNative(session); session.complete(create.id, 400, item.body, 5500);
        const auto& status = session.status(); const auto& d = status.httpDiagnostic;
        require(status.state == NativeSession::State::Failed && status.block == NativeSession::Block::Http &&
            status.httpStatus == 400 && status.remoteWorkMayRemain && !session.nextRequest(5600),
            "Rejected create remains stopped and conservatively preserves remote uncertainty");
        require(d.operation == NativeSession::Operation::Create && d.error == item.error &&
            d.body == item.shape && d.fieldHint == item.hint && d.responseBytes == item.body.size(),
            "HTTP diagnostics classify bounded response structure and fixed labels only");
        const auto labels = std::string(NativeSession::operationLabel(d.operation)) + NativeSession::httpErrorLabel(d.error) +
            NativeSession::errorBodyLabel(d.body) + NativeSession::errorFieldLabel(d.fieldHint);
        require(labels.find("FAKE_") == std::string::npos, "No server-supplied credential/message identifier leaves error classifier");
        session.complete(create.id, 200, created(), 5700);
        require(session.status().state == NativeSession::State::Failed && !session.nextRequest(5800),
            "A stale success cannot revive a rejected create");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        auto create = createNative(session); session.complete(create.id, 200, created(), 5500);
        auto poll = session.nextRequest(10500);
        session.complete(poll->id, 400, Json{{"errorCode", 42}, {"message", "gameSessionJobId rejected"}}.dump(), 10600);
        require(session.status().httpDiagnostic.operation == NativeSession::Operation::Poll &&
            session.status().apiErrorCode == 42 && !session.nextRequest(11000),
            "Non-200 poll preserves safe numeric API code and identifies the failing operation without recreating");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        session.start(5100); auto pin = session.nextRequest(5100);
        session.complete(pin->id, 403, Json{{"__type", "AccessDeniedException"}}.dump(), 5200);
        require(session.status().httpDiagnostic.operation == NativeSession::Operation::Pin &&
            session.status().httpDiagnostic.error == NativeSession::HttpError::AccessDenied &&
            !session.status().remoteWorkMayRemain && !session.nextRequest(5300),
            "Pre-provisioning parental HTTP failure remains distinguishable from uncertain create failure");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        const auto create = createNative(session); session.transportFailed(create.id, 5500);
        require(session.status().state == NativeSession::State::Failed && session.status().remoteWorkMayRemain &&
            !session.nextRequest(5600), "Lost create response never retries allocation");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        const auto create = createNative(session); session.cancel(5400);
        require(session.status().jobState == luna::session::State::Cancelling && !session.nextRequest(5400), "Cancel waits for an in-flight create");
        session.complete(create.id, 200, created(), 5500);
        auto cancel = session.nextRequest(5500);
        require(cancel && cancel->url.find("/cancelGameSessionJob") != std::string::npos, "Cancel uses the newly known job ID");
        session.complete(cancel->id, 200, "{}", 5600);
        require(session.status().state == NativeSession::State::Cancelled && session.status().remoteWorkMayRemain,
            "Job cancellation ACK does not claim allocated-game termination");
    }
    {
        NativeAuthProbe auth(Mode::ProfilesAndCatalog); readyNative(auth); NativeSession session(auth);
        const auto create = createNative(session); session.complete(create.id, 200, created(), 5500);
        auto refresh = session.nextRequest(3'550'000);
        require(refresh && refresh->url == "https://api.amazon.com/auth/token" && session.status().refreshing,
            "Long queue proactively refreshes before token expiry, after original browser deadline");
        session.complete(refresh->id, 200, Json{{"access_token", "FAKE_QUEUE_REFRESH"}, {"expires_in", "3600"},
            {"refresh_token", "FAKE_QUEUE_ROTATED_REFRESH"}}.dump(), 3'550'100);
        auto poll = session.nextRequest(3'550'100);
        require(poll && poll->url.find("/getGameSessionJob") != std::string::npos &&
            hasHeader(*poll, "x-amz-access-token: FAKE_QUEUE_REFRESH"), "Refresh preserves the original job and profile, never recreates it");
        session.complete(poll->id, 200, queued(), 3'550'200);
        auto nextRefresh = session.nextRequest(7'100'000);
        require(nextRefresh && nextRefresh->url == "https://api.amazon.com/auth/token" &&
            Json::parse(nextRefresh->body)["source_token"] == "FAKE_QUEUE_ROTATED_REFRESH",
            "Queue refresh rotation updates the same private remembered credential used by later refreshes");
        session.complete(nextRefresh->id, 200, Json{{"access_token", "FAKE_QUEUE_REFRESH_2"}, {"expires_in", 3600}}.dump(), 7'100'100);
        require(!session.nextRequest(7'205'100) && session.status().state == NativeSession::State::Failed &&
            session.status().remoteWorkMayRemain, "Token refresh never extends the two-hour queue cap or erases remote allocation uncertainty");
    }
    std::cout << "PASS: " << checks << " native auth/profile/catalog/detail checks (synthetic data, no network).\n";
}
