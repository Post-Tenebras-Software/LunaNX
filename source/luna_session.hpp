#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace luna::session
{
// Transport boundary only: the future authenticated adapter owns JSON parsing,
// HTTP, and aborting requests. No token or SDP contents belong in diagnostics.
struct CreateJob { std::string bodyJson; };
struct GetJob { std::string gameSessionJobId; };
struct CancelJob { std::string gameSessionJobId; };
using RequestBody = std::variant<CreateJob, GetJob, CancelJob>;
struct Request { std::uint64_t id; RequestBody body; };

struct ApiResult { std::int64_t errorCode = 0; };
struct CreatedJob : ApiResult
{
    std::string gameSessionJobId;
    std::optional<std::int64_t> startPollingAfterMs;
};
struct SessionIds { std::string sessionId, connectionId; };
struct Answer : SessionIds
{
    // Original JSON string representing the SDP answer. The RTC adapter must
    // parse/validate it and apply it to the peer that supplied the create body.
    std::string signalingResponse;
};
struct JobUpdate : ApiResult
{
    std::string gameSessionJobId, status;
    std::optional<std::int64_t> retryAfterMs;
    std::optional<std::int64_t> positionInLine, expectedWaitTimeSeconds;
    std::optional<Answer> gameSession;
};
struct CancelledJob : ApiResult
{
    // Semantic adapter result, not an observed Luna response field. Only set
    // when the transport can establish that no game was allocated.
    bool jobCancelledBeforeAllocation = false;
};
using Response = std::variant<CreatedJob, JobUpdate, CancelledJob>;

enum class State { Idle, Creating, WaitingToPoll, Polling, Cancelling, Ready, Failed, Cancelled };
enum class Error
{
    None, Api, Transport, InvalidResponse, InvalidStatus, InvalidDelay,
    JobFailed, RequestTimeout, OverallTimeout, CancellationTimeout, Clock
};
const char* errorLabel(Error error);
struct Status
{
    State state = State::Idle;
    Error error = Error::None;
    std::int64_t apiErrorCode = 0;
    std::string gameSessionJobId, jobStatus;
    std::optional<std::int64_t> positionInLine, expectedWaitTimeSeconds;
    bool cancellationConfirmed = false;
    // True while this owner has possible/known remote work. A timeout does not
    // prove a create failed; a cancel-job ACK does not terminate an allocated game.
    bool remoteWorkMayRemain = false;
    std::optional<SessionIds> allocatedDuringCancellation;
};

// One attempt per object, owned by one thread. A request returned by nextRequest
// is considered dispatched immediately. Marshal network results to this owner.
class ProvisioningSession
{
public:
    using Clock = std::function<std::int64_t()>; // monotonic milliseconds
    struct Options
    {
        std::int64_t requestTimeoutMs = 10'000;
        std::int64_t overallTimeoutMs = 600'000;
        std::int64_t cancellationTimeoutMs = 20'000;
        std::int64_t maxPollDelayMs = 60'000;
    };
    explicit ProvisioningSession(Clock clock);
    ProvisioningSession(Clock clock, Options options);
    ProvisioningSession(const ProvisioningSession&) = delete;
    ProvisioningSession& operator=(const ProvisioningSession&) = delete;
    ProvisioningSession(ProvisioningSession&&) = delete;
    ProvisioningSession& operator=(ProvisioningSession&&) = delete;
    void start(CreateJob request);
    std::optional<Request> nextRequest();
    // False means stale, duplicate, unknown, or expired request: no result applied.
    bool complete(std::uint64_t requestId, const Response& response);
    bool transportFailed(std::uint64_t requestId);
    // Waits for in-flight create/poll within a bounded cancellation window.
    // Ready/terminal owners return false; they never implicitly terminate a game.
    bool cancel();
    std::optional<Answer> takeAnswer(); // one delivery, after Ready only
    const Status& status() const { return status_; }

    static constexpr std::size_t MaxCreateBytes = 65'536;
    static constexpr std::size_t MaxAnswerBytes = 65'536;
    static constexpr std::size_t MaxIdentifierBytes = 512;
    static constexpr std::int64_t DefaultPollDelayMs = 1'500;

private:
    enum class Operation { Create, Poll, Cancel };
    struct Pending { std::uint64_t id; Operation operation; std::int64_t deadline; };
    bool active() const;
    std::optional<std::int64_t> advance();
    Request issue(RequestBody body, Operation operation, std::int64_t now);
    void fail(Error error, std::int64_t apiErrorCode = 0);
    void clearQueue();
    bool schedulePoll(std::optional<std::int64_t> delay, bool first, std::int64_t now);
    void created(const CreatedJob& response, std::int64_t now);
    void polled(const JobUpdate& response, std::int64_t now);
    Clock clock_;
    Options options_;
    Status status_;
    CreateJob create_;
    std::optional<Pending> pending_;
    std::optional<Answer> answer_;
    std::uint64_t nextId_ = 1;
    bool cancelling_ = false;
    std::int64_t lastNow_ = 0, overallDeadline_ = 0, cancelDeadline_ = 0, nextPoll_ = 0;
};
}
