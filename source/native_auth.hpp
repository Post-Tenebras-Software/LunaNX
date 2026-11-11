#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace luna::auth
{
inline constexpr std::size_t MaxAuthResponse = 128 * 1024;
inline constexpr std::size_t MaxLunaResponse = 2 * 1024 * 1024;
enum class Step { Idle, CodePair, Register, Refresh, LunaPage, NegativeControl, Complete, Failed, Cancelled,
    AppConfig, ProfilesPage, ChooseProfile, PlayerLogin, Catalog, CatalogReady,
    DetailPage, DetailReady, LaunchWidget, ExperimentalPage, ExperimentalReady, NativeSession, NativeNotice, SearchPage, BrowsePage, ProfileManagement, CatalogAccess, ClientIdentity };
enum class Error { None, Http, Response, Transport, Expired, AuthorizationNotProven };
enum class Mode { ProofOnly, ProfilesAndCatalog };
enum class AuthError { None, Unknown, InvalidValue, MissingValue, InvalidRequest, InvalidClient,
    UnauthorizedClient, Unauthorized, AccessDenied, InvalidGrant, InvalidCodePair, ExpiredCodePair,
    InvalidData, ServerError, SlowDown, InvalidToken, DuplicateDeviceName };
enum class AuthResponse { None, Empty, Unparseable, NoErrorField, TopLevelError, NestedError, TooLarge };
enum class AuthParameter { None, Unknown, CodePair, PublicCode, PrivateCode, RegistrationData,
    DeviceSerial, DeviceType, SourceToken };
const char* authErrorLabel(AuthError error);
const char* authParameterLabel(AuthParameter parameter);
const char* authResponseLabel(AuthResponse response);
enum class ProfileBlock { None, PinRequired, Migration, NotInFamily, ImportRequired };
enum class ProfileParse { None, Json, Page, Widgets, Presentation, Actions, Identity, Name, Flags, Limit, NoProfiles, Complete };
const char* profileParseLabel(ProfileParse stage);
struct ProfileDiagnostics
{
    ProfileParse stage = ProfileParse::None;
    std::size_t widgets = 0, tiles = 0, encodedActions = 0, parsed = 0;
};
enum class CatalogParse { None, Json, Page, Widgets, Presentation, Identity, Title, Limit, Complete,
    BrowseContext, BrowseQuery, BrowseSort, BrowseResults, Access };
const char* catalogParseLabel(CatalogParse stage);
struct CatalogDiagnostics
{
    CatalogParse stage = CatalogParse::None;
    std::size_t widgets = 0, tiles = 0, horizontal = 0, vertical = 0, parsed = 0;
    bool browse = false;
    bool navigationToken = false, navigationQuery = false, filterWidget = false, defaultSearchOnLoad = false;
    std::size_t accessChecked=0, accessIncluded=0, accessHidden=0;
    unsigned accessFieldHints=0;
    bool accessValidation=false, accessSingleFallback=false;
};
struct Profile
{
    std::string id, name;
    ProfileBlock block = ProfileBlock::None;
    std::string artwork = {};
};
struct Game
{
    std::string id, title;
    bool detailsAvailable = false;
    std::string artwork;
};
// Search text is local display data, intentionally excluded from log-safe Status.
struct BrowseState
{
    bool active = false, hasNext = false;
    bool includedOnly = false;
    std::size_t offset = 0;
    std::string query;
    Error error = Error::None;
};
inline constexpr std::size_t SearchPageSize = 50;
inline constexpr std::size_t MaxSearchOffset = 9950;
inline constexpr std::size_t MaxSearchQueryBytes = 200;
enum class DetailParse { None, Json, Page, Widgets, Presentation, Actions, LaunchPayload, Complete };
enum class LaunchAction { None, GameSession, DirectLaunch, Other, Ambiguous, PageAction, UnsupportedOs, ExperimentalClient, AcceptEula };
const char* detailParseLabel(DetailParse stage);
const char* launchActionLabel(LaunchAction action);
struct DetailDiagnostics
{
    DetailParse stage = DetailParse::None;
    LaunchAction action = LaunchAction::None;
    LaunchAction primaryAction = LaunchAction::None;
    Error error = Error::None;
    std::size_t widgets = 0, products = 0, buttons = 0;
    std::size_t pageActions = 0, blockingPageActions = 0;
    bool hasGameId = false, hasPlayToken = false;
};
struct GameDetail
{
    std::string title, description, buttonText;
    std::string primaryActionTarget;
    // Validated action identifiers for the Switch debug screen only.
    std::vector<std::string> pageActionTargets;
    bool hasArtwork = false, hasTrailer = false;
    std::string artwork;
    std::vector<std::string> trailers;
    std::vector<std::string> screenshots;
};
struct NoticeChoice
{
    std::string label, actionTarget;
    LaunchAction action = LaunchAction::None;
};
struct ExperimentalNotice
{
    std::string text;
    std::vector<NoticeChoice> choices;
    std::vector<std::string> unsupportedWidgetTypes;
    // Validated identifiers and numeric positions for the local screen only.
    std::vector<std::string> unsupportedActionTargets;
};
enum class NoticeIssue { None, Widget, Checkbox, LegalTerms, SecondaryAction };
enum class NoticeAction { None, Unknown, Dismiss, GameSession, DirectLaunch, LocalStorageToggle, DoNotShowAgain };
const char* noticeIssueLabel(NoticeIssue issue);
const char* noticeActionLabel(NoticeAction action);
struct NoticeDiagnostics
{
    DetailParse stage = DetailParse::None;
    std::size_t widgets = 0, textBlocks = 0, buttons = 0, unsupported = 0, blockingActions = 0;
    bool complete = false, accepted = false;
    NoticeIssue firstIssue = NoticeIssue::None;
    NoticeAction firstUnsupportedAction = NoticeAction::None;
    NoticeAction checkboxAction = NoticeAction::None;
    bool checkboxPreferenceSupported = false;
    std::size_t unsupportedWidgets = 0, checkboxes = 0, legalTerms = 0, secondaryActions = 0;
    // One-based button/action positions; zero means no secondary-action issue.
    std::size_t firstIssueButton = 0, firstIssueAction = 0;
};
struct Status
{
    Step step = Step::Idle;
    Error error = Error::None;
    Step failedAt = Step::Idle;
    long httpStatus = 0;
    std::string publicCode;
    bool registered = false, refreshed = false, lunaAccepted = false, restoredLogin = false;
    bool clientIdentityReady = false;
    ProfileBlock profileBlock = ProfileBlock::None;
    AuthError authError = AuthError::None;
    AuthParameter authParameter = AuthParameter::None;
    AuthResponse authResponse = AuthResponse::None;
    std::size_t authResponseBytes = 0;
    unsigned approvalPolls = 0;
    std::int64_t codeAgeSeconds = 0, codeLifetimeSeconds = 0;
    ProfileDiagnostics profileParsing;
    CatalogDiagnostics catalogParsing;
    DetailDiagnostics detailParsing;
    NoticeDiagnostics noticeParsing;
};
struct Request
{
    std::uint64_t id;
    Step step;
    std::string url, body;
    std::vector<std::string> headers;
};
std::size_t responseLimit(Step step);

// One authentication session. Status is safe for logs;
// profile/game display data belongs only on the local screen.
class NativeAuthProbe
{
public:
    ~NativeAuthProbe();
    explicit NativeAuthProbe(Mode mode = Mode::ProofOnly, bool clientIdentity = false) :
        mode_(mode), clientIdentityEnabled_(clientIdentity) {}
    NativeAuthProbe(const NativeAuthProbe&) = delete;
    NativeAuthProbe& operator=(const NativeAuthProbe&) = delete;
    void start(std::string serial, std::int64_t nowMs);
    std::optional<Request> nextRequest(std::int64_t nowMs);
    void complete(std::uint64_t id, long httpStatus, const std::string& body, std::int64_t nowMs);
    void transportFailed(std::uint64_t id);
    void cancel();
    bool chooseProfile(std::size_t index, std::int64_t nowMs);
    void returnToProfiles();
    bool browseGames(const std::string& query, std::size_t offset, std::int64_t nowMs, bool includedOnly=false);
    void returnToHome();
    const BrowseState& browsing() const { return browsing_; }
    bool openGame(std::size_t index, std::int64_t nowMs);
    bool openGamePath(const std::string& path, const std::string& title, std::int64_t nowMs);
    const std::string& currentGamePath() const { return detailPath_; }
    bool inspectLaunch(std::int64_t nowMs);
    bool chooseNoticeButton(std::size_t index, std::int64_t nowMs);
    bool acknowledgeUnsupportedClient(std::int64_t nowMs);
    void returnToDetail();
    void returnToCatalog();
    const Status& status() const { return status_; }
    // Account display data is for the local screen, never terminal diagnostics.
    const std::vector<Profile>& profiles() const { return profiles_; }
    const std::vector<Game>& games() const { return games_; }
    const GameDetail& detail() const { return detail_; }
    const ExperimentalNotice& notice() const { return notice_; }
    // Unmapped server identifier for the local debug screen only. Never log it.
    const std::string& localAuthErrorIdentifier() const { return localAuthErrorIdentifier_; }

private:
    friend class NativeSession;
    friend class RememberedLogin;
    friend class ProfileManager;
    void restoreLogin(std::string serial, std::string refresh, std::int64_t nowMs);
    void replaceRefreshToken(std::string token);
    void proxyHeaders(Request& request, bool profile, bool negative = false) const;
    std::string proxyContext() const;
    void fail(Error error, long httpStatus = 0);
    void clearSecrets();
    void clearGames();
    void clearDetail();
    void failDetail(Error error, long httpStatus = 0);
    void failBrowse(Error error, long httpStatus = 0);
    void clearBrowseCandidates();
    void finishBrowse();
    void acceptLaunchPayload(const std::string& payload);
    void acceptDetail(const std::string& body);
    void acceptExperimentalPage(const std::string& body);
    void readNoticePage(const std::string& body, ExperimentalNotice& notice,
        NoticeDiagnostics& diagnostics, std::vector<std::string>& actions, bool softwareWarning = false, bool eula = false);
    void clearNoticeActions();
    std::string localActionTarget(const std::string& target) const;
    Mode mode_;
    bool clientIdentityEnabled_ = false;
    Status status_;
    std::string serial_, privateCode_, accessToken_, refreshToken_, profileToken_, clientTdi_;
    std::uint64_t credentialRevision_ = 0;
    std::string localAuthErrorIdentifier_;
    std::string profilePageType_, profilePageId_;
    std::vector<Profile> profiles_;
    std::vector<Game> games_;
    struct PageLink { std::string serviceToken, path; };
    std::vector<PageLink> gameLinks_;
    std::vector<Game> homeGames_;
    std::vector<PageLink> homeLinks_;
    std::vector<Game> browseCandidates_;
    std::vector<PageLink> browseCandidateLinks_;
    std::vector<bool> browseIncluded_;
    std::vector<std::string> browseRawIds_;
    std::size_t accessOffset_=0;
    std::size_t accessBatchSize_=25;
    std::string browseAccessStage_;
    bool requestedIncludedOnly_=false, candidateHasNext_=false;
    BrowseState browsing_;
    std::string requestedQuery_;
    std::string browseRequest_;
    std::string browsePageToken_, browseQuickSearch_;
    std::size_t requestedOffset_ = 0;
    std::optional<std::size_t> selectedGame_;
    std::string detailPath_;
    GameDetail detail_;
    ExperimentalNotice notice_;
    std::string experimentalPageToken_;
    std::vector<std::string> noticeActions_;
    bool detailLoaded_ = false;
    // Navigation/launch credentials never leave the core's private state.
    std::string launchWidgetToken_, launchGameId_, playToken_, productStage_, launchPayload_;
    std::int64_t accessExpiresAt_ = 0;
    std::optional<std::size_t> selectedProfile_;
    std::int64_t deadline_ = 0, nextPoll_ = 0, interval_ = 0;
    std::int64_t sessionDeadline_ = 0;
    std::int64_t codeIssuedAt_ = 0;
    std::optional<std::uint64_t> pending_;
    std::uint64_t nextId_ = 1;
};
}
