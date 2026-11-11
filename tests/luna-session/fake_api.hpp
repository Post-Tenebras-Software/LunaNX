#pragma once

#include "luna_session.hpp"

#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

namespace session_test
{
namespace s = luna::session;
inline const std::string CreateBody = R"({"gameId":"fake-game","playToken":"fake-play-token","signalingRequest":"fake-offer"})";
inline const std::string AnswerJson = R"({"type":"answer","sdp":"v=0\r\ns=fake-answer\r\n"})";

inline s::CreatedJob created(std::optional<std::int64_t> delay = 0)
{
    s::CreatedJob result;
    result.gameSessionJobId = "fake-job";
    result.startPollingAfterMs = delay;
    return result;
}
inline s::JobUpdate update(std::string status = "COMPLETED_SUCCEEDED",
    std::optional<std::int64_t> delay = std::nullopt)
{
    s::JobUpdate result;
    result.gameSessionJobId = "fake-job";
    result.status = std::move(status);
    result.retryAfterMs = delay;
    if (result.status == "COMPLETED_SUCCEEDED")
    {
        s::Answer answer;
        answer.sessionId = "fake-session";
        answer.connectionId = "fake-connection";
        answer.signalingResponse = AnswerJson;
        result.gameSession = std::move(answer);
    }
    return result;
}
inline s::CancelledJob cancelledBeforeAllocation()
{
    s::CancelledJob result;
    result.jobCancelledBeforeAllocation = true;
    return result;
}

// Entirely local, typed API substitute: no HTTP, JSON parser, credentials or RTC.
class FakeApi
{
public:
    std::deque<s::JobUpdate> polls;
    std::vector<s::Request> sent;
    bool allocated = false;
    s::Response respond(const s::Request& request)
    {
        sent.push_back(request);
        if (const auto create = std::get_if<s::CreateJob>(&request.body))
        {
            if (create->bodyJson != CreateBody) throw std::runtime_error("Create body changed");
            return created(200);
        }
        const auto job = std::get_if<s::GetJob>(&request.body);
        if ((job ? job->gameSessionJobId : std::get<s::CancelJob>(request.body).gameSessionJobId) != "fake-job")
            throw std::runtime_error("Request used another job ID");
        if (!job)
        {
            auto result = cancelledBeforeAllocation();
            result.jobCancelledBeforeAllocation = !allocated;
            return result;
        }
        if (polls.empty()) throw std::runtime_error("Unexpected extra poll");
        auto response = std::move(polls.front());
        polls.pop_front();
        if (response.status == "COMPLETED_SUCCEEDED") allocated = true;
        return response;
    }
};
class FakePeer
{
public:
    bool live = true;
    unsigned answers = 0;
    void apply(const s::Answer& answer)
    {
        if (!live || answers || answer.sessionId != "fake-session" ||
            answer.connectionId != "fake-connection" || answer.signalingResponse != AnswerJson)
            throw std::runtime_error("Answer did not reach its original live peer exactly once");
        ++answers;
    }
};
}
