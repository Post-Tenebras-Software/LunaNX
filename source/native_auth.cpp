#include "native_auth.hpp"
#include "launcher_media.hpp"
#include "launcher_text.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string_view>

namespace luna::auth
{
namespace
{
using Json = nlohmann::json;
constexpr const char* AuthOrigin = "https://api.amazon.com";
constexpr const char* ProxyOrigin = "https://proxy-prod.us-east-1.tempo.digital.a2z.com";
// Browser-protocol compatibility profile based on the working Fedora Chromium
// caller, not a claim about the Switch OS/CPU or a supported native client.
// Registration remains Nintendo Switch/Horizon; this suffix identifies us.
constexpr const char* ProxyBrowserVersion = "156.0.0.0";
std::string proxyUserAgent()
{
    return std::string("User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 ") +
        "(KHTML, like Gecko) Chrome/" + ProxyBrowserVersion + " Safari/537.36 LunaNX/alpha-auth";
}
Json proxyClientContext()
{
    // ClientContextHelper (1055), BrowserClientRole (62176). This is the
    // JSON context injected by ProxyClient.getPage/getWidget (11707), separate
    // from HTTP User-Agent and the Switch/Horizon registration identity.
    return {{"browserMetadata", {{"browserClientRole", "browser"}, {"browserType", "Chrome"},
        {"browserVersion", ProxyBrowserVersion}, {"deviceModel", "unknown"}, {"deviceType", "unknown"},
        {"osName", "Linux"}, {"osVersion", "unknown"}}}, {"refMarker", nullptr}, {"referrer", nullptr}};
}

void wipe(std::string& value)
{
    volatile char* data = value.empty() ? nullptr : &value[0];
    for (std::size_t i = 0; i < value.size(); ++i) data[i] = 0;
    value.clear();
}
Json parse(const std::string& body, std::size_t limit)
{
    if (body.empty() || body.size() > limit) throw std::invalid_argument("Invalid auth response");
    std::vector<std::set<std::string>> keys;
    auto value = Json::parse(body, [&](int depth, Json::parse_event_t event, Json& item)
    {
        if (depth > 32) throw std::invalid_argument("Auth response nesting limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second)
            throw std::invalid_argument("Duplicate auth field");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    if (!value.is_object()) throw std::invalid_argument("Expected auth object");
    return value;
}
std::string text(const Json& value, const char* name, std::size_t limit)
{
    const auto& field = value.at(name);
    if (!field.is_string()) throw std::invalid_argument("Expected auth string");
    auto result = field.get<std::string>();
    if (result.empty() || result.size() > limit || std::any_of(result.begin(), result.end(),
        [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
        throw std::invalid_argument("Invalid auth string");
    return result;
}
std::int64_t seconds(const Json& value, const char* key, std::int64_t maximum)
{
    const auto& item = value.at(key);
    std::int64_t result;
    if (item.is_number_integer()) result = item.get<std::int64_t>();
    else if (item.is_string())
    {
        const auto string = item.get<std::string>();
        if (string.empty() || string.size() > 8 || string.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("Invalid auth interval");
        result = std::stoll(string);
    }
    else throw std::invalid_argument("Invalid auth interval");
    if (result < 1 || result > maximum) throw std::invalid_argument("Auth interval outside bounds");
    return result;
}
Json registration(const std::string& serial)
{
    // The public Luna CBL library's generic registration, not a TV's identity.
    // Code-pair issuance is verified separately from account/Luna authorization.
    // Amazon's public naming template still collided on repeated probe runs.
    // Bind the full name to this attempt's random serial, including every bit;
    // rebuilding the same registration for approval polls must not rename it.
    return {{"domain", "Device"}, {"device_name", "LunaNX Switch " + serial},
        {"app_name", "Amazon Luna"}, {"app_version", "1.0.37340.0"},
        {"device_model", "Nintendo Switch (LunaNX)"}, {"device_type", "A1MPSLFC7L5AFK"},
        {"device_serial", serial}, {"os_version", "Horizon"}};
}
bool terminal(Step step)
{
    return step == Step::Complete || step == Step::Failed || step == Step::Cancelled || step == Step::Idle;
}
// Auth failures may be a top-level error string or Panda's nested error object.
// Only exact allowlisted categories and parameter names leave this parser.
// Descriptions can contain credentials and must never be copied into Status.
void classifyAuthError(Status& status, std::string& localIdentifier, const std::string& body,
    std::initializer_list<std::string_view> secrets)
{
    status.authError = AuthError::Unknown;
    localIdentifier.clear();
    status.authResponseBytes = body.size();
    if (body.empty()) { status.authResponse = AuthResponse::Empty; return; }
    if (body.size() > MaxAuthResponse) { status.authResponse = AuthResponse::TooLarge; return; }
    status.authResponse = AuthResponse::Unparseable;
    try
    {
        const auto value = parse(body, MaxAuthResponse);
        const Json* container = &value;
        status.authResponse = AuthResponse::NoErrorField;
        if (value.contains("response") && value["response"].is_object() && value["response"].contains("error"))
        {
            container = &value["response"];
            status.authResponse = AuthResponse::NestedError;
        }
        else if (value.contains("error")) status.authResponse = AuthResponse::TopLevelError;
        const auto& error = container->at("error");
        const auto& codeValue = error.is_object() ? error.at("code") : error;
        const auto code = codeValue.is_string() ? text(Json{{"code", codeValue}}, "code", 128) :
            codeValue.is_number_integer() && codeValue >= 0 && codeValue <= 999999 ?
                "error_number_" + std::to_string(codeValue.get<unsigned>()) : std::string();
        // Public error names are short identifiers, not arbitrary server text.
        // Also exclude our actual codes/tokens even when they look like names.
        // This value is intentionally separate from log-safe Status.
        const bool name = !code.empty() && code.size() <= 80 &&
            code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:-") == std::string::npos &&
            code[0] != '.' && code[0] != ':' && code[0] != '-' &&
            code.compare(0, 6, "amzn1.") != 0;
        const bool sensitive = std::any_of(secrets.begin(), secrets.end(), [&](std::string_view secret)
        { return !secret.empty() && code.find(secret) != std::string::npos; });
        if (name && !sensitive) localIdentifier = code;
        for (const auto& item : std::vector<std::pair<const char*, AuthError>>{
            {"InvalidValue", AuthError::InvalidValue}, {"MissingValue", AuthError::MissingValue},
            {"InvalidRequest", AuthError::InvalidRequest}, {"invalid_request", AuthError::InvalidRequest},
            {"InvalidClient", AuthError::InvalidClient}, {"invalid_client", AuthError::InvalidClient},
            {"unauthorized_client", AuthError::UnauthorizedClient}, {"Unauthorized", AuthError::Unauthorized},
            {"access_denied", AuthError::AccessDenied}, {"invalid_grant", AuthError::InvalidGrant},
            {"InvalidCodePair", AuthError::InvalidCodePair}, {"ExpiredCodePair", AuthError::ExpiredCodePair},
            {"InvalidData", AuthError::InvalidData}, {"server_error", AuthError::ServerError},
            {"InternalError", AuthError::ServerError}, {"slow_down", AuthError::SlowDown},
            {"InvalidToken", AuthError::InvalidToken}, {"DuplicateDeviceName", AuthError::DuplicateDeviceName},
            {"DuplicateDeviceNameError", AuthError::DuplicateDeviceName}})
            if (code == item.first) { status.authError = item.second; break; }
        const Json* description = nullptr;
        if (error.is_object() && error.contains("message")) description = &error["message"];
        else if (container->contains("error_description")) description = &(*container)["error_description"];
        if (!description || !description->is_string()) return;
        const auto message = description->get<std::string>();
        for (const auto* prefix : {"The request has an invalid parameter : ", "The request is missing a required parameter : "})
        {
            if (message.compare(0, std::char_traits<char>::length(prefix), prefix) != 0) continue;
            status.authParameter = AuthParameter::Unknown;
            const auto parameter = message.substr(std::char_traits<char>::length(prefix));
            for (const auto& item : std::vector<std::pair<const char*, AuthParameter>>{
                {"code_pair", AuthParameter::CodePair}, {"public_code", AuthParameter::PublicCode},
                {"private_code", AuthParameter::PrivateCode}, {"registration_data", AuthParameter::RegistrationData},
                {"device_serial", AuthParameter::DeviceSerial}, {"device_type", AuthParameter::DeviceType},
                {"source_token", AuthParameter::SourceToken}})
                if (parameter == item.first) { status.authParameter = item.second; break; }
        }
    }
    catch (const std::exception&) {} // Unrecognized responses stay redacted.
}
bool successful(const Json& value)
{
    return (!value.contains("errorCode") || value["errorCode"].is_null() || value["errorCode"] == 0) &&
        (!value.contains("error") || value["error"].is_null());
}
bool browseRead(Step step) { return step == Step::SearchPage || step == Step::BrowsePage || step==Step::CatalogAccess; }
const Json& pageGroups(const Json& value)
{
    const auto& groups = value.at("pageMemberGroups");
    if (!successful(value) || !groups.is_object() || groups.empty())
        throw std::invalid_argument("Unsuccessful Luna page");
    return groups;
}
std::string displayText(const Json& value, const char* name)
{
    const auto result = value.at(name).get<std::string>();
    if (result.empty() || result.size() > 256 || std::any_of(result.begin(), result.end(),
        [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        throw std::invalid_argument("Invalid display text");
    return result;
}
Json presentation(const Json& widget)
{
    const auto& data = widget.at("presentationData");
    // PDS transmits presentationData as JSON text; the web wrapper parses it.
    return data.is_string() ? parse(data.get<std::string>(), MaxAuthResponse) : data;
}
bool flag(const Json& value, const char* name)
{
    if (!value.contains(name) || value[name].is_null()) return false;
    return value.at(name).get<bool>();
}
template<typename Visitor> void visitWidgets(const Json& groups, Visitor visit)
{
    std::size_t count = 0;
    const auto walk = [&](const auto& self, const Json& widgets, unsigned depth) -> void
    {
        if (!widgets.is_array() || depth > 24) throw std::invalid_argument("Invalid widgets");
        for (const auto& widget : widgets)
        {
            if (!widget.is_object() || ++count > 4096) throw std::invalid_argument("Widget limit");
            visit(widget);
            if (widget.contains("widgets") && !widget["widgets"].is_null()) self(self, widget.at("widgets"), depth + 1);
        }
    };
    for (const auto& group : groups.items())
    {
        if (!group.value().is_object()) throw std::invalid_argument("Invalid page group");
        if (group.value().contains("widgets") && !group.value()["widgets"].is_null()) walk(walk, group.value().at("widgets"), 0);
    }
}
std::vector<Profile> readProfiles(const Json& value, ProfileDiagnostics& diagnostic)
{
    std::vector<Profile> result;
    std::set<std::string> seen;
    diagnostic.stage = ProfileParse::Page;
    const auto& groups = pageGroups(value);
    diagnostic.stage = ProfileParse::Widgets;
    visitWidgets(groups, [&](const Json& widget)
    {
        ++diagnostic.widgets;
        if (widget.value("type", "") != "PLAYER_SELECTION_TILE") return;
        ++diagnostic.tiles;
        diagnostic.stage = ProfileParse::Presentation;
        const auto data = presentation(widget);
        if (!data.is_object()) throw std::invalid_argument("Invalid profile presentation");
        diagnostic.stage = ProfileParse::Actions;
        const auto& actions = widget.at("actions");
        if (!actions.is_array()) throw std::invalid_argument("Invalid profile actions");
        std::optional<std::string> selectedId;
        for (const auto& action : actions)
            if (action.value("target", "") == "PLAYER_SELECT")
            {
                // Public parseAction (module 46439) decodes action.payload,
                // independently of the widget's presentationData JSON.
                const auto& rawPayload = action.at("payload");
                if (rawPayload.is_string()) ++diagnostic.encodedActions;
                const auto payload = rawPayload.is_string() ? parse(rawPayload.get<std::string>(), MaxAuthResponse) : rawPayload;
                if (!payload.is_object()) throw std::invalid_argument("Invalid profile action payload");
                diagnostic.stage = ProfileParse::Identity;
                auto id = text(payload, "profileId", 512);
                if (selectedId && *selectedId != id)
                    throw std::invalid_argument("Profile action mismatch");
                selectedId = std::move(id);
                diagnostic.stage = ProfileParse::Actions;
            }
        diagnostic.stage = ProfileParse::Identity;
        std::optional<std::string> displayId;
        if (data.contains("profileId") && !data["profileId"].is_null() && data["profileId"] != "")
            displayId = text(data, "profileId", 512);
        if (selectedId && displayId && *selectedId != *displayId)
            throw std::invalid_argument("Profile presentation/action mismatch");
        // SelectProfile reads its ID from the action. Older/unlocked tiles may
        // omit the presentation ID, which is also used by the PIN UI.
        if (!selectedId && !displayId) { diagnostic.stage = ProfileParse::Widgets; return; }
        diagnostic.stage = ProfileParse::Name;
        Profile profile{selectedId ? *selectedId : *displayId, displayText(data, "primaryText"),
            selectedId ? ProfileBlock::None : ProfileBlock::ImportRequired};
        if (data.contains("iconUrl") && data["iconUrl"].is_string() && launcher::artworkUrl(data["iconUrl"].get<std::string>()))
            profile.artwork = data["iconUrl"].get<std::string>();
        diagnostic.stage = ProfileParse::Flags;
        if (flag(data, "sourcePinExists") || flag(data, "isLocked")) profile.block = ProfileBlock::PinRequired;
        if (data.contains("migrationStatus") && !data["migrationStatus"].is_null() && data["migrationStatus"] != "" &&
            data["migrationStatus"] != false && data["migrationStatus"] != 0)
            profile.block = ProfileBlock::Migration;
        if (flag(data, "isHouseholdMismatch")) profile.block = ProfileBlock::NotInFamily;
        diagnostic.stage = ProfileParse::Identity;
        if (!seen.insert(profile.id).second) throw std::invalid_argument("Duplicate profiles");
        diagnostic.stage = ProfileParse::Limit;
        if (result.size() >= 16) throw std::invalid_argument("Excessive profiles");
        result.push_back(std::move(profile));
        ++diagnostic.parsed;
        diagnostic.stage = ProfileParse::Widgets;
    });
    diagnostic.stage = ProfileParse::NoProfiles;
    if (result.empty()) throw std::invalid_argument("No supported profile tiles");
    diagnostic.stage = ProfileParse::Complete;
    return result;
}
struct ParsedGame { Game display; std::string serviceToken, path; };
std::string pagePath(const Json& action)
{
    if (!action.contains("target") || !action["target"].is_string()) return {};
    auto path = action["target"].get<std::string>();
    // Only relative PDS navigation. The HTTP destination stays the fixed proxy;
    // URL schemes, query strings, escapes and traversal are not interpreted.
    if (path.empty() || path.size() > 1024 || path.find('/') == std::string::npos ||
        path.find("//") != std::string::npos ||
        path.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/_-") != std::string::npos)
        return {};
    if (path.front() == '/') path.erase(0, 1);
    if (!path.empty() && path.back() == '/') path.pop_back();
    if (path.size() > 8 && path.compare(path.size() - 8, 8, "/default") == 0) path.resize(path.size() - 8);
    return path;
}
Json actionPayload(const Json& action)
{
    const auto& raw = action.at("payload");
    const auto result = raw.is_string() ? parse(raw.get<std::string>(), MaxAuthResponse) : raw;
    if (!result.is_object()) throw std::invalid_argument("Invalid action payload");
    return result;
}
bool nonBlockingPageAction(const std::string& target)
{
    // DialogSurveyGame (35773) renders only when the browser's post-game
    // shouldShowSurvey flag is set. This probe never starts a game session.
    if (target == "DIALOG_SURVEY_GAME") return true;
    // Observed service actions absent from ActionMapper.UA (79466): the web
    // PageLevelActions component (89256) explicitly filters both of them out.
    // Keep this an exact list; unknown/account/policy actions remain blocked.
    if (target == "DIALOG_CONTROLLER_UNRECOGNIZED" || target == "SOFTWARE_DECODE") return true;
    return false;
}
LaunchAction primaryActionKind(const std::string& target)
{
    if (target == "GAME_SESSION_ASYNC") return LaunchAction::GameSession;
    if (target == "DIRECT_LAUNCH") return LaunchAction::DirectLaunch;
    if (target == "OS_UNSUPPORTED") return LaunchAction::UnsupportedOs;
    if (target == "EXPERIMENTAL_CLIENT") return LaunchAction::ExperimentalClient;
    return LaunchAction::Other;
}
std::string detailText(const Json& value, const char* key, std::size_t limit = 2048)
{
    if (!value.contains(key) || value[key].is_null() || value[key] == "") return {};
    auto result = value.at(key).get<std::string>();
    if (result.size() > limit) throw std::invalid_argument("Detail text limit");
    for (auto& c : result)
    {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            throw std::invalid_argument("Invalid detail text");
    }
    return result;
}
bool mediaPresent(const Json& value, const char* key)
{
    if (!value.contains(key) || value[key].is_null() || value[key] == "") return false;
    const auto url = text(value, key, 4096);
    return url.compare(0, 8, "https://") == 0;
}
std::string artwork(const Json& data, const char* key)
{
    // Optional presentation media must never invalidate otherwise valid
    // account, navigation or launch data.
    if (!data.contains(key) || !data[key].is_string()) return {};
    const auto url = data[key].get<std::string>();
    return launcher::artworkUrl(url) ? url : std::string();
}
std::vector<ParsedGame> readGames(const Json& value, CatalogDiagnostics& diagnostic, std::size_t limit = 256)
{
    std::vector<ParsedGame> result;
    diagnostic.stage = CatalogParse::Page;
    const auto& groups = pageGroups(value);
    diagnostic.stage = CatalogParse::Widgets;
    visitWidgets(groups, [&](const Json& widget)
    {
        ++diagnostic.widgets;
        const auto type = widget.value("type", "");
        // WidgetMapper (57957) maps all four names to the same game-tile
        // component (76609), with the same gameId/title presentation fields.
        if (type != "GAME_TILE" && type != "GAMEPLAY_TILE" && type != "GAME_TILE_HORIZONTAL" && type != "GAME_TILE_VERTICAL") return;
        ++diagnostic.tiles;
        if (type == "GAME_TILE_HORIZONTAL") ++diagnostic.horizontal;
        if (type == "GAME_TILE_VERTICAL") ++diagnostic.vertical;
        diagnostic.stage = CatalogParse::Presentation;
        const auto data = presentation(widget);
        diagnostic.stage = CatalogParse::Identity;
        auto id = text(data, "gameId", 512);
        diagnostic.stage = CatalogParse::Title;
        ParsedGame game{{std::move(id), displayText(data, "title"), false, {}}, {}, {}};
        // GameTile (76609) uses these source-defined presentation properties.
        game.display.artwork = artwork(data, "imageLandscape");
        if (game.display.artwork.empty()) game.display.artwork = artwork(data, "imagePortrait");
        // The browser navigates with actions[0].target and caches its
        // serviceToken. Later actions are not substitutes for a gated first one.
        if (widget.contains("actions") && widget["actions"].is_array() && !widget["actions"].empty())
        {
            const auto& action = widget["actions"][0];
            game.path = pagePath(action);
            if (!game.path.empty() && action.contains("serviceToken") && !action["serviceToken"].is_null() && action["serviceToken"] != "")
                game.serviceToken = text(action, "serviceToken", 32768);
        }
        game.display.detailsAvailable = !game.path.empty();
        const auto existing = std::find_if(result.begin(), result.end(), [&](const ParsedGame& old) { return old.display.id == game.display.id; });
        if (existing == result.end())
        {
            diagnostic.stage = CatalogParse::Limit;
            if (result.size() >= limit) throw std::invalid_argument("Game tile limit");
            result.push_back(std::move(game));
            ++diagnostic.parsed;
        }
        else if (!existing->display.detailsAvailable && game.display.detailsAvailable)
        {
            existing->display.detailsAvailable = true;
            existing->serviceToken = std::move(game.serviceToken);
            existing->path = std::move(game.path);
        }
        diagnostic.stage = CatalogParse::Widgets;
    });
    diagnostic.stage = CatalogParse::Complete;
    return result;
}
std::optional<Json> catalogFollowup(const Json& value, const std::string& navigationQuery, CatalogDiagnostics& diagnostic)
{
    diagnostic.stage=CatalogParse::Page;
    std::optional<Json> result;
    const Json main = {{"mainContent", pageGroups(value).at("mainContent")}};
    visitWidgets(main, [&](const Json& widget)
    {
        const auto type = widget.value("type", "");
        if (type != "BROWSE_PILL_FILTER_ROW_WITH_RESULTS" && type != "GRID_FILTER_ROW") return;
        diagnostic.filterWidget=true;
        diagnostic.stage=CatalogParse::BrowseContext;
        if (result) throw std::invalid_argument("Ambiguous catalog context");
        Json context = {{"sort", "TITLE_A_TO_Z"}, {"filtersList", Json::array()}};
        std::string pageType = "landing_library_unfiltered";
        if (type == "BROWSE_PILL_FILTER_ROW_WITH_RESULTS")
        {
            // BrowsePillFilterRowWithResults (97917) calls useFilters (80098)
            // with these server-supplied defaults. Empty free-text search is
            // a different screen and can return only suggested games.
            const auto data = presentation(widget);
            diagnostic.defaultSearchOnLoad=flag(data, "defaultSearchOnLoad");
            diagnostic.stage=CatalogParse::BrowseResults;
            pageType = text(data, "resultsPageType", 80);
            if (pageType != "multistate_browse_results") throw std::invalid_argument("Unknown catalog results page");
            diagnostic.stage=CatalogParse::BrowseQuery;
            std::string query=navigationQuery;
            if (query.empty() && data.contains("defaultSearch") && !data["defaultSearch"].is_null() && data["defaultSearch"]!="")
                query=text(data,"defaultSearch",512);
            if (query.empty() && widget.contains("widgets") && !widget["widgets"].is_null())
            {
                // QUICK_SEARCH (94070) puts payload.type into quick_search.
                // Catalog is an explicit selection: use the declared default
                // or first offered non-channel preset even when the website
                // elects not to run that default automatically on page load.
                visitWidgets(Json{{"presets",{{"widgets",widget["widgets"]}}}},[&](const Json& preset)
                {
                    if (!query.empty() || !preset.contains("actions") || !preset["actions"].is_array() || preset["actions"].empty()) return;
                    const auto& action=preset["actions"][0];
                    if (action.value("target","")!="QUICK_SEARCH") return;
                    const auto payload=actionPayload(action);
                    if (!flag(payload,"isChannelDetailQuickSearch")) query=text(payload,"type",512);
                });
            }
            if (query.empty()) throw std::invalid_argument("Catalog has no offered default or preset");
            context["query"] = std::move(query);
            diagnostic.stage=CatalogParse::BrowseSort;
            if (data.contains("defaultSort") && !data["defaultSort"].is_null())
            {
                const auto sort = text(data, "defaultSort", 32);
                if (sort != "TITLE_A_TO_Z" && sort != "TITLE_Z_TO_A" && sort != "RELEVANCE")
                    throw std::invalid_argument("Unknown catalog sort");
                context["sort"] = sort;
            }
        }
        // GridFilterRow (63526) uses the unfiltered library page when no URL
        // filters are selected. Neither browser flow sends search pagination;
        // its result grid scrolls through the returned widgets locally.
        result = Json{{"featureScheme", "WEB_V1"},
            {"pageContext", {{"pageId", "default"}, {"pageType", pageType}}}, {"searchContext", context}};
    });
    return result;
}
std::pair<std::string,std::string> catalogNavigation(const Json& page)
{
    std::pair<std::string,std::string> route;
    visitWidgets(pageGroups(page),[&](const Json& widget)
    {
        if (!widget.contains("actions") || !widget["actions"].is_array() || widget["actions"].empty()) return;
        const auto& action=widget["actions"][0];
        if (pagePath(action)!="browse" && action.value("target", "")!="browse") return;
        std::pair<std::string,std::string> offered;
        if (action.contains("serviceToken") && !action["serviceToken"].is_null() && action["serviceToken"]!="")
            offered.first=text(action,"serviceToken",32768);
        if (action.contains("payload") && !action["payload"].is_null())
        {
            const auto& raw=action["payload"];
            const auto payload=raw.is_string()?parse(raw.get<std::string>(),MaxAuthResponse):raw;
            if (payload.contains("quickSearch") && !payload["quickSearch"].is_null() && payload["quickSearch"]!="")
                offered.second=text(payload,"quickSearch",512);
        }
        if (route!=std::pair<std::string,std::string>{} && route!=offered)
            throw std::invalid_argument("Ambiguous browse navigation");
        route=std::move(offered);
    });
    return route;
}
}

std::size_t responseLimit(Step step)
{
    return step == Step::LunaPage || step == Step::AppConfig || step == Step::ProfilesPage || step == Step::Catalog ||
        step == Step::DetailPage || step == Step::LaunchWidget || step == Step::ExperimentalPage || step == Step::NativeNotice || step == Step::ProfileManagement || browseRead(step) ?
        MaxLunaResponse : MaxAuthResponse;
}
const char* detailParseLabel(DetailParse stage)
{
    switch (stage)
    {
    case DetailParse::None: return "none";
    case DetailParse::Json: return "json";
    case DetailParse::Page: return "page";
    case DetailParse::Widgets: return "widgets";
    case DetailParse::Presentation: return "presentation";
    case DetailParse::Actions: return "actions";
    case DetailParse::LaunchPayload: return "launch_payload";
    case DetailParse::Complete: return "complete";
    }
    return "unknown";
}
const char* launchActionLabel(LaunchAction action)
{
    switch (action)
    {
    case LaunchAction::None: return "none";
    case LaunchAction::GameSession: return "game_session_async";
    case LaunchAction::DirectLaunch: return "direct_launch";
    case LaunchAction::Other: return "unsupported_action";
    case LaunchAction::Ambiguous: return "ambiguous_buttons";
    case LaunchAction::PageAction: return "page_action_required";
    case LaunchAction::UnsupportedOs: return "os_unsupported";
    case LaunchAction::ExperimentalClient: return "experimental_client";
    case LaunchAction::AcceptEula: return "accept_eula";
    }
    return "unknown";
}
const char* catalogParseLabel(CatalogParse stage)
{
    switch (stage)
    {
    case CatalogParse::None: return "none";
    case CatalogParse::Json: return "json";
    case CatalogParse::Page: return "page";
    case CatalogParse::Widgets: return "widgets";
    case CatalogParse::Presentation: return "presentation";
    case CatalogParse::Identity: return "identity";
    case CatalogParse::Title: return "title";
    case CatalogParse::Limit: return "limit";
    case CatalogParse::Complete: return "complete";
    case CatalogParse::BrowseContext: return "browse_context";
    case CatalogParse::BrowseQuery: return "browse_query";
    case CatalogParse::BrowseSort: return "browse_sort";
    case CatalogParse::BrowseResults: return "browse_results_type";
    case CatalogParse::Access: return "access";
    }
    return "unknown";
}
const char* profileParseLabel(ProfileParse stage)
{
    switch (stage)
    {
    case ProfileParse::None: return "none";
    case ProfileParse::Json: return "json";
    case ProfileParse::Page: return "page";
    case ProfileParse::Widgets: return "widgets";
    case ProfileParse::Presentation: return "presentation";
    case ProfileParse::Actions: return "actions";
    case ProfileParse::Identity: return "identity";
    case ProfileParse::Name: return "name";
    case ProfileParse::Flags: return "flags";
    case ProfileParse::Limit: return "limit";
    case ProfileParse::NoProfiles: return "no_profiles";
    case ProfileParse::Complete: return "complete";
    }
    return "unknown";
}
const char* authErrorLabel(AuthError error)
{
    switch (error)
    {
    case AuthError::None: return "none";
    case AuthError::Unknown: return "unknown";
    case AuthError::InvalidValue: return "invalid_value";
    case AuthError::MissingValue: return "missing_value";
    case AuthError::InvalidRequest: return "invalid_request";
    case AuthError::InvalidClient: return "invalid_client";
    case AuthError::UnauthorizedClient: return "unauthorized_client";
    case AuthError::Unauthorized: return "unauthorized";
    case AuthError::AccessDenied: return "access_denied";
    case AuthError::InvalidGrant: return "invalid_grant";
    case AuthError::InvalidCodePair: return "invalid_code_pair";
    case AuthError::ExpiredCodePair: return "expired_code_pair";
    case AuthError::InvalidData: return "invalid_data";
    case AuthError::ServerError: return "server_error";
    case AuthError::SlowDown: return "slow_down";
    case AuthError::InvalidToken: return "invalid_token";
    case AuthError::DuplicateDeviceName: return "duplicate_device_name";
    }
    return "unknown";
}
const char* authResponseLabel(AuthResponse response)
{
    switch (response)
    {
    case AuthResponse::None: return "none";
    case AuthResponse::Empty: return "empty";
    case AuthResponse::Unparseable: return "unparseable";
    case AuthResponse::NoErrorField: return "no_error_field";
    case AuthResponse::TopLevelError: return "top_level_error";
    case AuthResponse::NestedError: return "nested_error";
    case AuthResponse::TooLarge: return "too_large";
    }
    return "unknown";
}
const char* authParameterLabel(AuthParameter parameter)
{
    switch (parameter)
    {
    case AuthParameter::None: return "none";
    case AuthParameter::Unknown: return "unknown";
    case AuthParameter::CodePair: return "code_pair";
    case AuthParameter::PublicCode: return "public_code";
    case AuthParameter::PrivateCode: return "private_code";
    case AuthParameter::RegistrationData: return "registration_data";
    case AuthParameter::DeviceSerial: return "device_serial";
    case AuthParameter::DeviceType: return "device_type";
    case AuthParameter::SourceToken: return "source_token";
    }
    return "unknown";
}

NativeAuthProbe::~NativeAuthProbe() { clearSecrets(); wipe(localAuthErrorIdentifier_); }
void NativeAuthProbe::clearNoticeActions()
{
    for (auto& action : noticeActions_) wipe(action);
    noticeActions_.clear();
}
void NativeAuthProbe::clearDetail()
{
    wipe(launchWidgetToken_); wipe(launchGameId_); wipe(playToken_); wipe(productStage_); wipe(launchPayload_);
    wipe(experimentalPageToken_); clearNoticeActions(); notice_ = {}; status_.noticeParsing = {};
    detail_ = {}; detailLoaded_ = false; selectedGame_.reset();
    wipe(detailPath_);
    status_.detailParsing = {};
}
void NativeAuthProbe::clearGames()
{
    clearBrowseCandidates(); browseRawIds_.clear();
    clearDetail();
    for (auto& link : gameLinks_) { wipe(link.serviceToken); wipe(link.path); }
    gameLinks_.clear(); games_.clear();
    for (auto& link : homeLinks_) { wipe(link.serviceToken); wipe(link.path); }
    homeLinks_.clear(); homeGames_.clear();
    wipe(browsing_.query); browsing_ = {}; wipe(requestedQuery_); wipe(browseRequest_); requestedOffset_ = 0;
    wipe(browsePageToken_); wipe(browseQuickSearch_);
}
void NativeAuthProbe::failBrowse(Error error, long httpStatus)
{
    clearBrowseCandidates();
    browsing_.error = error;
    status_.httpStatus = httpStatus;
    status_.step = Step::CatalogReady;
    pending_.reset(); deadline_ = sessionDeadline_;
}
void NativeAuthProbe::clearBrowseCandidates()
{
    for (auto& link:browseCandidateLinks_) { wipe(link.serviceToken); wipe(link.path); }
    browseCandidateLinks_.clear(); browseCandidates_.clear(); browseIncluded_.clear(); accessOffset_=0; accessBatchSize_=25;
    wipe(browseAccessStage_);
}
void NativeAuthProbe::finishBrowse()
{
    for (auto& link:gameLinks_) { wipe(link.serviceToken); wipe(link.path); }
    games_.clear(); gameLinks_.clear(); browseRawIds_.clear();
    for (std::size_t i=0;i<browseCandidates_.size();++i)
    {
        browseRawIds_.push_back(browseCandidates_[i].id);
        if (requestedIncludedOnly_ && !browseIncluded_[i]) continue;
        games_.push_back(std::move(browseCandidates_[i])); gameLinks_.push_back(std::move(browseCandidateLinks_[i]));
    }
    clearBrowseCandidates();
    browsing_.query=requestedQuery_; browsing_.offset=requestedOffset_; browsing_.includedOnly=requestedIncludedOnly_;
    browsing_.hasNext=candidateHasNext_; browsing_.error=Error::None;
    status_.step=Step::CatalogReady; deadline_=sessionDeadline_;
}
void NativeAuthProbe::clearSecrets()
{
    wipe(privateCode_); wipe(accessToken_); wipe(refreshToken_); wipe(profileToken_); wipe(clientTdi_);
    status_.clientIdentityReady = false;
    status_.publicCode.clear();
    profiles_.clear(); clearGames(); selectedProfile_.reset();
    pending_.reset();
}
void NativeAuthProbe::failDetail(Error error, long httpStatus)
{
    wipe(launchWidgetToken_); wipe(launchGameId_); wipe(playToken_); wipe(productStage_); wipe(launchPayload_);
    clearNoticeActions();
    status_.noticeParsing.complete = status_.noticeParsing.accepted = false;
    if (!detailLoaded_) detail_ = {};
    status_.detailParsing.error = error;
    status_.detailParsing.hasGameId = status_.detailParsing.hasPlayToken = false;
    status_.httpStatus = httpStatus;
    status_.step = detailLoaded_ ? Step::DetailReady : Step::CatalogReady;
    pending_.reset();
    deadline_ = sessionDeadline_;
}
void NativeAuthProbe::acceptLaunchPayload(const std::string& payload)
{
    status_.detailParsing.stage = DetailParse::LaunchPayload;
    const auto value = parse(payload, MaxAuthResponse);
    launchGameId_ = text(value, "gameId", 512);
    playToken_ = text(value, "playToken", 32768);
    if (value.contains("productStage") && !value["productStage"].is_null() && value["productStage"] != "")
        productStage_ = text(value, "productStage", 128);
    status_.detailParsing.hasGameId = status_.detailParsing.hasPlayToken = true;
    // The session coordinator checks preflight requirements before allocation.
    wipe(launchPayload_);
    launchPayload_ = payload;
}
std::string NativeAuthProbe::localActionTarget(const std::string& target) const
{
    // These are protocol identifiers for local diagnosis, never terminal logs.
    // Reject arbitrary text/paths and anything containing a known credential.
    if (target.empty() || target.size() > 80 || target.front() < 'A' || target.front() > 'Z' ||
        target.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
        return "UNKNOWN_ACTION";
    for (const auto* secret : {&accessToken_, &refreshToken_, &profileToken_, &privateCode_, &status_.publicCode,
        &serial_, &launchWidgetToken_, &playToken_, &experimentalPageToken_, &clientTdi_})
        if (!secret->empty() && target.find(*secret) != std::string::npos) return "UNKNOWN_ACTION";
    for (const auto& link : gameLinks_)
        if (!link.serviceToken.empty() && target.find(link.serviceToken) != std::string::npos) return "UNKNOWN_ACTION";
    return target;
}
void NativeAuthProbe::acceptExperimentalPage(const std::string& body)
{
    readNoticePage(body, notice_, status_.noticeParsing, noticeActions_);
}
const char* noticeIssueLabel(NoticeIssue issue)
{
    switch (issue)
    {
    case NoticeIssue::None: return "none";
    case NoticeIssue::Widget: return "widget";
    case NoticeIssue::Checkbox: return "checkbox";
    case NoticeIssue::LegalTerms: return "legal_terms";
    case NoticeIssue::SecondaryAction: return "secondary_action";
    }
    return "unknown";
}
const char* noticeActionLabel(NoticeAction action)
{
    switch (action)
    {
    case NoticeAction::None: return "none";
    case NoticeAction::Unknown: return "unknown";
    case NoticeAction::Dismiss: return "DIALOG_DISMISS";
    case NoticeAction::GameSession: return "GAME_SESSION_ASYNC";
    case NoticeAction::DirectLaunch: return "DIRECT_LAUNCH";
    case NoticeAction::LocalStorageToggle: return "LOCAL_STORAGE_TOGGLE";
    case NoticeAction::DoNotShowAgain: return "DO_NOT_SHOW_AGAIN";
    }
    return "unknown";
}
void NativeAuthProbe::readNoticePage(const std::string& body, ExperimentalNotice& notice,
    NoticeDiagnostics& diagnostic, std::vector<std::string>& savedActions, bool softwareWarning, bool eula)
{
    for (auto& action : savedActions) wipe(action);
    savedActions.clear(); notice = {}; diagnostic = {};
    const auto unsupported = [&](NoticeIssue issue)
    {
        ++diagnostic.unsupported;
        if (diagnostic.firstIssue == NoticeIssue::None) diagnostic.firstIssue = issue;
    };
    diagnostic.stage = DetailParse::Json;
    const auto value = parse(body, MaxLunaResponse);
    diagnostic.stage = DetailParse::Page;
    const auto& groups = pageGroups(value);
    if (value.contains("actions") && !value["actions"].is_null())
    {
        const auto& actions = value["actions"];
        if (!actions.is_array() || actions.size() > 16) throw std::invalid_argument("Invalid notice actions");
        for (const auto& action : actions)
            if (!nonBlockingPageAction(action.value("target", ""))) ++diagnostic.blockingActions;
    }
    const auto append = [&](const Json& data, const char* key)
    {
        const auto raw = detailText(data, key, eula ? 131072 : 8192);
        if (raw.empty()) return;
        const auto text = eula ? launcher::consentText(raw) : raw;
        if (text.empty() || notice.text.size() + text.size() + 2 > (eula ? 131072u : 16384u))
            throw std::invalid_argument("Notice text limit");
        notice.text += text + "\n\n";
        ++diagnostic.textBlocks;
    };
    // WidgetsPage renders mainContent for a modal. Other page members can be
    // global navigation/SEO content and do not belong to the warning.
    const Json main = {{"mainContent", groups.at("mainContent")}};
    diagnostic.stage = DetailParse::Widgets;
    visitWidgets(main, [&](const Json& widget)
    {
        ++diagnostic.widgets;
        const auto type = widget.value("type", "");
        if (type == "COLLECTION" || type == "DIALOG_BUTTON_CAROUSEL" || type == "BASIC_BUTTON_CAROUSEL") return;
        const bool dialog = type == "BASIC_DIALOG" || type == "BASIC_DIALOG_TITLE" || type == "DIALOG_INFO";
        const bool button = type == "DIALOG_BUTTON" || type == "DIALOG_PRIMARY_BUTTON" || type == "BASIC_BUTTON" ||
            type == "BASIC_BUTTON_PRIMARY" || type == "BASIC_BUTTON_PRIMARY_SINGLE_LINE" ||
            type == "BASIC_BUTTON_SECONDARY" || type == "BASIC_BUTTON_SECONDARY_SINGLE_LINE" || type == "TEXT_BUTTON";
        if (!dialog && !button)
        {
            unsupported(NoticeIssue::Widget);
            ++diagnostic.unsupportedWidgets;
            if (notice.unsupportedWidgetTypes.size() < 8) notice.unsupportedWidgetTypes.push_back(localActionTarget(type));
            return;
        }
        diagnostic.stage = DetailParse::Presentation;
        const auto data = presentation(widget);
        if (!data.is_object()) throw std::invalid_argument("Invalid notice presentation");
        if (dialog)
        {
            if (type == "BASIC_DIALOG")
            {
                append(data, detailText(data, "title").empty() ? "primaryText" : "title");
                append(data, detailText(data, "description", eula ? 131072 : 2048).empty() ? "secondaryText" : "description");
                append(data, "tertiaryText");
                if (data.contains("numberedList") && !data["numberedList"].is_null())
                {
                    const auto& items = data["numberedList"];
                    if (!items.is_array() || items.size() > 16) throw std::invalid_argument("Notice list limit");
                    for (const auto& item : items) append(Json{{"text", item}}, "text");
                }
                const auto checkboxText = detailText(data, "checkboxText");
                const bool checkbox = !checkboxText.empty();
                const bool legal = data.contains("legalTerms") && !data["legalTerms"].is_null() && !data["legalTerms"].empty();
                bool optionalPreference = false;
                if (checkbox)
                {
                    ++diagnostic.checkboxes;
                    // BasicDialog (79903) invokes action zero only when the
                    // checkbox changes. An unchecked optional warning preference
                    // requires no action. Never infer consent from its label.
                    try
                    {
                        const auto& actions = widget.at("actions");
                        if (actions.is_array() && !actions.empty() && actions[0].is_object())
                        {
                            const auto target = actions[0].value("target", "");
                            if (diagnostic.checkboxes == 1)
                                diagnostic.checkboxAction = target == "DO_NOT_SHOW_AGAIN" ? NoticeAction::DoNotShowAgain :
                                    target == "LOCAL_STORAGE_TOGGLE" ? NoticeAction::LocalStorageToggle :
                                    target == "GAME_SESSION_ASYNC" ? NoticeAction::GameSession : NoticeAction::Unknown;
                            if (softwareWarning && diagnostic.checkboxes == 1 && actions.size() == 1 && target == "DO_NOT_SHOW_AGAIN")
                            {
                                const auto payload = actionPayload(actions[0]);
                                // DoNotShowAgain (26604) only stores modalId in
                                // the local suppression list (95167). Scope this
                                // exception to the software_decode modal (532).
                                optionalPreference = text(payload, "modalId", 128) == "software_decode" &&
                                    !flag(payload, "shouldExecuteActionOnLoad");
                                flag(payload, "shouldDismissDialog"); // Validate optional boolean metadata.
                            }
                        }
                    }
                    catch (...) { optionalPreference = false; }
                    diagnostic.checkboxPreferenceSupported = diagnostic.checkboxes == 1 && optionalPreference;
                    const auto rendered = "[ ] " + checkboxText + (optionalPreference ?
                        "\nPreference left unchecked; warning preferences are not saved in this alpha." :
                        "\nCheckbox action is unsupported; continuation remains disabled.");
                    if (notice.text.size() + rendered.size() + 2 > 16384) throw std::invalid_argument("Notice text limit");
                    notice.text += rendered + "\n\n";
                    ++diagnostic.textBlocks;
                }
                if (legal) { ++diagnostic.legalTerms; if (eula) append(data, "legalTerms"); }
                if ((checkbox && !optionalPreference) || (legal && !eula))
                    unsupported(checkbox && !optionalPreference ? NoticeIssue::Checkbox : NoticeIssue::LegalTerms);
            }
            else
            {
                append(data, "primaryText");
                if (type == "DIALOG_INFO") append(data, "secondaryText");
            }
        }
        else
        {
            if (notice.choices.size() >= 8) throw std::invalid_argument("Notice button limit");
            auto label = detailText(data, "primaryText", 256);
            if (label.empty()) label = detailText(data, "text", 256);
            if (label.empty()) throw std::invalid_argument("Missing notice button label");
            diagnostic.stage = DetailParse::Actions;
            const auto& actions = widget.at("actions");
            if (!actions.is_array()) throw std::invalid_argument("Invalid notice button actions");
            const auto first = actions.empty() ? Json::object() : actions[0];
            const auto target = first.value("target", "");
            const auto kind = eula && target == "ACCEPT_EULA" ? LaunchAction::AcceptEula :
                actions.empty() ? LaunchAction::None : primaryActionKind(target);
            // A continuation may also dismiss its warning. Unknown secondary
            // actions could require more work; never silently discard them.
            for (std::size_t i = 1; i < actions.size(); ++i)
            {
                const auto secondary = actions[i].value("target", "");
                if (secondary == "DIALOG_DISMISS") continue;
                unsupported(NoticeIssue::SecondaryAction);
                ++diagnostic.secondaryActions;
                if (!diagnostic.firstIssueButton)
                {
                    diagnostic.firstIssueButton = diagnostic.buttons + 1;
                    diagnostic.firstIssueAction = i + 1;
                    // Only fixed public protocol labels can enter logs.
                    diagnostic.firstUnsupportedAction = secondary == "LOCAL_STORAGE_TOGGLE" ? NoticeAction::LocalStorageToggle :
                        secondary == "GAME_SESSION_ASYNC" ? NoticeAction::GameSession :
                        secondary == "DIRECT_LAUNCH" ? NoticeAction::DirectLaunch : NoticeAction::Unknown;
                }
                if (notice.unsupportedActionTargets.size() < 8)
                    notice.unsupportedActionTargets.push_back("Button " + std::to_string(diagnostic.buttons + 1) +
                        ", action " + std::to_string(i + 1) + ": " + localActionTarget(secondary));
            }
            notice.choices.push_back({std::move(label), localActionTarget(target), kind});
            auto serialized = kind == LaunchAction::GameSession || kind == LaunchAction::DirectLaunch ||
                kind == LaunchAction::AcceptEula ? first.dump() : "{}";
            if (serialized.size() > MaxAuthResponse) throw std::invalid_argument("Notice action limit");
            savedActions.push_back(std::move(serialized));
            ++diagnostic.buttons;
        }
        diagnostic.stage = DetailParse::Widgets;
    });
    if (eula && notice.text.find("[http") != std::string::npos)
        notice.text += "Open the linked addresses on another device to review those documents before accepting.\n\n";
    diagnostic.complete = diagnostic.textBlocks > 0 && diagnostic.unsupported == 0 && diagnostic.blockingActions == 0;
    diagnostic.stage = DetailParse::Complete;
}
void NativeAuthProbe::acceptDetail(const std::string& body)
{
    auto& diagnostic = status_.detailParsing;
    diagnostic.stage = DetailParse::Json;
    const auto value = parse(body, MaxLunaResponse);
    diagnostic.stage = DetailParse::Page;
    const auto& groups = pageGroups(value);
    Json primary;
    std::string fullDescription;
    const auto optionalDescription = [](const Json& data, const char* key)
    {
        if (!data.contains(key) || !data[key].is_string()) return std::string();
        return launcher::descriptionText(data[key].get<std::string>());
    };
    const auto addTrailer = [&](const Json& data)
    {
        if (!data.contains("videoUrl") || !data["videoUrl"].is_string()) return;
        const auto url = data["videoUrl"].get<std::string>();
        if (launcher::trailerUrl(url) && detail_.trailers.size() < 8 &&
            std::find(detail_.trailers.begin(), detail_.trailers.end(), url) == detail_.trailers.end())
            detail_.trailers.push_back(url);
    };
    diagnostic.stage = DetailParse::Widgets;
    visitWidgets(groups, [&](const Json& widget)
    {
        ++diagnostic.widgets;
        const auto type = widget.value("type", "");
        if (type == "PRODUCT_DETAIL")
        {
            ++diagnostic.products;
            if (diagnostic.products > 1) throw std::invalid_argument("Ambiguous product detail");
            diagnostic.stage = DetailParse::Presentation;
            const auto data = presentation(widget);
            if (!data.is_object()) throw std::invalid_argument("Invalid product presentation");
            const auto title = detailText(data, "title", 256);
            if (!title.empty()) detail_.title = title;
            detail_.description = optionalDescription(data, "browseDescription");
            if (detail_.description.empty()) detail_.description = optionalDescription(data, "subHeader");
            detail_.hasArtwork = mediaPresent(data, "backgroundImageLandscape") || mediaPresent(data, "productIcon");
            detail_.hasTrailer = mediaPresent(data, "videoUrl");
            addTrailer(data);
            detail_.artwork = artwork(data, "backgroundImageLandscape");
            if (detail_.artwork.empty()) detail_.artwork = artwork(data, "productIcon");
        }
        else if (type == "GAME_DESCRIPTION" || type == "GAME_SPECIFICATIONS" || type == "GAME_SPECIFICATIONS_V2")
        {
            try
            {
                const auto data = presentation(widget);
                auto description = optionalDescription(data, "gameDescriptionHtml");
                if (description.empty()) description = optionalDescription(data, "gameDescription");
                if (fullDescription.empty()) fullDescription = std::move(description);
            }
            catch (...) {} // Optional prose cannot block Play.
        }
        else if (type == "TRAILER_TILE")
        {
            try { addTrailer(presentation(widget)); } catch (...) {}
        }
        else if (type == "SCREENSHOT_TILE" && detail_.screenshots.size() < 12)
        {
            // ScreenshotTile (14599) renders presentationData.imageUrl.
            try
            {
                const auto url = artwork(presentation(widget), "imageUrl");
                if (!url.empty() && std::find(detail_.screenshots.begin(), detail_.screenshots.end(), url) == detail_.screenshots.end())
                    detail_.screenshots.push_back(url);
            }
            catch (...) {} // Optional media cannot authorize or block a launch.
        }
        else if (type == "PRODUCT_DETAIL_PRIMARY_BUTTON" || type == "PRODUCT_DETAIL_BUTTON")
        {
            diagnostic.stage = DetailParse::Presentation;
            const auto data = presentation(widget);
            if (!data.is_object()) throw std::invalid_argument("Invalid button presentation");
            if (type == "PRODUCT_DETAIL_PRIMARY_BUTTON" || data.value("buttonType", "") == "PRIMARY")
            {
                ++diagnostic.buttons;
                if (diagnostic.buttons == 1)
                {
                    detail_.buttonText = detailText(data, "primaryText", 256);
                    primary = widget;
                }
            }
        }
        diagnostic.stage = DetailParse::Widgets;
    });
    if (!fullDescription.empty()) detail_.description = std::move(fullDescription);
    detail_.hasTrailer = detail_.hasTrailer || !detail_.trailers.empty();
    if (!diagnostic.products) throw std::invalid_argument("Missing product detail");
    diagnostic.stage = DetailParse::Actions;
    // PageLevelActions (89256) mounts these separately from the primary button.
    // Only the exact source-inspected exceptions can be omitted.
    if (value.contains("actions") && !value["actions"].is_null())
    {
        const auto& actions = value["actions"];
        if (!actions.is_array() || actions.size() > 16) throw std::invalid_argument("Invalid page actions");
        for (const auto& action : actions)
        {
            ++diagnostic.pageActions;
            std::string target;
            if (action.is_object() && action.contains("target") && action["target"].is_string())
                target = action["target"].get<std::string>();
            const auto identifier = localActionTarget(target);
            detail_.pageActionTargets.push_back(identifier);
            const bool nonBlocking = identifier != "UNKNOWN_ACTION" && nonBlockingPageAction(target);
            if (!nonBlocking) ++diagnostic.blockingPageActions;
        }
    }
    Json primaryAction;
    if (diagnostic.buttons > 1) diagnostic.primaryAction = LaunchAction::Ambiguous;
    else if (diagnostic.buttons == 1)
    {
        const auto& actions = primary.at("actions");
        if (!actions.is_array()) throw std::invalid_argument("Invalid primary actions");
        if (!actions.empty())
        {
            primaryAction = actions[0];
            const auto target = primaryAction.value("target", "");
            detail_.primaryActionTarget = localActionTarget(target);
            diagnostic.primaryAction = primaryActionKind(target);
        }
    }
    diagnostic.action = diagnostic.blockingPageActions ? LaunchAction::PageAction : diagnostic.primaryAction;
    if (diagnostic.action == LaunchAction::GameSession)
    {
        auto payload = actionPayload(primaryAction).dump();
        acceptLaunchPayload(payload);
        wipe(payload);
    }
    else if (diagnostic.action == LaunchAction::DirectLaunch)
    {
        // DirectLaunch uses payload.serviceToken, unlike Navigate's top-level
        // action.serviceToken (public modules 79466/62999).
        launchWidgetToken_ = text(actionPayload(primaryAction), "serviceToken", 32768);
    }
    else if (diagnostic.action == LaunchAction::ExperimentalClient)
        experimentalPageToken_ = text(primaryAction, "serviceToken", 32768);
    detailLoaded_ = true;
    diagnostic.stage = DetailParse::Complete;
}
void NativeAuthProbe::fail(Error error, long httpStatus)
{
    const auto detailDiagnostic = status_.detailParsing;
    const auto noticeDiagnostic = status_.noticeParsing;
    status_.failedAt = status_.step;
    status_.step = Step::Failed;
    status_.error = error;
    status_.httpStatus = httpStatus;
    clearSecrets();
    status_.detailParsing = detailDiagnostic;
    status_.noticeParsing = noticeDiagnostic;
}
void NativeAuthProbe::start(std::string serial, std::int64_t nowMs)
{
    if (status_.step != Step::Idle || nowMs < 0 || serial.size() != 32 ||
        serial.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("Start one auth probe with a fresh device serial");
    serial_ = std::move(serial);
    status_.step = Step::CodePair;
    deadline_ = nowMs + 15 * 60 * 1000;
}
void NativeAuthProbe::restoreLogin(std::string serial, std::string refresh, std::int64_t nowMs)
{
    if (status_.step != Step::Idle || nowMs < 0 || serial.size() != 32 ||
        serial.find_first_not_of("0123456789abcdef") != std::string::npos || refresh.empty() ||
        refresh.size() > 32768 || std::any_of(refresh.begin(), refresh.end(),
            [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
    {
        wipe(refresh);
        throw std::invalid_argument("Invalid remembered login");
    }
    serial_ = std::move(serial);
    replaceRefreshToken(std::move(refresh));
    status_.registered = status_.restoredLogin = true;
    status_.step = Step::Refresh;
    deadline_ = nowMs + 60'000;
}
void NativeAuthProbe::replaceRefreshToken(std::string token)
{
    if (token != refreshToken_)
    {
        wipe(refreshToken_);
        refreshToken_ = std::move(token);
        ++credentialRevision_;
    }
    wipe(token);
}
std::optional<Request> NativeAuthProbe::nextRequest(std::int64_t nowMs)
{
    if (terminal(status_.step)) return std::nullopt;
    if (nowMs >= deadline_)
    {
        if (browseRead(status_.step) && nowMs < sessionDeadline_) failBrowse(Error::Transport);
        else fail(Error::Expired);
        return std::nullopt;
    }
    if (status_.step == Step::ChooseProfile || status_.step == Step::CatalogReady || status_.step == Step::DetailReady ||
        status_.step == Step::ExperimentalReady) return std::nullopt;
    if (pending_ || (status_.step == Step::Register && nowMs < nextPoll_)) return std::nullopt;
    Request request{nextId_++, status_.step, {}, {}, {"Content-Type: application/json", "Accept-Language: en_US"}};
    switch (status_.step)
    {
    case Step::ClientIdentity:
        request.url = std::string(ProxyOrigin) + "/getIotConnectionInfo";
        request.body = Json{{"deviceRole", "STREAMING_CLIENT"},
            {"iotFeatureScheme", "TEMPO_CLIENT_PARENT_V4_MIGRATION"}}.dump();
        request.headers[0] = "Content-Type: text/plain;charset=UTF-8";
        proxyHeaders(request, true);
        break;
    case Step::CatalogAccess:
    {
        request.url=std::string(ProxyOrigin)+"/getProductEntitlementStatuses";
        Json ids=Json::array();
        for (std::size_t i=accessOffset_;i<browseCandidates_.size()&&i<accessOffset_+accessBatchSize_;++i)
            ids.push_back(browseCandidates_[i].id);
        // Public request class 40902 and entitlement callers 1343/9999 use
        // productIdList plus the actual product stage. Retail page contexts
        // identify Release; an empty stage can fail server validation.
        request.body=Json{{"productIdList",ids},{"stage",browseAccessStage_}}.dump();
        request.headers[0]="Content-Type: text/plain;charset=UTF-8";
        proxyHeaders(request,true); break;
    }
    case Step::CodePair:
        request.url = std::string(AuthOrigin) + "/auth/create/codepair";
        request.body = Json{{"code_data", registration(serial_)}}.dump();
        break;
    case Step::Register:
        ++status_.approvalPolls;
        request.url = std::string(AuthOrigin) + "/auth/register";
        request.body = Json{{"auth_data", {{"code_pair", {{"public_code", status_.publicCode}, {"private_code", privateCode_}}}}},
            {"registration_data", registration(serial_)}, {"requested_token_type", {"bearer"}}}.dump();
        break;
    case Step::Refresh:
        request.url = std::string(AuthOrigin) + "/auth/token";
        request.body = Json{{"app_name", "Amazon Luna"}, {"app_version", "1.0.37340.0"},
            {"requested_token_type", "access_token"}, {"source_token", refreshToken_}, {"source_token_type", "refresh_token"}}.dump();
        break;
    case Step::LunaPage:
    case Step::NegativeControl:
    case Step::AppConfig:
    case Step::ProfilesPage:
    case Step::PlayerLogin:
    case Step::Catalog:
    case Step::SearchPage:
    case Step::BrowsePage:
    case Step::DetailPage:
    case Step::LaunchWidget:
    case Step::ExperimentalPage:
    {
        request.url = std::string(ProxyOrigin) + "/getPage";
        Json requestBody = {{"featureScheme", "WEB_V1"}, {"pageContext", {{"pageId", "default"}, {"pageUri", "home"}}}};
        if (status_.step == Step::AppConfig)
        {
            request.url = std::string(ProxyOrigin) + "/getAppConfig";
            requestBody = Json::object();
        }
        else if (status_.step == Step::ProfilesPage)
            requestBody = Json{{"featureScheme", "WEB_V1"},
                {"pageContext", {{"pageType", profilePageType_}, {"pageId", profilePageId_}}}};
        else if (status_.step == Step::PlayerLogin)
        {
            request.url = std::string(ProxyOrigin) + "/player/playerLogin";
            requestBody = Json{{"profileId", profiles_.at(*selectedProfile_).id}};
        }
        else if (status_.step == Step::DetailPage)
        {
            const PageLink savedPath{{}, detailPath_};
            const auto& link = selectedGame_ ? gameLinks_.at(*selectedGame_) : savedPath;
            requestBody = !link.serviceToken.empty() ?
                Json{{"featureScheme", "WEB_V1"}, {"serviceToken", link.serviceToken}} :
                Json{{"featureScheme", "WEB_V1"}, {"pageContext", {{"pageId", "default"}, {"pageUri", link.path}}}};
        }
        else if (status_.step == Step::BrowsePage)
        {
            requestBody["pageContext"]["pageUri"] = "browse";
            if (!browsePageToken_.empty()) requestBody=Json{{"featureScheme","WEB_V1"},{"serviceToken",browsePageToken_}};
        }
        else if (status_.step == Step::SearchPage)
        {
            // Public web chunk 7689/ProxyClientOrchestrator: searchContext
            // is a JSON body, not an invented query-string endpoint.
            requestBody = Json{{"featureScheme", "WEB_V1"},
                {"pageContext", {{"pageId", "default"}, {"pageType", "multistate_search_results"}}},
                {"searchContext", {{"query", requestedQuery_}, {"sort", "RELEVANCE"},
                    {"pagination", {{"startIndex", requestedOffset_}, {"pageSize", SearchPageSize}}}}}};
            if (requestedQuery_.empty()) requestBody = parse(browseRequest_, MaxAuthResponse);
        }
        else if (status_.step == Step::LaunchWidget)
        {
            request.url = std::string(ProxyOrigin) + "/getWidget";
            requestBody = Json{{"featureScheme", "WEB_V1"}, {"serviceToken", launchWidgetToken_}};
        }
        else if (status_.step == Step::ExperimentalPage)
            requestBody = Json{{"featureScheme", "WEB_V1"}, {"serviceToken", experimentalPageToken_}};
        if (status_.step != Step::AppConfig && status_.step != Step::PlayerLogin)
        {
            requestBody["clientContext"] = proxyClientContext();
            requestBody["dynamicFeatures"] = Json::array();
            // InputContextHelper (66718) supplies gamepadTypes to getPage.
            // This auth-only executable has no remote controller forwarding;
            // it does not claim CONTROLLER_PAIRED or a virtual controller.
            if (status_.step != Step::LaunchWidget)
                requestBody["inputContext"] = Json{{"gamepadTypes", Json::array()}};
        }
        request.body = requestBody.dump();
        request.headers[0] = "Content-Type: text/plain;charset=UTF-8";
        proxyHeaders(request, status_.step == Step::Catalog || browseRead(status_.step) || status_.step == Step::DetailPage ||
            status_.step == Step::LaunchWidget || status_.step == Step::ExperimentalPage,
            status_.step == Step::NegativeControl);
        break;
    }
    default: return std::nullopt;
    }
    pending_ = request.id;
    return request;
}
void NativeAuthProbe::proxyHeaders(Request& request, bool profile, bool negative) const
{
    request.headers.push_back(proxyUserAgent());
    const auto token = negative ? "LUNANX_INVALID_TOKEN_CONTROL" : accessToken_;
    char suffix[9];
    std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned>(request.id));
    const auto nonce = serial_.substr(0, 8) + "-" + serial_.substr(8, 4) + "-" + serial_.substr(12, 4) + "-" +
        serial_.substr(16, 4) + "-" + serial_.substr(20, 4) + suffix;
    for (const auto& header : std::vector<std::string>{"x-amz-access-token: " + token,
        "x-amz-device-serial-number: " + serial_, "x-amz-session-id: " + serial_, "x-amzn-RequestId: " + nonce,
        "x-amz-device-type: browser", "x-amz-platform: web", "x-amz-client-version: -",
        "x-amz-react-version: 1.0.37340.0", "x-amz-marketplace-id: ATVPDKIKX0DER",
        "x-amz-locale: en_US", "x-amz-country-of-residence: US", "x-amz-timezone: America/New_York"})
        request.headers.push_back(header);
    if (profile)
        request.headers.push_back("x-amz-profile-token: " + profileToken_);
    if (!clientTdi_.empty()) request.headers.push_back("x-amz-tdi: " + clientTdi_);
}
std::string NativeAuthProbe::proxyContext() const { return proxyClientContext().dump(); }
void NativeAuthProbe::complete(std::uint64_t id, long http, const std::string& body, std::int64_t nowMs)
{
    if (!pending_ || *pending_ != id || terminal(status_.step)) return;
    pending_.reset();
    if (nowMs >= deadline_)
    {
        if (browseRead(status_.step) && nowMs < sessionDeadline_) failBrowse(Error::Transport, http);
        else fail(Error::Expired, http);
        return;
    }
    status_.httpStatus = http;
    if (status_.step == Step::Register)
        status_.codeAgeSeconds = std::max<std::int64_t>(0, nowMs - codeIssuedAt_) / 1000;
    if (status_.step == Step::Register && http == 401) { nextPoll_ = nowMs + interval_; return; }
    if (status_.step == Step::NegativeControl)
    {
        if (http == 401 || http == 403)
        {
            if (mode_ == Mode::ProofOnly) { status_.step = Step::Complete; clearSecrets(); }
            else { status_.step = Step::AppConfig; deadline_ = std::min(sessionDeadline_, nowMs + 60'000); }
        }
        else fail(Error::AuthorizationNotProven, http);
        return;
    }
    if (http != 200)
    {
        if (status_.step==Step::CatalogAccess && http==400)
        {
            try
            {
                const auto error=parse(body,MaxAuthResponse);
                auto kind=error.value("__type",error.value("type",std::string()));
                auto message=error.value("message",error.value("Message",std::string()));
                auto& d=status_.catalogParsing;
                d.accessValidation=kind.find("Validation")!=std::string::npos || kind.find("InvalidArgument")!=std::string::npos;
                if (message.find("stage")!=std::string::npos) d.accessFieldHints|=1;
                if (message.find("productIdList")!=std::string::npos) d.accessFieldHints|=2;
                if (message.find("productId")!=std::string::npos) d.accessFieldHints|=4;
                wipe(kind); wipe(message);
                if (d.accessValidation && (d.accessFieldHints&2) && !(d.accessFieldHints&1) && accessBatchSize_>1)
                { accessBatchSize_=1; d.accessSingleFallback=true; return; }
            }
            catch (const std::exception&) { /* Keep server error text out of logs. */ }
        }
        if (browseRead(status_.step) && http != 401 && http != 403)
        { failBrowse(Error::Http, http); return; }
        if ((status_.step == Step::DetailPage || status_.step == Step::LaunchWidget || status_.step == Step::ExperimentalPage) && http != 401 && http != 403)
        {
            failDetail(Error::Http, http);
            return;
        }
        if (status_.step == Step::CodePair || status_.step == Step::Register || status_.step == Step::Refresh)
            classifyAuthError(status_, localAuthErrorIdentifier_, body,
                {status_.publicCode, privateCode_, accessToken_, refreshToken_, profileToken_, serial_});
        fail(Error::Http, http);
        return;
    }
    try
    {
        if (status_.step == Step::DetailPage)
        {
            acceptDetail(body);
            status_.step = Step::DetailReady;
            deadline_ = sessionDeadline_;
            return;
        }
        if (status_.step == Step::ExperimentalPage)
        {
            acceptExperimentalPage(body);
            status_.step = Step::ExperimentalReady;
            deadline_ = sessionDeadline_;
            return;
        }
        if (status_.step == Step::LaunchWidget) status_.detailParsing.stage = DetailParse::Json;
        if (status_.step == Step::ProfilesPage)
        {
            status_.profileParsing = {};
            status_.profileParsing.stage = ProfileParse::Json;
        }
        if (status_.step == Step::Catalog || (browseRead(status_.step) && status_.step!=Step::CatalogAccess))
        {
            status_.catalogParsing = {};
            status_.catalogParsing.stage = CatalogParse::Json;
            status_.catalogParsing.browse = browseRead(status_.step) && requestedQuery_.empty();
        }
        const auto value = parse(body, responseLimit(status_.step));
        switch (status_.step)
        {
        case Step::CodePair:
            status_.publicCode = text(value, "public_code", 32);
            if (status_.publicCode.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
                throw std::invalid_argument("Invalid public code");
            privateCode_ = text(value, "private_code", 4096);
            status_.codeLifetimeSeconds = seconds(value, "expires_in", 3600);
            codeIssuedAt_ = nowMs;
            deadline_ = std::min(deadline_, nowMs + status_.codeLifetimeSeconds * 1000);
            interval_ = seconds(value, "polling_interval_in_seconds", 60) * 1000;
            nextPoll_ = nowMs + interval_;
            status_.step = Step::Register;
            break;
        case Step::Register:
        {
            const auto& bearer = value.at("response").at("success").at("tokens").at("bearer");
            accessToken_ = text(bearer, "access_token", 32768);
            auto refresh = text(bearer, "refresh_token", 32768);
            seconds(bearer, "expires_in", 86400);
            replaceRefreshToken(std::move(refresh));
            status_.publicCode.clear();
            wipe(privateCode_);
            status_.registered = true;
            status_.step = Step::Refresh;
            deadline_ = nowMs + 60'000; // Code expiry no longer limits an already approved registration.
            break;
        }
        case Step::Refresh:
        {
            auto access = text(value, "access_token", 32768);
            const auto expires = seconds(value, "expires_in", 86400);
            auto refresh = value.contains("refresh_token") ? text(value, "refresh_token", 32768) : std::string{};
            wipe(accessToken_);
            accessToken_ = std::move(access);
            if (!refresh.empty()) replaceRefreshToken(std::move(refresh));
            accessExpiresAt_ = nowMs + expires * 1000;
            sessionDeadline_ = std::min(accessExpiresAt_, nowMs + 15 * 60 * 1000);
            deadline_ = std::min(deadline_, sessionDeadline_);
            status_.refreshed = true;
            status_.step = Step::LunaPage;
            break;
        }
        case Step::LunaPage:
            pageGroups(value);
            status_.lunaAccepted = true;
            status_.step = Step::NegativeControl;
            break;
        case Step::AppConfig:
        {
            if (!successful(value)) throw std::invalid_argument("Invalid app config");
            const auto& context = value.at("profilePageContext");
            profilePageType_ = text(context, "pageType", 128);
            profilePageId_ = text(context, "pageId", 512);
            status_.step = Step::ProfilesPage;
            break;
        }
        case Step::ProfilesPage:
            profiles_ = readProfiles(value, status_.profileParsing);
            status_.step = Step::ChooseProfile;
            deadline_ = sessionDeadline_;
            break;
        case Step::PlayerLogin:
        {
            // Public PlayerLoginHelper blocks these codes before accepting a
            // token, even if a response includes one. Never skip PIN checks.
            const auto code = value.contains("errorCode") && !value["errorCode"].is_null() ? value.at("errorCode").get<int>() : 0;
            if (code == 26 || code == 27 || code == 28 || code == 29)
            {
                const auto block = code == 28 ? ProfileBlock::PinRequired :
                    code == 29 ? ProfileBlock::NotInFamily : ProfileBlock::Migration;
                profiles_.at(*selectedProfile_).block = block;
                status_.profileBlock = block;
                selectedProfile_.reset();
                status_.step = Step::ChooseProfile;
                deadline_ = sessionDeadline_;
                break;
            }
            if (!successful(value)) throw std::invalid_argument("Profile login rejected");
            profileToken_ = text(value, "token", 32768);
            status_.step = clientIdentityEnabled_ ? Step::ClientIdentity : Step::Catalog;
            break;
        }
        case Step::ClientIdentity:
        {
            if (!successful(value)) throw std::invalid_argument("Client identity rejected");
            auto tdi = text(value, "tdi", 36);
            if (tdi.size() != 36) throw std::invalid_argument("Client identity shape");
            for (std::size_t i = 0; i < tdi.size(); ++i)
            {
                const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
                const char c = tdi[i];
                if (separator ? c != '-' : !((c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    throw std::invalid_argument("Client identity shape");
            }
            // Only the authenticated server-issued identifier is retained.
            // MQTT credentials, signed URLs and topics are not used here.
            wipe(clientTdi_); clientTdi_ = std::move(tdi);
            status_.clientIdentityReady = true;
            status_.step = Step::Catalog;
            break;
        }
        case Step::Catalog:
        {
            // Navigate caches the offered token and quickSearch URL context.
            // Dropping these can open a filter shell with no default results.
            wipe(browsePageToken_); wipe(browseQuickSearch_);
            try
            {
                const auto route=catalogNavigation(value);
                browsePageToken_=route.first; browseQuickSearch_=route.second;
            }
            catch (const std::exception&) { /* Optional navigation cannot prevent Home from loading. */ }
            for (auto& game : readGames(value, status_.catalogParsing))
            {
                games_.push_back(std::move(game.display));
                gameLinks_.push_back({std::move(game.serviceToken), std::move(game.path)});
            }
            status_.step = Step::CatalogReady;
            deadline_ = sessionDeadline_;
            break;
        }
        case Step::BrowsePage:
            status_.catalogParsing.navigationToken=!browsePageToken_.empty();
            status_.catalogParsing.navigationQuery=!browseQuickSearch_.empty();
            if (auto followup = catalogFollowup(value,browseQuickSearch_,status_.catalogParsing))
            {
                browseRequest_ = followup->dump();
                status_.step = Step::SearchPage;
                break;
            }
            [[fallthrough]];
        case Step::SearchPage:
        {
            // Only result content belongs in the grid; navigation and
            // recommendation groups must not become search matches.
            const auto& main = pageGroups(value).at("mainContent");
            const auto results = Json{{"pageMemberGroups", {{"mainContent", main}}}};
            auto parsed = readGames(results, status_.catalogParsing, 1024);
            const bool repeatedRaw=requestedOffset_>browsing_.offset && !parsed.empty() && !browseRawIds_.empty() &&
                std::all_of(parsed.begin(),parsed.end(),[&](const auto& item)
                { return std::find(browseRawIds_.begin(),browseRawIds_.end(),item.display.id)!=browseRawIds_.end(); });
            const bool repeat = repeatedRaw || (requestedOffset_ > browsing_.offset && !parsed.empty() && !games_.empty() &&
                std::all_of(parsed.begin(), parsed.end(), [&](const auto& item)
                { return std::any_of(games_.begin(), games_.end(), [&](const auto& old) { return old.id == item.display.id; }); }));
            candidateHasNext_ = !requestedQuery_.empty() && !repeat &&
                status_.catalogParsing.tiles >= SearchPageSize && requestedOffset_ < MaxSearchOffset;
            // An empty/repeated next page ends paging without stranding the
            // user on an empty grid when a server ignores pagination.
            if (!(requestedOffset_ > browsing_.offset && (parsed.empty() || repeat)))
            {
                clearBrowseCandidates();
                browseAccessStage_="Release";
                if (requestedIncludedOnly_ && value.contains("pageContext") && value["pageContext"].is_object() &&
                    value["pageContext"].contains("productStage") && !value["pageContext"]["productStage"].is_null())
                    browseAccessStage_=text(value["pageContext"],"productStage",128);
                for (auto& game : parsed)
                {
                    browseCandidates_.push_back(std::move(game.display));
                    browseCandidateLinks_.push_back({std::move(game.serviceToken), std::move(game.path)});
                }
                browseIncluded_.assign(browseCandidates_.size(),false);
                if (requestedIncludedOnly_ && !browseCandidates_.empty())
                { status_.step=Step::CatalogAccess; status_.catalogParsing.stage=CatalogParse::Access; break; }
                finishBrowse();
                break;
            }
            browsing_.hasNext=false;
            browsing_.error = Error::None;
            status_.step = Step::CatalogReady; deadline_ = sessionDeadline_;
            break;
        }
        case Step::CatalogAccess:
        {
            status_.catalogParsing.stage=CatalogParse::Access;
            if (!successful(value) || (value.contains("errorCode") && !value["errorCode"].is_null() && value["errorCode"]!=0) ||
                (value.contains("failedEntitlementSources") && !value["failedEntitlementSources"].is_null() && !value["failedEntitlementSources"].empty()))
                throw std::invalid_argument("Access check failed");
            const auto& entries=value.at("productEntitlementStatuses");
            const auto end=std::min(browseCandidates_.size(),accessOffset_+accessBatchSize_);
            if (!entries.is_array() || entries.size()!=end-accessOffset_) throw std::invalid_argument("Incomplete access check");
            std::set<std::string> seen;
            for (const auto& entry:entries)
            {
                const auto id=text(entry,"productId",512); const auto access=text(entry,"status",80);
                if (!seen.insert(id).second) throw std::invalid_argument("Duplicate access result");
                auto i=accessOffset_;
                for (;i<end && browseCandidates_[i].id!=id;++i) {}
                if (i==end) throw std::invalid_argument("Access identity mismatch");
                browseIncluded_[i]=access=="ENTITLED";
            }
            for (auto i=accessOffset_;i<end;++i)
            {
                ++status_.catalogParsing.accessChecked;
                if (browseIncluded_[i]) ++status_.catalogParsing.accessIncluded;
                else ++status_.catalogParsing.accessHidden;
            }
            accessOffset_=end;
            if (end==browseCandidates_.size()) finishBrowse();
            break;
        }
        case Step::LaunchWidget:
        {
            status_.detailParsing.stage = DetailParse::Actions;
            if (!successful(value)) throw std::invalid_argument("Launch widget rejected");
            const auto& actions = value.at("widget").at("actions");
            if (!actions.is_array() || actions.empty() || actions[0].value("target", "") != "GAME_SESSION_ASYNC")
                throw std::invalid_argument("Unexpected launch widget action");
            auto payload = actionPayload(actions[0]).dump();
            acceptLaunchPayload(payload);
            wipe(payload); wipe(launchWidgetToken_);
            status_.detailParsing.action = LaunchAction::GameSession;
            status_.detailParsing.stage = DetailParse::Complete;
            status_.step = Step::DetailReady;
            deadline_ = sessionDeadline_;
            break;
        }
        default: break;
        }
    }
    catch (const std::exception&)
    {
        if (browseRead(status_.step)) failBrowse(Error::Response, http);
        else if (status_.step == Step::DetailPage || status_.step == Step::LaunchWidget || status_.step == Step::ExperimentalPage) failDetail(Error::Response, http);
        else fail(Error::Response, http);
    }
}
bool NativeAuthProbe::chooseProfile(std::size_t index, std::int64_t nowMs)
{
    if (status_.step != Step::ChooseProfile || index >= profiles_.size()) return false;
    if (nowMs >= sessionDeadline_) { fail(Error::Expired); return false; }
    status_.profileBlock = profiles_[index].block;
    if (status_.profileBlock != ProfileBlock::None) return false;
    selectedProfile_ = index;
    wipe(profileToken_); wipe(clientTdi_); status_.clientIdentityReady = false; clearGames();
    status_.catalogParsing = {};
    status_.step = Step::PlayerLogin;
    deadline_ = std::min(sessionDeadline_, nowMs + 60'000);
    return true;
}
void NativeAuthProbe::returnToProfiles()
{
    if (status_.step != Step::CatalogReady) return;
    wipe(profileToken_); wipe(clientTdi_); status_.clientIdentityReady = false; clearGames(); selectedProfile_.reset();
    status_.catalogParsing = {};
    status_.profileBlock = ProfileBlock::None;
    status_.step = Step::ChooseProfile;
}
bool NativeAuthProbe::browseGames(const std::string& query, std::size_t offset, std::int64_t nowMs, bool includedOnly)
{
    if (status_.step != Step::CatalogReady || query.size() > MaxSearchQueryBytes ||
        offset > MaxSearchOffset || offset % SearchPageSize || (query.empty() && offset) ||
        std::any_of(query.begin(), query.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return false;
    try { (void)Json(query).dump(); } catch (...) { return false; } // Validate UTF-8 before changing state.
    if (offset && (!browsing_.active || query != browsing_.query || includedOnly!=browsing_.includedOnly ||
        (offset != browsing_.offset && offset + SearchPageSize != browsing_.offset &&
         !(offset == browsing_.offset + SearchPageSize && browsing_.hasNext)))) return false;
    if (nowMs >= sessionDeadline_) { fail(Error::Expired); return false; }
    clearDetail();
    if (!browsing_.active)
    {
        homeGames_ = std::move(games_); homeLinks_ = std::move(gameLinks_);
        games_.clear(); gameLinks_.clear(); browsing_.active = true;
    }
    else if (includedOnly!=browsing_.includedOnly)
    {
        // A failed switch to Included must not leave an unfiltered grid under
        // the Included heading. The user can explicitly reload Show all.
        for (auto& link:gameLinks_) { wipe(link.serviceToken); wipe(link.path); }
        games_.clear(); gameLinks_.clear(); browseRawIds_.clear();
    }
    clearBrowseCandidates();
    requestedQuery_ = query; requestedOffset_ = offset; requestedIncludedOnly_=includedOnly;
    browsing_.includedOnly=includedOnly; browsing_.error = Error::None; wipe(browseRequest_);
    status_.step = query.empty() ? Step::BrowsePage : Step::SearchPage;
    deadline_ = std::min(sessionDeadline_, nowMs + (includedOnly?120'000:60'000));
    return true;
}
void NativeAuthProbe::returnToHome()
{
    if (!browsing_.active || (status_.step != Step::CatalogReady && !browseRead(status_.step))) return;
    pending_.reset(); clearDetail();
    clearBrowseCandidates(); browseRawIds_.clear();
    for (auto& link : gameLinks_) { wipe(link.serviceToken); wipe(link.path); }
    games_ = std::move(homeGames_); gameLinks_ = std::move(homeLinks_);
    homeGames_.clear(); homeLinks_.clear(); wipe(browsing_.query); browsing_ = {}; wipe(requestedQuery_); wipe(browseRequest_);
    status_.step = Step::CatalogReady; deadline_ = sessionDeadline_;
}
bool NativeAuthProbe::openGame(std::size_t index, std::int64_t nowMs)
{
    if (status_.step != Step::CatalogReady || index >= games_.size() || !games_[index].detailsAvailable) return false;
    if (nowMs >= sessionDeadline_) { fail(Error::Expired); return false; }
    clearDetail();
    selectedGame_ = index;
    detailPath_ = gameLinks_[index].path;
    detail_.title = games_[index].title;
    status_.step = Step::DetailPage;
    deadline_ = std::min(sessionDeadline_, nowMs + 60'000);
    return true;
}
bool NativeAuthProbe::openGamePath(const std::string& path, const std::string& title, std::int64_t nowMs)
{
    if (status_.step != Step::CatalogReady || browsing_.active) return false;
    try
    {
        const auto normalized = pagePath(Json{{"target", path}});
        if (normalized != path || (path.rfind("game/", 0) != 0 && path.rfind("detail/", 0) != 0) ||
            path.back() == '/') return false;
        const auto display = displayText(Json{{"title", title}}, "title");
        (void)Json(display).dump();
        if (nowMs >= sessionDeadline_) { fail(Error::Expired); return false; }
        clearDetail(); detailPath_ = normalized; detail_.title = display;
        status_.step = Step::DetailPage; deadline_ = std::min(sessionDeadline_, nowMs + 60'000);
        return true;
    }
    catch (...) { return false; }
}
bool NativeAuthProbe::inspectLaunch(std::int64_t nowMs)
{
    if (status_.step != Step::DetailReady || status_.detailParsing.error != Error::None) return false;
    const bool notice = status_.detailParsing.action == LaunchAction::ExperimentalClient && !experimentalPageToken_.empty();
    if (!notice && (status_.detailParsing.action != LaunchAction::DirectLaunch || launchWidgetToken_.empty())) return false;
    if (nowMs >= sessionDeadline_) { fail(Error::Expired); return false; }
    if (notice) { clearNoticeActions(); notice_ = {}; status_.noticeParsing = {}; }
    status_.step = notice ? Step::ExperimentalPage : Step::LaunchWidget;
    deadline_ = std::min(sessionDeadline_, nowMs + 60'000);
    return true;
}
bool NativeAuthProbe::acknowledgeUnsupportedClient(std::int64_t nowMs)
{
    // Invoked by the launcher only for a pending Play request. The protocol's
    // EXPERIMENTAL_CLIENT route identifies this warning; text/button labels
    // are not used to guess whether a generic consent dialog may be skipped.
    const auto& parsing = status_.noticeParsing;
    if (status_.step != Step::ExperimentalReady ||
        status_.detailParsing.primaryAction != LaunchAction::ExperimentalClient ||
        status_.detailParsing.action != LaunchAction::ExperimentalClient || !parsing.complete ||
        parsing.checkboxes || parsing.legalTerms || parsing.secondaryActions ||
        notice_.choices.size() != 2) return false;
    std::optional<std::size_t> continuation;
    bool dismiss = false;
    for (std::size_t i = 0; i < notice_.choices.size(); ++i)
    {
        const auto& choice = notice_.choices[i];
        if (choice.action == LaunchAction::GameSession || choice.action == LaunchAction::DirectLaunch)
        {
            if (continuation) return false;
            continuation = i;
        }
        else if (choice.actionTarget == "DIALOG_DISMISS") dismiss = true;
        else return false;
    }
    return continuation && dismiss && chooseNoticeButton(*continuation, nowMs);
}
bool NativeAuthProbe::chooseNoticeButton(std::size_t index, std::int64_t nowMs)
{
    if (status_.step != Step::ExperimentalReady || !status_.noticeParsing.complete || index >= notice_.choices.size() ||
        index >= noticeActions_.size()) return false;
    if (nowMs >= sessionDeadline_) { fail(Error::Expired); return false; }
    const auto kind = notice_.choices[index].action;
    if (kind != LaunchAction::GameSession && kind != LaunchAction::DirectLaunch) return false;
    try
    {
        const auto action = parse(noticeActions_[index], MaxAuthResponse);
        if (kind == LaunchAction::GameSession)
        {
            auto payload = actionPayload(action).dump();
            acceptLaunchPayload(payload); wipe(payload);
            status_.step = Step::DetailReady;
            deadline_ = sessionDeadline_;
        }
        else
        {
            launchWidgetToken_ = text(actionPayload(action), "serviceToken", 32768);
            status_.step = Step::LaunchWidget;
            deadline_ = std::min(sessionDeadline_, nowMs + 60'000);
        }
        status_.detailParsing.action = kind;
        status_.detailParsing.stage = DetailParse::Complete;
        status_.noticeParsing.accepted = true;
        clearNoticeActions();
        return true;
    }
    catch (const std::exception&) { failDetail(Error::Response, status_.httpStatus); return false; }
}
void NativeAuthProbe::returnToDetail()
{
    if (status_.step != Step::ExperimentalPage && status_.step != Step::ExperimentalReady) return;
    pending_.reset(); clearNoticeActions(); notice_ = {}; status_.noticeParsing = {};
    status_.step = Step::DetailReady;
    deadline_ = sessionDeadline_;
}
void NativeAuthProbe::returnToCatalog()
{
    if (status_.step != Step::DetailPage && status_.step != Step::DetailReady && status_.step != Step::LaunchWidget &&
        status_.step != Step::ExperimentalPage && status_.step != Step::ExperimentalReady) return;
    pending_.reset();
    clearDetail();
    status_.step = Step::CatalogReady;
    deadline_ = sessionDeadline_;
}
void NativeAuthProbe::transportFailed(std::uint64_t id)
{
    if (!pending_ || *pending_ != id) return;
    if (browseRead(status_.step)) failBrowse(Error::Transport);
    else if (status_.step == Step::DetailPage || status_.step == Step::LaunchWidget || status_.step == Step::ExperimentalPage) failDetail(Error::Transport);
    else fail(Error::Transport);
}
void NativeAuthProbe::cancel()
{
    if (terminal(status_.step)) return;
    status_.step = Step::Cancelled;
    clearSecrets();
}
}
