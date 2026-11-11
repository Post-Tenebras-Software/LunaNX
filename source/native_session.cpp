#include "native_session.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace luna::auth
{
namespace
{
using Json = nlohmann::json;
constexpr std::int64_t QueueLimit = 2 * 60 * 60 * 1000;
void wipe(std::string& value)
{
    volatile char* data = value.empty() ? nullptr : &value[0];
    for (std::size_t i = 0; i < value.size(); ++i) data[i] = 0;
    value.clear();
}
Json object(const std::string& body, std::size_t limit = MaxAuthResponse)
{
    if (body.empty() || body.size() > limit) throw std::invalid_argument("Response size");
    std::vector<std::set<std::string>> keys;
    auto value = Json::parse(body, [&](int depth, Json::parse_event_t event, Json& item)
    {
        if (depth > 32) throw std::invalid_argument("Response depth");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second)
            throw std::invalid_argument("Duplicate field");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    if (!value.is_object()) throw std::invalid_argument("Response object");
    return value;
}
std::string text(const Json& value, const char* key, std::size_t max = 512)
{
    const auto& item = value.at(key);
    if (!item.is_string()) throw std::invalid_argument("String field");
    auto result = item.get<std::string>();
    if (result.empty() || result.size() > max || std::any_of(result.begin(), result.end(),
        [](unsigned char c) { return c < 32 || c > 126; })) throw std::invalid_argument("String limit");
    return result;
}
std::optional<std::int64_t> number(const Json& value, const char* key)
{
    if (!value.contains(key) || value[key].is_null()) return std::nullopt;
    const auto& item = value[key];
    if (!item.is_number_integer() || (item.is_number_unsigned() && item.get<std::uint64_t>() >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument("Integer field");
    const auto result = item.get<std::int64_t>();
    if (result < 0) throw std::invalid_argument("Negative field");
    return result;
}
void classifyHttpError(NativeSession::Status& status, NativeSession::Operation operation,
    const std::string& body)
{
    using Session = NativeSession;
    auto& d = status.httpDiagnostic;
    d = {}; d.operation = operation; d.error = Session::HttpError::Unknown;
    d.responseBytes = body.size();
    if (body.empty()) { d.body = Session::ErrorBody::Empty; return; }
    if (body.size() > MaxAuthResponse) { d.body = Session::ErrorBody::TooLarge; return; }
    d.body = Session::ErrorBody::Unparseable;
    try
    {
        const auto value = object(body);
        d.body = Session::ErrorBody::Object;
        try { status.apiErrorCode = number(value, "errorCode").value_or(0); } catch (...) {}
        const Json* container = &value;
        if (value.contains("response") && value["response"].is_object()) container = &value["response"];
        if (container->contains("error") && (*container)["error"].is_object()) container = &(*container)["error"];
        std::string name;
        for (const auto* key : {"__type", "type", "code", "errorType", "error"})
            if (container->contains(key) && (*container)[key].is_string() &&
                (*container)[key].get_ref<const std::string&>().size() <= 256)
            { name = (*container)[key].get<std::string>(); break; }
        // Coral/AWS exception identifiers may have a namespace prefix. Only
        // exact known suffixes become public labels; the identifier is discarded.
        const auto separator = name.find_last_of("#:");
        if (separator != std::string::npos) name.erase(0, separator + 1);
        for (const auto& item : std::vector<std::pair<const char*, Session::HttpError>>{
            {"InvalidRequestException", Session::HttpError::InvalidRequest},
            {"InvalidRequest", Session::HttpError::InvalidRequest}, {"invalid_request", Session::HttpError::InvalidRequest},
            {"ValidationException", Session::HttpError::Validation},
            {"InvalidParameterException", Session::HttpError::InvalidParameter},
            {"InvalidParameterValue", Session::HttpError::InvalidParameter},
            {"MissingRequiredParameterException", Session::HttpError::MissingParameter},
            {"MissingParameter", Session::HttpError::MissingParameter},
            {"UnauthorizedException", Session::HttpError::Unauthorized},
            {"NotAuthorizedException", Session::HttpError::Unauthorized},
            {"AccessDeniedException", Session::HttpError::AccessDenied},
            {"InvalidTokenException", Session::HttpError::InvalidToken},
            {"InvalidAccessTokenException", Session::HttpError::InvalidToken},
            {"invalid_grant", Session::HttpError::InvalidToken},
            {"ThrottlingException", Session::HttpError::Throttled},
            {"TooManyRequestsException", Session::HttpError::Throttled},
            {"InternalServerException", Session::HttpError::Internal},
            {"InternalFailure", Session::HttpError::Internal}})
            if (name == item.first) { d.error = item.second; break; }
        // Message text can echo credentials. Retain only mentions of a fixed
        // field vocabulary, never descriptions, values or unknown identifiers.
        // These are hints, not proof that the mentioned field caused rejection.
        const auto identifier = [](char c)
        { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_'; };
        for (const auto* key : {"message", "Message", "error_description", "parameter", "field"})
        {
            if (!container->contains(key) || !(*container)[key].is_string()) continue;
            const auto& message = (*container)[key].get_ref<const std::string&>();
            if (message.size() > 8192) continue;
            for (unsigned field = 1; field <= static_cast<unsigned>(Session::ErrorField::SourceToken); ++field)
            {
                const auto kind = static_cast<Session::ErrorField>(field);
                const std::string label = Session::errorFieldLabel(kind);
                std::size_t at = 0;
                while ((at = message.find(label, at)) != std::string::npos)
                {
                    const auto end = at + label.size();
                    if ((at == 0 || !identifier(message[at - 1])) &&
                        (end == message.size() || !identifier(message[end])))
                    {
                        d.fieldHints |= 1u << (field - 1);
                        if (d.fieldHint == Session::ErrorField::None) d.fieldHint = kind;
                        break;
                    }
                    at = end;
                }
            }
        }
    }
    catch (...) {} // HTTP remains the failure even if its body is malformed.
}
bool present(const Json& value, const char* key)
{
    if (!value.contains(key) || value[key].is_null()) return false;
    if (value[key].is_boolean()) return value[key].get<bool>();
    if (value[key].is_string()) return !value[key].get<std::string>().empty();
    return true; // Unsupported structured launch requirement: fail closed.
}
std::string parentalAction(const Json& payload)
{
    const auto rating = payload.value("esrbRating", "");
    if (rating == "EVERYONE" || rating == "EARLY_CHILDHOOD") return "LAUNCH_GAME_EVERYONE";
    if (rating == "EVERYONE_10_PLUS" || rating == "EVERYONE10+") return "LAUNCH_GAME_10PLUS";
    if (rating == "TEEN" || rating == "TEEN13+") return "LAUNCH_GAME_TEEN";
    return "LAUNCH_GAME_MATURE"; // Same conservative unknown/unrated mapping as module 50984.
}
NativeSession::ValueKind valueKind(const Json& value)
{
    using Kind = NativeSession::ValueKind;
    if (value.is_boolean()) return Kind::Boolean;
    if (value.is_string()) return Kind::String;
    if (value.is_number()) return value == 0 ? Kind::ZeroNumber : Kind::Number;
    if (value.is_array()) return Kind::Array;
    if (value.is_object()) return Kind::Object;
    return Kind::None;
}
NativeSession::Block launchBlock(const Json& p, NativeSession::Status& status, bool handleEula = false)
{
    using Block = NativeSession::Block;
    using Requirement = NativeSession::Requirement;
    unsigned bit = 1;
    for (const auto& field : std::vector<std::pair<const char*, Requirement>>{
        {"transferRequest", Requirement::Transfer}, {"revokeProfileActivityRequest", Requirement::RevokeActivity},
        {"partyId", Requirement::Party}, {"streamerMode", Requirement::StreamerMode},
        {"buildId", Requirement::Build}, {"gameBuildConfigurationId", Requirement::BuildConfiguration},
        {"saveSlotId", Requirement::SaveSlot}, {"pconToken", Requirement::ParentalProof}})
    {
        // StartGameSession (61343) forwards these fields only when its
        // isDirectBuildLaunch prop is true. Our GAME_SESSION_ASYNC path is a
        // normal retail launch; payload build metadata does not select a build.
        const bool retailBuildMetadata = field.second == Requirement::Build ||
            field.second == Requirement::BuildConfiguration;
        if (!retailBuildMetadata && present(p, field.first))
        {
            status.unsupportedFields |= bit;
            if (status.requirement == Requirement::None)
            {
                status.requirement = field.second;
                status.requirementKind = valueKind(p.at(field.first));
            }
        }
        bit <<= 1;
    }
    if (status.unsupportedFields) return Block::UnsupportedLaunch;
    for (const auto& gate : std::vector<std::pair<const char*, Block>>{
        {"dialogTermsAndConditionsToken", Block::Terms}, {"dialogAgeRatingConfirmationToken", Block::AgeRating},
        {"dialogAcceptEulaToken", Block::Eula},
        {"dialogMouseKeyboardRequiredToken", Block::MouseKeyboard}})
        if (present(p, gate.first) && !(handleEula && gate.second == Block::Eula)) return gate.second;
    if (p.contains("supportedInputTypes") && !p["supportedInputTypes"].is_null())
    {
        const auto& inputs = p["supportedInputTypes"];
        if (!inputs.is_array() || inputs.empty() || inputs.size() > 16 ||
            std::find(inputs.begin(), inputs.end(), "CONTROLLER") == inputs.end()) return Block::MouseKeyboard;
    }
    // Location-service and browser pointer-lock dialogs do not apply to the
    // Switch's controller-only client. No keyboard/mouse capability is claimed.
    return Block::None;
}
bool ipv4(const std::string& address)
{
    std::istringstream stream(address);
    std::string part;
    unsigned count = 0;
    while (std::getline(stream, part, '.'))
    {
        if (++count > 4 || part.empty() || part.size() > 3 ||
            part.find_first_not_of("0123456789") != std::string::npos ||
            (part.size() > 1 && part.front() == '0') || std::stoi(part) > 255) return false;
    }
    return count == 4 && address.back() != '.';
}
std::string normalizeAnswer(const Json& game)
{
    auto response = object(text(game, "signalingResponse", 65536));
    // text() rejects control bytes in JSON strings, not escaped SDP within it.
    if (response.value("type", "") != "answer" || !response.contains("sdp") || !response["sdp"].is_string())
        throw std::invalid_argument("Answer type");
    std::string sdp = response["sdp"].get<std::string>();
    if (sdp.size() > 65536 || sdp.rfind("v=0\r\n", 0) != 0 || sdp.find('\0') != std::string::npos)
        throw std::invalid_argument("Answer SDP");
    // Avoid Switch reverse-DNS formatting in ICE. Only replace an EC2 hostname
    // whose embedded IPv4 exactly matches this authenticated session's publicIp.
    if (game.contains("publicIp") && !game["publicIp"].is_null())
    {
        const auto ip = text(game, "publicIp", 64);
        if (!ipv4(ip)) throw std::invalid_argument("Session IPv4");
        auto encoded = ip;
        std::replace(encoded.begin(), encoded.end(), '.', '-');
        const auto host = "ec2-" + encoded + ".compute-1.amazonaws.com";
        std::istringstream lines(sdp);
        std::string line, normalized;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("a=candidate:", 0) == 0 || line.rfind("c=IN IP4 ", 0) == 0)
            {
                const auto pos = line.find(" " + host + " ");
                if (pos != std::string::npos) line.replace(pos + 1, host.size(), ip);
                if (line == "c=IN IP4 " + host) line = "c=IN IP4 " + ip;
            }
            normalized += line + "\r\n";
        }
        wipe(sdp); sdp = std::move(normalized);
    }
    response["sdp"] = sdp;
    wipe(sdp);
    return response.dump();
}
}
NativeSession::NativeSession(NativeAuthProbe& auth) : auth_(auth) {}
NativeSession::~NativeSession()
{
    wipe(allocatedSession_); wipe(launchProfile_);
    wipe(payload_); wipe(ratingAction_); wipe(eulaToken_); wipe(eulaProduct_); clearSoftwareNotice();
}
void NativeSession::clearSoftwareNotice()
{
    wipe(softwareToken_);
    clearNoticeContent();
}
void NativeSession::clearNoticeContent()
{
    for (auto& action : softwareActions_) wipe(action);
    softwareActions_.clear(); softwareNotice_ = {};
}
const char* NativeSession::blockLabel(Block block)
{
    switch (block)
    {
    case Block::None: return "none";
    case Block::ParentalPin: return "parental_pin_required";
    case Block::ParentalDenied: return "parental_denied";
    case Block::Terms: return "terms_dialog_required";
    case Block::AgeRating: return "age_rating_dialog_required";
    case Block::Eula: return "eula_dialog_required";
    case Block::SoftwareDecode: return "software_decode_dialog_required";
    case Block::MouseKeyboard: return "mouse_keyboard_required";
    case Block::UnsupportedLaunch: return "unsupported_solo_launch_requirements";
    case Block::InvalidResponse: return "invalid_response";
    case Block::Expired: return "expired";
    case Block::Transport: return "transport";
    case Block::Http: return "http";
    }
    return "unknown";
}
const char* NativeSession::requirementLabel(Requirement requirement)
{
    switch (requirement)
    {
    case Requirement::None: return "none";
    case Requirement::Transfer: return "transferRequest";
    case Requirement::RevokeActivity: return "revokeProfileActivityRequest";
    case Requirement::Party: return "partyId";
    case Requirement::StreamerMode: return "streamerMode";
    case Requirement::Build: return "buildId";
    case Requirement::BuildConfiguration: return "gameBuildConfigurationId";
    case Requirement::SaveSlot: return "saveSlotId";
    case Requirement::ParentalProof: return "pconToken";
    }
    return "unknown";
}
const char* NativeSession::valueKindLabel(ValueKind kind)
{
    switch (kind)
    {
    case ValueKind::None: return "none";
    case ValueKind::Boolean: return "boolean";
    case ValueKind::String: return "string";
    case ValueKind::ZeroNumber: return "number_zero";
    case ValueKind::Number: return "number";
    case ValueKind::Array: return "array";
    case ValueKind::Object: return "object";
    }
    return "unknown";
}
const char* NativeSession::operationLabel(Operation operation)
{
    switch (operation)
    {
    case Operation::None: return "none";
    case Operation::Pin: return "isPinChallengeRequired";
    case Operation::Refresh: return "refresh";
    case Operation::Create: return "createGameSessionJob";
    case Operation::Poll: return "getGameSessionJob";
    case Operation::Cancel: return "cancelGameSessionJob";
    case Operation::SoftwarePage: return "software_warning";
    case Operation::EulaPage: return "eula_page";
    case Operation::EulaAccept: return "eula_accept";
    case Operation::End: return "terminateGameSession";
    }
    return "unknown";
}
const char* NativeSession::httpErrorLabel(HttpError error)
{
    switch (error)
    {
    case HttpError::None: return "none";
    case HttpError::Unknown: return "unknown";
    case HttpError::InvalidRequest: return "invalid_request";
    case HttpError::Validation: return "validation";
    case HttpError::InvalidParameter: return "invalid_parameter";
    case HttpError::MissingParameter: return "missing_parameter";
    case HttpError::Unauthorized: return "unauthorized";
    case HttpError::AccessDenied: return "access_denied";
    case HttpError::InvalidToken: return "invalid_token";
    case HttpError::Throttled: return "throttled";
    case HttpError::Internal: return "internal";
    }
    return "unknown";
}
const char* NativeSession::errorBodyLabel(ErrorBody body)
{
    switch (body)
    {
    case ErrorBody::None: return "none";
    case ErrorBody::Empty: return "empty";
    case ErrorBody::TooLarge: return "too_large";
    case ErrorBody::Unparseable: return "unparseable";
    case ErrorBody::Object: return "object";
    }
    return "unknown";
}
const char* NativeSession::errorFieldLabel(ErrorField field)
{
    switch (field)
    {
    case ErrorField::None: return "none";
    case ErrorField::Timeout: return "timeout";
    case ErrorField::GameId: return "gameId";
    case ErrorField::PlayToken: return "playToken";
    case ErrorField::SignalingRequest: return "signalingRequest";
    case ErrorField::ClientContext: return "clientContext";
    case ErrorField::ClientFeatures: return "clientFeatures";
    case ErrorField::DataUsage: return "dataUsage";
    case ErrorField::Routing: return "routing";
    case ErrorField::RegionLatencies: return "regionLatencies";
    case ErrorField::RegionPreferences: return "regionPreferences";
    case ErrorField::ProductStage: return "productStage";
    case ErrorField::Tdi: return "tdi";
    case ErrorField::ParentalToken: return "pconToken";
    case ErrorField::SaveSlot: return "saveSlotId";
    case ErrorField::JobId: return "gameSessionJobId";
    case ErrorField::Transfer: return "transferRequest";
    case ErrorField::RevokeActivity: return "revokeProfileActivityRequest";
    case ErrorField::SourceToken: return "source_token";
    }
    return "unknown";
}
void NativeSession::fail(Block block)
{
    status_.state = status_.state == State::Ending ? State::EndFailed : State::Failed;
    status_.block = block; status_.refreshing = false;
    pending_.reset(); wipe(payload_); wipe(eulaToken_); wipe(eulaProduct_); clearSoftwareNotice();
}
bool NativeSession::start(std::int64_t now, bool hardwareDecoderPrepared)
{
    if (status_.state != State::Idle || auth_.status_.step != Step::DetailReady ||
        !auth_.status_.detailParsing.hasPlayToken || auth_.profileToken_.empty() || auth_.launchPayload_.empty()) return false;
    if (now < 0 || now >= auth_.sessionDeadline_) { fail(Block::Expired); return false; }
    now_ = now; deadline_ = now + QueueLimit;
    if (!auth_.selectedProfile_ || *auth_.selectedProfile_ >= auth_.profiles_.size()) return false;
    launchProfile_ = auth_.profiles_[*auth_.selectedProfile_].id;
    try
    {
        payload_ = auth_.launchPayload_;
        const auto p = object(payload_);
        ratingAction_ = parentalAction(p);
        const auto block = launchBlock(p, status_, true);
        if (block != Block::None) { status_.state = State::Blocked; status_.block = block; wipe(payload_); }
        else
        {
            // The browser's checkSoftwareDecode is conditional on software
            // decoding, not merely on this token being present. The launcher
            // initializes its actual decoder before providing this capability.
            if (!hardwareDecoderPrepared && present(p, "dialogSoftwareDecodeToken"))
                softwareToken_ = text(p, "dialogSoftwareDecodeToken", 32768);
            if (present(p, "dialogAcceptEulaToken"))
                eulaToken_ = text(p, "dialogAcceptEulaToken", 32768);
            status_.state = State::CheckingPin;
        }
        return true;
    }
    catch (...) { fail(Block::InvalidResponse); return false; }
}
Request NativeSession::issue(Operation operation, std::string body, std::uint64_t ownerId)
{
    const auto id = nextId_++;
    Request result{id, Step::NativeSession, {}, std::move(body), {"Content-Type: text/plain;charset=UTF-8"}};
    const char* route = operation == Operation::Pin ? "isPinChallengeRequired" :
        operation == Operation::Create ? "createGameSessionJob" :
        operation == Operation::Poll ? "getGameSessionJob" :
        operation == Operation::SoftwarePage || operation == Operation::EulaPage ? "getPage" :
        operation == Operation::EulaAccept ? "writeCustomerPreferences" :
        operation == Operation::End ? "terminateGameSession" : "cancelGameSessionJob";
    if (operation == Operation::Refresh)
    {
        result.url = "https://api.amazon.com/auth/token";
        result.headers[0] = "Content-Type: application/json";
        result.body = Json{{"app_name", "Amazon Luna"}, {"app_version", "1.0.37340.0"},
            {"requested_token_type", "access_token"}, {"source_token", auth_.refreshToken_},
            {"source_token_type", "refresh_token"}}.dump();
        status_.refreshing = true;
    }
    else
    {
        if (operation == Operation::SoftwarePage || operation == Operation::EulaPage)
        {
            result.step = Step::NativeNotice;
            result.body = Json{{"featureScheme", "WEB_V1"},
                {"serviceToken", operation == Operation::EulaPage ? eulaToken_ : softwareToken_},
                {"clientContext", object(auth_.proxyContext())}, {"dynamicFeatures", Json::array()},
                {"inputContext", {{"gamepadTypes", Json::array()}}}}.dump();
        }
        if (operation == Operation::EulaAccept)
            result.body = Json{{"timeout", 10000}, {"preferences", Json::array({Json{
                {"preferenceName", "EULA_ACCEPTED"},
                {"preferenceValue", Json{{"isAccepted", true}, {"productId", eulaProduct_}}.dump()}}})}}.dump();
        result.url = std::string("https://proxy-prod.us-east-1.tempo.digital.a2z.com/") + route;
        auth_.proxyHeaders(result, true);
    }
    pending_ = Pending{id, ownerId, operation};
    return result;
}
bool NativeSession::chooseEulaButton(std::size_t index, std::int64_t now)
{
    if (status_.state != State::EulaNotice || pending_ ||
        index >= softwareNotice_.choices.size() || index >= softwareActions_.size()) return false;
    if (now < now_ || now >= deadline_) { fail(Block::Expired); return false; }
    now_ = now;
    const auto& choice = softwareNotice_.choices[index];
    if (choice.actionTarget == "DIALOG_DISMISS") { cancel(now); return true; }
    if (!status_.softwareNotice.complete || choice.action != LaunchAction::AcceptEula) return false;
    try
    {
        // Browser AcceptEula (38824) records this exact preference. Never use
        // arbitrary actions or a product supplied by a different game's modal.
        const auto action = object(softwareActions_[index]);
        const auto& raw = action.at("payload");
        const auto p = raw.is_string() ? object(raw.get<std::string>()) : raw;
        eulaProduct_ = text(p, "productId");
        if (eulaProduct_ != text(object(payload_), "gameId")) throw std::invalid_argument("EULA product mismatch");
        status_.state = State::EulaAccept;
        return true;
    }
    catch (...) { fail(Block::InvalidResponse); return false; }
}
bool NativeSession::chooseSoftwareNoticeButton(std::size_t index, std::int64_t now)
{
    if (status_.state != State::SoftwareNotice || pending_ || !status_.softwareNotice.complete ||
        index >= softwareNotice_.choices.size() || index >= softwareActions_.size()) return false;
    if (now < now_ || now >= deadline_) { fail(Block::Expired); return false; }
    now_ = now;
    const auto& choice = softwareNotice_.choices[index];
    if (choice.actionTarget == "DIALOG_DISMISS") { cancel(now); return true; }
    if (choice.action != LaunchAction::GameSession) return false;
    try
    {
        const auto original = object(payload_);
        const auto action = object(softwareActions_[index]);
        const auto& raw = action.at("payload");
        auto next = raw.is_string() ? object(raw.get<std::string>()) : raw;
        // Continue only this explicitly selected game and product stage. A
        // fresh play token may arrive in the authenticated continuation action.
        if (text(next, "gameId") != text(original, "gameId") ||
            next.value("productStage", "") != original.value("productStage", ""))
            throw std::invalid_argument("Notice launch mismatch");
        text(next, "playToken", 32768);
        const bool sameEula = status_.eulaAccepted && (!present(next, "dialogAcceptEulaToken") ||
            text(next, "dialogAcceptEulaToken", 32768) == eulaToken_);
        const auto block = launchBlock(next, status_, sameEula);
        if (block != Block::None)
        {
            status_.state = State::Blocked; status_.block = block;
            wipe(payload_); clearSoftwareNotice(); return false;
        }
        ratingAction_ = parentalAction(next);
        wipe(payload_); payload_ = next.dump();
        status_.softwareNotice.accepted = true;
        clearSoftwareNotice();
        // Recheck parental authorization for the continuation payload. The
        // warning is acknowledged only for this one in-memory attempt.
        status_.state = State::CheckingPin;
        return true;
    }
    catch (...) { fail(Block::InvalidResponse); return false; }
}
bool NativeSession::supplyOffer(std::string sdp, std::int64_t now, video::Mode mode, Quality requestedQuality)
{
    if (status_.state != State::AwaitOffer || pending_) { wipe(sdp); return false; }
    now_ = now;
    try
    {
        if (now >= deadline_ || sdp.size() > 32768 || sdp.rfind("v=0\r\n", 0) != 0 ||
            sdp.find("m=video ") == std::string::npos || sdp.find("m=audio ") == std::string::npos ||
            sdp.find("m=application ") == std::string::npos || sdp.find("a=fingerprint:") == std::string::npos ||
            sdp.find('\0') != std::string::npos) throw std::invalid_argument("Fresh offer required");
        const auto p = object(payload_);
        const Json context = {{"browserClientRole", "browser"}, {"browserType", "LunaNX"},
            {"browserVersion", "0.4.0"}, {"deviceModel", "Nintendo Switch"}, {"deviceType", "console"},
            {"osName", "Horizon"}, {"osVersion", "homebrew"}};
        Json offer = {{"type", "offer"}, {"sdp", sdp}, {"extras", {
            {"requestedChannels", {"display", "audio", "input", "participant"}},
            {"features", Json::array()}, {"clientContext", context}}}};
        // Browser DeviceInfoController (95985) always includes dataUsage.
        // ClientSettingsState (34549) uses LIMITED for its 720p default and
        // STANDARD otherwise. Frame rate is negotiated separately in SDP/control.
        if (requestedQuality != Quality::MatchMode && requestedQuality != Quality::Standard)
            throw std::invalid_argument("Unknown provisioning quality");
        const auto height = video::configuration(mode).height;
        const auto quality = requestedQuality == Quality::Standard || height > 720 ?
            "STANDARD" : "LIMITED";
        Json body = {{"timeout", 10000}, {"gameId", text(p, "gameId")}, {"playToken", text(p, "playToken", 32768)},
            {"signalingRequest", offer.dump()}, {"clientContext", object(auth_.proxyContext())},
            {"clientFeatures", {{"dataUsage", quality}, {"routing", {"TURN"}}}}};
        // Do not copy buildId/gameBuildConfigurationId from retail metadata.
        // The browser also omits both outside its dedicated direct-build mode.
        // Region preferences/latencies are optional in the browser when probes
        // fail. Leave them absent rather than inventing measurements or a region.
        if (present(p, "productStage")) body["productStage"] = text(p, "productStage", 128);
        session::ProvisioningSession::Options options;
        options.overallTimeoutMs = std::max<std::int64_t>(1, deadline_ - now);
        options.requestTimeoutMs = 20'000; // HTTPS transport is bounded to 15 seconds.
        owner_ = std::make_unique<session::ProvisioningSession>([this] { return now_; }, options);
        owner_->start({body.dump()});
        wipe(payload_); wipe(sdp); wipe(auth_.launchPayload_); wipe(auth_.playToken_);
        status_.state = State::Provisioning;
        sync();
        return true;
    }
    catch (...) { wipe(sdp); fail(Block::InvalidResponse); return false; }
}
void NativeSession::sync()
{
    if (!owner_ || status_.state == State::Ending || status_.state == State::Ended ||
        status_.state == State::EndFailed) return;
    const auto& s = owner_->status();
    status_.jobState = s.state; status_.jobError = s.error; status_.apiErrorCode = s.apiErrorCode;
    status_.position = s.positionInLine; status_.waitSeconds = s.expectedWaitTimeSeconds;
    status_.remoteWorkMayRemain = s.remoteWorkMayRemain;
    if (s.state == session::State::Ready) status_.state = State::Ready;
    if (s.state == session::State::Failed) status_.state = State::Failed;
    if (s.state == session::State::Cancelled) status_.state = State::Cancelled;
}
std::optional<Request> NativeSession::nextRequest(std::int64_t now)
{
    if (status_.state != State::CheckingPin && status_.state != State::AwaitOffer && status_.state != State::Provisioning &&
        status_.state != State::SoftwarePage && status_.state != State::SoftwareNotice &&
        status_.state != State::EulaPage && status_.state != State::EulaNotice && status_.state != State::EulaAccept &&
        status_.state != State::Ending)
        return std::nullopt;
    if (now < now_ || now >= deadline_) { fail(Block::Expired); return std::nullopt; }
    now_ = now;
    if (status_.state == State::SoftwareNotice || status_.state == State::EulaNotice) return std::nullopt;
    // The HTTPS transport bounds each pending request to 15 seconds. Never
    // issue two authenticated operations at once.
    if (pending_) return std::nullopt;
    if (status_.state == State::Ending && (!auth_.selectedProfile_ ||
        *auth_.selectedProfile_ >= auth_.profiles_.size() || auth_.profileToken_.empty() ||
        auth_.profiles_[*auth_.selectedProfile_].id != launchProfile_))
    { fail(Block::InvalidResponse); return std::nullopt; }
    if (auth_.accessExpiresAt_ <= now + 60'000)
        return issue(Operation::Refresh, {});
    if (status_.state == State::Ending)
        return issue(Operation::End, Json{{"timeout", 10000}, {"sessionIds", {allocatedSession_}},
            {"terminationReason", "OVERLAY_EXIT"}}.dump());
    if (status_.state == State::CheckingPin)
        return issue(Operation::Pin, Json{{"actions", {ratingAction_}}}.dump());
    if (status_.state == State::SoftwarePage) return issue(Operation::SoftwarePage, {});
    if (status_.state == State::EulaPage) return issue(Operation::EulaPage, {});
    if (status_.state == State::EulaAccept) return issue(Operation::EulaAccept, {});
    if (!owner_) return std::nullopt;
    auto next = owner_->nextRequest();
    sync();
    if (!next) return std::nullopt;
    if (auto* create = std::get_if<session::CreateJob>(&next->body))
        return issue(Operation::Create, std::move(create->bodyJson), next->id);
    const bool cancel = std::holds_alternative<session::CancelJob>(next->body);
    const auto& id = cancel ? std::get<session::CancelJob>(next->body).gameSessionJobId :
        std::get<session::GetJob>(next->body).gameSessionJobId;
    return issue(cancel ? Operation::Cancel : Operation::Poll, Json{{"gameSessionJobId", id}}.dump(), next->id);
}
void NativeSession::complete(std::uint64_t id, long http, const std::string& body, std::int64_t now)
{
    if (!pending_ || pending_->id != id) return;
    if (status_.state == State::Ending && (now < now_ || !auth_.selectedProfile_ ||
        *auth_.selectedProfile_ >= auth_.profiles_.size() || auth_.profileToken_.empty() ||
        auth_.profiles_[*auth_.selectedProfile_].id != launchProfile_))
    { fail(Block::InvalidResponse); return; }
    const auto pending = *pending_; pending_.reset(); now_ = now;
    status_.httpStatus = http; status_.refreshing = false;
    try
    {
        if (now >= deadline_) { fail(Block::Expired); return; }
        if (http != 200)
        {
            // A lost/denied create is never replayed. Refresh proactively before
            // expiry; unexpected authorization failures end this attempt.
            if (owner_ && pending.ownerId) owner_->transportFailed(pending.ownerId);
            sync(); classifyHttpError(status_, pending.operation, body);
            fail(Block::Http); return;
        }
        if (pending.operation == Operation::SoftwarePage || pending.operation == Operation::EulaPage)
        {
            const bool eula = pending.operation == Operation::EulaPage;
            auth_.readNoticePage(body, softwareNotice_, status_.softwareNotice, softwareActions_, !eula, eula);
            status_.state = eula ? State::EulaNotice : State::SoftwareNotice;
            return;
        }
        const auto value = object(body);
        const auto code = number(value, "errorCode").value_or(0);
        status_.apiErrorCode = code;
        if (pending.operation == Operation::End)
        {
            if (code || value.contains("error") || value.contains("errorType") || value.contains("__type") ||
                (value.contains("failedIds") && !value["failedIds"].is_null() &&
                (!value["failedIds"].is_array() || !value["failedIds"].empty())))
            { fail(Block::InvalidResponse); return; }
            status_.state = State::Ended; status_.remoteWorkMayRemain = false;
            owner_.reset(); wipe(allocatedSession_);
            return;
        }
        if (pending.operation == Operation::EulaAccept)
        {
            // A 200 can still contain rejected preferences. Do not replay a
            // write or start provisioning after an ambiguous/failed response.
            if (code || value.contains("error") || value.contains("errorType") ||
                (value.contains("failedPreferences") && !value["failedPreferences"].is_null() &&
                (!value["failedPreferences"].is_array() || !value["failedPreferences"].empty())))
            { fail(Block::InvalidResponse); return; }
            status_.eulaAccepted = true;
            clearNoticeContent(); status_.softwareNotice = {};
            status_.state = State::CheckingPin;
            return;
        }
        if (pending.operation == Operation::Refresh)
        {
            auto token = text(value, "access_token", 32768);
            auto lifetime = value.at("expires_in");
            if (lifetime.is_string())
            {
                const auto digits = lifetime.get<std::string>();
                if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("Refresh lifetime");
                lifetime = std::stoll(digits);
            }
            const auto seconds = number(Json{{"lifetime", lifetime}}, "lifetime").value_or(0);
            if (seconds < 120 || seconds > 86400 || code) throw std::invalid_argument("Refresh lifetime");
            auto refresh = value.contains("refresh_token") ? text(value, "refresh_token", 32768) : std::string{};
            wipe(auth_.accessToken_); auth_.accessToken_ = std::move(token);
            if (!refresh.empty()) auth_.replaceRefreshToken(std::move(refresh));
            auth_.accessExpiresAt_ = now + seconds * 1000;
            return;
        }
        if (pending.operation == Operation::Pin)
        {
            if (code) { fail(Block::InvalidResponse); return; }
            const auto challenge = text(value, "challenge", 16);
            if (challenge == "ALLOW") status_.state = !eulaToken_.empty() && !status_.eulaAccepted ? State::EulaPage :
                softwareToken_.empty() || status_.softwareNotice.accepted ? State::AwaitOffer : State::SoftwarePage;
            else if (challenge == "PIN" || challenge == "DENY")
            {
                status_.state = State::Blocked;
                status_.block = challenge == "PIN" ? Block::ParentalPin : Block::ParentalDenied;
                wipe(payload_);
            }
            else fail(Block::InvalidResponse);
            return;
        }
        if (!owner_) throw std::invalid_argument("Missing job owner");
        if (pending.operation == Operation::Create)
        {
            session::CreatedJob result; result.errorCode = code;
            if (!code)
            {
                result.gameSessionJobId = text(value, "gameSessionJobId");
                result.startPollingAfterMs = number(value, "startPollingAfterMs");
            }
            owner_->complete(pending.ownerId, result);
        }
        else if (pending.operation == Operation::Poll)
        {
            session::JobUpdate result; result.errorCode = code;
            if (!code)
            {
                result.gameSessionJobId = text(value, "gameSessionJobId");
                result.status = text(value, "status", 64);
                result.retryAfterMs = number(value, "retryAfterMs");
                result.positionInLine = number(value, "positionInLine");
                result.expectedWaitTimeSeconds = number(value, "expectedWaitTimeSeconds");
                if (value.contains("gameSession") && !value["gameSession"].is_null())
                {
                    const auto& game = value["gameSession"];
                    if (game.value("protocol", "WEB_RTC") != "WEB_RTC" || game.value("transport", "UDP") != "UDP")
                        throw std::invalid_argument("Unsupported transport");
                    session::Answer answer;
                    answer.sessionId = text(game, "sessionId"); answer.connectionId = text(game, "connectionId");
                    answer.signalingResponse = normalizeAnswer(game);
                    result.gameSession = std::move(answer);
                }
            }
            const bool accepted = owner_->complete(pending.ownerId, result);
            if (accepted && owner_->status().state == session::State::Ready && result.gameSession)
                allocatedSession_ = result.gameSession->sessionId;
        }
        else
        {
            session::CancelledJob result; result.errorCode = code;
            // An ACK is not proof that an already allocated game was terminated.
            owner_->complete(pending.ownerId, result);
        }
        sync();
    }
    catch (...)
    {
        if (owner_ && pending.ownerId) owner_->transportFailed(pending.ownerId);
        sync(); fail(Block::InvalidResponse);
    }
}
void NativeSession::transportFailed(std::uint64_t id, std::int64_t now)
{
    if (!pending_ || pending_->id != id) return;
    now_ = now;
    if (owner_ && pending_->ownerId) owner_->transportFailed(pending_->ownerId);
    pending_.reset(); sync(); fail(Block::Transport);
}
void NativeSession::cancel(std::int64_t now)
{
    now_ = now;
    if (status_.state == State::Ending) { fail(Block::Transport); return; }
    if (owner_ && status_.state == State::Provisioning) { owner_->cancel(); sync(); }
    else if (status_.state != State::Ready)
    {
        pending_.reset(); wipe(payload_); wipe(eulaToken_); wipe(eulaProduct_); clearSoftwareNotice();
        status_.refreshing = false; status_.state = State::Cancelled;
    }
}
bool NativeSession::canEnd() const
{
    return (status_.state == State::Ready || status_.state == State::EndFailed) && !pending_ &&
        !allocatedSession_.empty() && !launchProfile_.empty() && !auth_.profileToken_.empty() &&
        auth_.selectedProfile_ && *auth_.selectedProfile_ < auth_.profiles_.size() &&
        auth_.profiles_[*auth_.selectedProfile_].id == launchProfile_;
}
bool NativeSession::endGame(std::int64_t now)
{
    if (!canEnd() || now < now_ || now > std::numeric_limits<std::int64_t>::max() - 20'000) return false;
    now_ = now; deadline_ = now + 20'000;
    status_.state = State::Ending; status_.block = Block::None;
    status_.httpStatus = 0; status_.apiErrorCode = 0; status_.httpDiagnostic = {};
    return true;
}
std::optional<std::string> NativeSession::takeAnswer()
{
    if (!owner_ || status_.state != State::Ready) return std::nullopt;
    auto answer = owner_->takeAnswer();
    if (!answer) return std::nullopt;
    auto result = object(answer->signalingResponse)["sdp"].get<std::string>();
    wipe(answer->signalingResponse);
    // The UI now owns the remote allocation; closing RTC is not termination.
    status_.remoteWorkMayRemain = true;
    return result;
}
}
