#include "luna_session.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace luna::session
{
const char* errorLabel(Error error)
{
    switch (error)
    {
    case Error::None: return "none";
    case Error::Api: return "api";
    case Error::Transport: return "transport";
    case Error::InvalidResponse: return "invalid_response";
    case Error::InvalidStatus: return "invalid_status";
    case Error::InvalidDelay: return "invalid_delay";
    case Error::JobFailed: return "COMPLETED_FAILED";
    case Error::RequestTimeout: return "request_timeout";
    case Error::OverallTimeout: return "overall_timeout";
    case Error::CancellationTimeout: return "cancellation_timeout";
    case Error::Clock: return "clock";
    }
    return "unknown";
}
namespace
{
std::int64_t after(std::int64_t now, std::int64_t delay)
{
    return now > std::numeric_limits<std::int64_t>::max() - delay
        ? std::numeric_limits<std::int64_t>::max() : now + delay;
}
bool identifier(const std::string& value)
{
    return !value.empty() && value.size() <= ProvisioningSession::MaxIdentifierBytes &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool activeStatus(const std::string& status)
{
    return status == "ACTIVE_REQUESTED" || status == "ACTIVE_QUEUED" ||
        status == "ACTIVE_CREATING_SESSION" || status == "ACTIVE_CONNECTING" ||
        status == "ACTIVE_TIDYING_UP";
}
bool validAnswer(const Answer& answer)
{
    return identifier(answer.sessionId) && identifier(answer.connectionId) &&
        !answer.signalingResponse.empty() &&
        answer.signalingResponse.size() <= ProvisioningSession::MaxAnswerBytes;
}
}
ProvisioningSession::ProvisioningSession(Clock clock)
    : ProvisioningSession(std::move(clock), Options{}) {}
ProvisioningSession::ProvisioningSession(Clock clock, Options options)
    : clock_(std::move(clock)), options_(options)
{
    if (!clock_ || options.requestTimeoutMs <= 0 || options.overallTimeoutMs <= 0 ||
        options.cancellationTimeoutMs <= 0 || options.maxPollDelayMs < DefaultPollDelayMs)
        throw std::invalid_argument("Invalid provisioning clock or scheduling options");
}
bool ProvisioningSession::active() const
{
    return status_.state == State::Creating || status_.state == State::WaitingToPoll ||
        status_.state == State::Polling || status_.state == State::Cancelling;
}
void ProvisioningSession::start(CreateJob request)
{
    if (status_.state != State::Idle)
        throw std::logic_error("Provisioning owner already used");
    if (request.bodyJson.empty() || request.bodyJson.size() > MaxCreateBytes)
        throw std::invalid_argument("Invalid create request size");
    const auto now = clock_();
    if (now < 0) throw std::invalid_argument("Negative provisioning clock");
    create_ = std::move(request);
    lastNow_ = now;
    overallDeadline_ = after(now, options_.overallTimeoutMs);
    status_.state = State::Creating;
}
void ProvisioningSession::clearQueue()
{
    status_.positionInLine.reset();
    status_.expectedWaitTimeSeconds.reset();
}
void ProvisioningSession::fail(Error error, std::int64_t apiErrorCode)
{
    pending_.reset();
    create_.bodyJson.clear();
    answer_.reset();
    clearQueue();
    status_.state = State::Failed;
    status_.error = error;
    status_.apiErrorCode = apiErrorCode;
}
std::optional<std::int64_t> ProvisioningSession::advance()
{
    if (!active()) return std::nullopt;
    const auto now = clock_();
    if (now < lastNow_)
    {
        fail(Error::Clock);
        return std::nullopt;
    }
    lastNow_ = now;
    if (cancelling_ ? now >= cancelDeadline_ : now >= overallDeadline_)
    {
        fail(cancelling_ ? Error::CancellationTimeout : Error::OverallTimeout);
        return std::nullopt;
    }
    if (pending_ && now >= pending_->deadline)
    {
        // A timed-out poll cannot create another job. We can still attempt to
        // cancel the known job, but never replay an ambiguous create request.
        if (cancelling_ && pending_->operation == Operation::Poll)
            pending_.reset();
        else
        {
            fail(Error::RequestTimeout);
            return std::nullopt;
        }
    }
    return now;
}
Request ProvisioningSession::issue(RequestBody body, Operation operation, std::int64_t now)
{
    Request request{nextId_++, std::move(body)};
    pending_ = Pending{request.id, operation, after(now, options_.requestTimeoutMs)};
    return request;
}
std::optional<Request> ProvisioningSession::nextRequest()
{
    const auto now = advance();
    if (!now || pending_) return std::nullopt;
    if (cancelling_)
    {
        if (!status_.gameSessionJobId.empty())
            return issue(CancelJob{status_.gameSessionJobId}, Operation::Cancel, *now);
        return std::nullopt;
    }
    if (status_.state == State::Creating)
    {
        auto request = issue(std::move(create_), Operation::Create, *now);
        create_.bodyJson.clear();
        status_.remoteWorkMayRemain = true;
        return request;
    }
    if (status_.state == State::WaitingToPoll && *now >= nextPoll_)
    {
        auto request = issue(GetJob{status_.gameSessionJobId}, Operation::Poll, *now);
        status_.state = State::Polling;
        return request;
    }
    return std::nullopt;
}
bool ProvisioningSession::schedulePoll(std::optional<std::int64_t> delay, bool first, std::int64_t now)
{
    const auto wait = !delay || (!first && *delay == 0) ? DefaultPollDelayMs : *delay;
    if (wait < 0 || wait > options_.maxPollDelayMs)
    {
        fail(Error::InvalidDelay);
        return false;
    }
    nextPoll_ = after(now, wait);
    status_.state = State::WaitingToPoll;
    return true;
}
void ProvisioningSession::created(const CreatedJob& response, std::int64_t now)
{
    if (response.errorCode)
    {
        status_.remoteWorkMayRemain = false; // explicit create rejection
        fail(Error::Api, response.errorCode);
        return;
    }
    if (!identifier(response.gameSessionJobId))
    {
        fail(Error::InvalidResponse);
        return;
    }
    status_.gameSessionJobId = response.gameSessionJobId;
    if (!cancelling_) schedulePoll(response.startPollingAfterMs, true, now);
}
void ProvisioningSession::polled(const JobUpdate& response, std::int64_t now)
{
    if (response.errorCode)
    {
        if (!cancelling_) fail(Error::Api, response.errorCode);
        return; // while cancelling, still send the explicit job cancellation
    }
    if (response.gameSessionJobId != status_.gameSessionJobId)
    {
        if (!cancelling_) fail(Error::InvalidResponse);
        return;
    }
    if (!activeStatus(response.status) && response.status != "COMPLETED_SUCCEEDED" &&
        response.status != "COMPLETED_FAILED")
    {
        if (!cancelling_) fail(Error::InvalidStatus);
        return;
    }
    status_.jobStatus = response.status;
    if (cancelling_)
    {
        if (response.status == "COMPLETED_SUCCEEDED" && response.gameSession &&
            identifier(response.gameSession->sessionId) && identifier(response.gameSession->connectionId))
            status_.allocatedDuringCancellation = SessionIds{response.gameSession->sessionId,
                response.gameSession->connectionId};
        return;
    }
    if (activeStatus(response.status))
    {
        if (response.gameSession)
        {
            fail(Error::InvalidResponse);
            return;
        }
        clearQueue();
        if (response.status == "ACTIVE_QUEUED")
        {
            if ((response.positionInLine && *response.positionInLine < 0) ||
                (response.expectedWaitTimeSeconds && *response.expectedWaitTimeSeconds < 0))
            {
                fail(Error::InvalidResponse);
                return;
            }
            status_.positionInLine = response.positionInLine;
            status_.expectedWaitTimeSeconds = response.expectedWaitTimeSeconds;
        }
        schedulePoll(response.retryAfterMs, false, now);
    }
    else if (response.status == "COMPLETED_FAILED")
    {
        if (response.gameSession)
        {
            fail(Error::InvalidResponse);
            return;
        }
        status_.remoteWorkMayRemain = false;
        fail(Error::JobFailed);
    }
    else if (response.status == "COMPLETED_SUCCEEDED")
    {
        if (!response.gameSession || !validAnswer(*response.gameSession))
        {
            fail(Error::InvalidResponse);
            return;
        }
        answer_ = response.gameSession;
        clearQueue();
        status_.state = State::Ready;
    }
    else fail(Error::InvalidStatus);
}
bool ProvisioningSession::complete(std::uint64_t requestId, const Response& response)
{
    if (!pending_ || pending_->id != requestId) return false;
    const auto now = advance();
    if (!now || !pending_ || pending_->id != requestId) return false;
    const auto operation = pending_->operation;
    pending_.reset();
    if (operation == Operation::Create)
    {
        if (const auto result = std::get_if<CreatedJob>(&response)) created(*result, *now);
        else fail(Error::InvalidResponse);
    }
    else if (operation == Operation::Poll)
    {
        if (const auto result = std::get_if<JobUpdate>(&response)) polled(*result, *now);
        else if (!cancelling_) fail(Error::InvalidResponse);
    }
    else if (const auto result = std::get_if<CancelledJob>(&response))
    {
        if (result->errorCode) fail(Error::Api, result->errorCode);
        else
        {
            status_.state = State::Cancelled;
            status_.cancellationConfirmed = true;
            status_.remoteWorkMayRemain = !result->jobCancelledBeforeAllocation ||
                status_.allocatedDuringCancellation.has_value() ||
                status_.jobStatus == "COMPLETED_SUCCEEDED";
        }
    }
    else fail(Error::InvalidResponse);
    return true;
}
bool ProvisioningSession::transportFailed(std::uint64_t requestId)
{
    if (!pending_ || pending_->id != requestId || !advance() || !pending_ || pending_->id != requestId)
        return false;
    const auto operation = pending_->operation;
    pending_.reset();
    if (!(cancelling_ && operation == Operation::Poll)) fail(Error::Transport);
    return true;
}
bool ProvisioningSession::cancel()
{
    if (status_.state == State::Idle)
    {
        status_.state = State::Cancelled;
        return true;
    }
    if (!active()) return false;
    const auto now = advance();
    if (!now) return false;
    if (cancelling_) return true; // repeated calls never extend the deadline
    if (status_.state == State::Creating && !pending_)
    {
        create_.bodyJson.clear();
        status_.state = State::Cancelled;
        return true;
    }
    cancelling_ = true;
    cancelDeadline_ = after(*now, options_.cancellationTimeoutMs);
    clearQueue();
    status_.state = State::Cancelling;
    return true;
}
std::optional<Answer> ProvisioningSession::takeAnswer()
{
    if (status_.state != State::Ready || !answer_) return std::nullopt;
    auto answer = std::move(answer_);
    answer_.reset();
    status_.remoteWorkMayRemain = false; // remote session ownership moves to caller
    return answer;
}
}
