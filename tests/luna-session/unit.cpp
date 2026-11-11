#include "fake_api.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace session_test;
static_assert(!std::is_copy_constructible_v<s::ProvisioningSession>);
static_assert(!std::is_move_constructible_v<s::ProvisioningSession>);
namespace
{
unsigned checks = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template <typename Exception = std::invalid_argument, typename F> void rejects(F action)
{
    bool rejected = false;
    try { action(); }
    catch (const Exception&) { rejected = true; }
    check(rejected, "Invalid operation was accepted");
}
struct Rig
{
    std::int64_t now = 1000;
    s::ProvisioningSession owner;
    explicit Rig(s::ProvisioningSession::Options options = {}) : owner([this] { return now; }, options) {}
    s::Request request()
    {
        auto result = owner.nextRequest();
        check(result.has_value(), "Expected API request");
        return std::move(*result);
    }
    s::Request start()
    {
        owner.start({CreateBody});
        auto result = request();
        check(std::holds_alternative<s::CreateJob>(result.body), "Create must be first");
        check(std::get<s::CreateJob>(result.body).bodyJson == CreateBody, "Body must preserve offer and play token");
        return result;
    }
    void create(std::optional<std::int64_t> delay = 0)
    {
        auto req = start();
        check(owner.complete(req.id, created(delay)), "Create reply consumed");
    }
    bool quiet() { return !owner.nextRequest() && !owner.takeAnswer(); }
};
void schedulingAndHandoff()
{
    Rig r;
    auto create = r.start();
    check(r.quiet(), "No duplicate create while pending");
    check(!r.owner.complete(create.id + 1, created()), "Unknown reply ignored");
    check(r.owner.complete(create.id, created(200)), "Create response");
    check(!r.owner.complete(create.id, created()), "Duplicate create reply ignored");
    r.now += 199;
    check(r.quiet(), "First poll must wait");
    ++r.now;
    for (const auto status : {"ACTIVE_REQUESTED", "ACTIVE_QUEUED", "ACTIVE_CREATING_SESSION",
        "ACTIVE_CONNECTING", "ACTIVE_TIDYING_UP"})
    {
        auto poll = r.request();
        check(std::holds_alternative<s::GetJob>(poll.body), "Poll operation");
        check(poll.id > create.id && std::get<s::GetJob>(poll.body).gameSessionJobId == "fake-job",
            "Poll must preserve job and use new ID");
        check(r.quiet(), "No concurrent poll");
        auto reply = update(status, 700);
        if (reply.status == "ACTIVE_QUEUED")
        {
            reply.positionInLine = 3;
            reply.expectedWaitTimeSeconds = 25;
        }
        check(r.owner.complete(poll.id, reply), "Active reply");
        check(r.owner.status().positionInLine.has_value() == (reply.status == "ACTIVE_QUEUED"),
            "Queue data must clear after leaving queue");
        r.now += 699;
        check(r.quiet(), "Honor server retry delay");
        ++r.now;
    }
    auto poll = r.request();
    r.owner.complete(poll.id, update());
    check(r.owner.status().state == s::State::Ready && r.owner.status().remoteWorkMayRemain,
        "Ready means answer available, owner still responsible for allocated game");
    check(!r.owner.cancel(), "Cancellation must not implicitly terminate an allocated game");
    FakePeer peer;
    auto answer = r.owner.takeAnswer();
    check(answer.has_value(), "Deliver answer");
    peer.apply(*answer);
    check(peer.answers == 1 && r.quiet() && !r.owner.status().remoteWorkMayRemain,
        "One answer handoff, remote ownership transfers to caller");
    check(!r.owner.complete(poll.id, update()), "Success replay ignored");
    rejects<std::logic_error>([&] { r.owner.start({CreateBody}); });

    for (const auto delay : {std::optional<std::int64_t>{}, std::optional<std::int64_t>{0}})
    {
        Rig fallback;
        fallback.create(delay);
        if (!delay) fallback.now += 1500;
        auto req = fallback.request();
        fallback.owner.complete(req.id, update("ACTIVE_CONNECTING", delay));
        fallback.now += 1499;
        check(fallback.quiet(), "Zero/missing subsequent delay uses 1500 ms fallback");
        ++fallback.now;
        check(std::holds_alternative<s::GetJob>(fallback.request().body), "Fallback due");
    }
    Rig late;
    late.create(100);
    late.now += 20'000;
    auto req = late.request();
    late.owner.complete(req.id, update("ACTIVE_CREATING_SESSION", 500));
    check(late.quiet(), "Late ticks cannot trigger catch-up polls");
    late.now += 500;
    late.request();
}
void failures()
{
    Rig rejected;
    auto req = rejected.start();
    auto badCreate = created();
    badCreate.errorCode = 150;
    rejected.owner.complete(req.id, badCreate);
    check(rejected.owner.status().error == s::Error::Api && rejected.owner.status().apiErrorCode == 150 &&
        !rejected.owner.status().remoteWorkMayRemain && rejected.quiet(), "HTTP success may carry API rejection");

    for (const auto operation : {0, 1})
    {
        Rig transport;
        s::Request current = transport.start();
        if (operation)
        {
            transport.owner.complete(current.id, created());
            current = transport.request();
        }
        check(transport.owner.transportFailed(current.id), "Matching transport failure");
        check(transport.owner.status().error == s::Error::Transport &&
            transport.owner.status().remoteWorkMayRemain && transport.quiet(), "Transport ambiguity never retries");
        check(!transport.owner.transportFailed(current.id), "Duplicate failure ignored");
        check(!transport.owner.complete(current.id, created()), "Late result cannot revive owner");
    }
    Rig type;
    req = type.start();
    type.owner.complete(req.id, update());
    check(type.owner.status().error == s::Error::InvalidResponse && type.quiet(), "Wrong response type");

    for (const auto name : {"COMPLETED_FAILED", "UNKNOWN_STATUS"})
    {
        Rig r;
        r.create();
        req = r.request();
        r.owner.complete(req.id, update(name));
        check(r.owner.status().state == s::State::Failed && r.quiet(), "Terminal/unknown state stops polling");
        check(r.owner.status().remoteWorkMayRemain == (std::string(name) == "UNKNOWN_STATUS"),
            "Unknown state cannot establish remote cleanup");
    }
    Rig api;
    api.create();
    req = api.request();
    auto response = update();
    response.errorCode = 999;
    api.owner.complete(req.id, response);
    check(api.owner.status().error == s::Error::Api && api.quiet(), "Poll API error takes precedence over success");

    for (const auto wait : {-1LL, 60'001LL, std::numeric_limits<long long>::max()})
        for (const bool first : {false, true})
        {
            Rig r;
            req = r.start();
            if (first) r.owner.complete(req.id, created(wait));
            else
            {
                r.owner.complete(req.id, created());
                req = r.request();
                r.owner.complete(req.id, update("ACTIVE_REQUESTED", wait));
            }
            check(r.owner.status().error == s::Error::InvalidDelay && r.quiet(), "Invalid/overflowing server delay");
        }
}
void invalidReplies()
{
    for (const auto& id : {std::string{}, std::string(513, 'x'), std::string("bad\njob")})
    {
        Rig r;
        auto req = r.start();
        auto response = created();
        response.gameSessionJobId = id;
        r.owner.complete(req.id, response);
        check(r.owner.status().error == s::Error::InvalidResponse && r.quiet(), "Invalid job identifier");
    }
    for (unsigned field = 0; field < 8; ++field)
    {
        Rig r;
        r.create();
        auto req = r.request();
        auto response = update();
        switch (field)
        {
        case 0: response.gameSessionJobId = "other-job"; break;
        case 1: response.gameSession.reset(); break;
        case 2: response.gameSession->sessionId.clear(); break;
        case 3: response.gameSession->connectionId.clear(); break;
        case 4: response.gameSession->signalingResponse.clear(); break;
        case 5: response.gameSession->signalingResponse.assign(s::ProvisioningSession::MaxAnswerBytes + 1, 'x'); break;
        case 6: response.gameSession->sessionId.assign(513, 'x'); break;
        case 7: response.status.assign(65, 'x'); break;
        }
        r.owner.complete(req.id, response);
        check(r.owner.status().state == s::State::Failed && r.quiet(), "Malformed or cross-job answer rejected");
    }
    for (const bool position : {false, true})
    {
        Rig r;
        r.create();
        auto req = r.request();
        auto response = update("ACTIVE_QUEUED");
        (position ? response.positionInLine : response.expectedWaitTimeSeconds) = -1;
        r.owner.complete(req.id, response);
        check(r.owner.status().error == s::Error::InvalidResponse, "Negative queue metadata");
    }
    Rig unexpected;
    unexpected.create();
    auto req = unexpected.request();
    auto response = update();
    response.status = "ACTIVE_CONNECTING";
    unexpected.owner.complete(req.id, response);
    check(unexpected.owner.status().error == s::Error::InvalidResponse, "Premature answer not handed off");
    Rig contradictory;
    contradictory.create();
    req = contradictory.request();
    response = update();
    response.status = "COMPLETED_FAILED";
    contradictory.owner.complete(req.id, response);
    check(contradictory.owner.status().error == s::Error::InvalidResponse &&
        contradictory.owner.status().remoteWorkMayRemain, "Contradictory failure cannot prove remote cleanup");
}
void timeouts()
{
    for (const bool poll : {false, true})
    {
        Rig r;
        auto req = r.start();
        if (poll)
        {
            r.owner.complete(req.id, created());
            req = r.request();
        }
        r.now += 10'000;
        check(!r.owner.complete(req.id, poll ? s::Response{update()} : s::Response{created()}),
            "Deadline checked before applying response, even without an intervening tick");
        check(r.owner.status().error == s::Error::RequestTimeout &&
            r.owner.status().remoteWorkMayRemain && r.quiet(), "Timeout never repeats create/poll");
    }
    s::ProvisioningSession::Options options;
    options.overallTimeoutMs = 2000;
    Rig overall(options);
    overall.create(1500);
    overall.now += 1500;
    auto req = overall.request();
    overall.now += 500;
    check(!overall.owner.complete(req.id, update()), "Overall timeout wins before request timeout");
    check(overall.owner.status().error == s::Error::OverallTimeout && overall.quiet(), "Bounded attempt");
    Rig clock;
    clock.create();
    --clock.now;
    check(clock.quiet() && clock.owner.status().error == s::Error::Clock, "Backward clock stops owner");
    Rig overflow;
    overflow.now = std::numeric_limits<std::int64_t>::max() - 500;
    req = overflow.start();
    overflow.owner.complete(req.id, created(400));
    overflow.now += 400;
    req = overflow.request();
    overflow.now += 100;
    check(!overflow.owner.complete(req.id, update()) && overflow.quiet(), "Deadline addition saturates safely");
}
void cancellation()
{
    Rig idle;
    check(idle.owner.cancel() && idle.quiet() && !idle.owner.status().remoteWorkMayRemain,
        "Idle cancellation needs no API call");
    Rig unsent;
    unsent.owner.start({CreateBody});
    check(unsent.owner.cancel() && unsent.quiet() && !unsent.owner.status().cancellationConfirmed,
        "Unsent create discarded locally, no invented server confirmation");

    for (const bool inCreate : {false, true})
    {
        Rig r;
        auto req = r.start();
        if (!inCreate) r.owner.complete(req.id, created(50'000));
        check(r.owner.cancel(), "Cancellation requested");
        if (inCreate)
        {
            check(r.quiet(), "Must learn job ID before cancellation");
            r.owner.complete(req.id, created(-99)); // poll delay is irrelevant when cancelling
        }
        req = r.request();
        check(std::get<s::CancelJob>(req.body).gameSessionJobId == "fake-job", "Cancel known job once");
        check(r.quiet(), "No duplicate cancellation");
        r.owner.complete(req.id, cancelledBeforeAllocation());
        check(r.owner.status().state == s::State::Cancelled && r.owner.status().cancellationConfirmed &&
            !r.owner.status().remoteWorkMayRemain && r.quiet(), "Confirmed pre-allocation cancellation");
        check(!r.owner.cancel(), "Terminal cancellation is inert");
    }
    for (unsigned race = 0; race < 5; ++race)
    {
        Rig r;
        r.create();
        const auto poll = r.request();
        r.owner.cancel();
        if (race == 0) r.owner.complete(poll.id, update("ACTIVE_CONNECTING"));
        if (race == 1 || race == 4) r.owner.complete(poll.id, update());
        if (race == 2) r.owner.transportFailed(poll.id);
        if (race == 3)
        {
            auto invalid = update();
            invalid.gameSessionJobId = "other-job";
            r.owner.complete(poll.id, invalid);
        }
        const auto req = r.request();
        check(std::holds_alternative<s::CancelJob>(req.body) && !r.owner.takeAnswer(), "Race cannot deliver answer");
        r.owner.complete(req.id, race == 4 ? s::CancelledJob{} : cancelledBeforeAllocation());
        check(r.owner.status().allocatedDuringCancellation.has_value() == (race == 1 || race == 4),
            "Retain IDs for cleanup when allocation races cancellation");
        check(r.owner.status().remoteWorkMayRemain == (race == 1 || race == 4), "Do not mistake job ACK for game termination");
        check(r.quiet() && !r.owner.complete(poll.id, update()), "Late poll cannot revive cancelled attempt");
    }
    Rig ackOnly;
    ackOnly.create();
    ackOnly.owner.cancel();
    auto req = ackOnly.request();
    ackOnly.owner.complete(req.id, s::CancelledJob{});
    check(ackOnly.owner.status().remoteWorkMayRemain, "ACK without allocation outcome remains uncertain");

    s::ProvisioningSession::Options options;
    options.requestTimeoutMs = 100;
    options.cancellationTimeoutMs = 500;
    Rig slowPoll(options);
    slowPoll.create();
    auto poll = slowPoll.request();
    slowPoll.owner.cancel();
    slowPoll.now += 100;
    req = slowPoll.request();
    check(std::holds_alternative<s::CancelJob>(req.body), "Cancel known job after poll expires");
    check(!slowPoll.owner.complete(poll.id, update()), "Expired poll ignored during cancel request");
    slowPoll.owner.complete(req.id, cancelledBeforeAllocation());
    check(slowPoll.owner.status().state == s::State::Cancelled, "Cancellation after lost poll");

    Rig defaultLostPoll;
    defaultLostPoll.create();
    defaultLostPoll.request();
    defaultLostPoll.owner.cancel();
    defaultLostPoll.now += 10'000;
    req = defaultLostPoll.request();
    check(std::holds_alternative<s::CancelJob>(req.body), "Default cancellation budget covers poll wait and cancel");
    defaultLostPoll.owner.complete(req.id, cancelledBeforeAllocation());

    Rig lostCreate(options);
    req = lostCreate.start();
    lostCreate.owner.cancel();
    lostCreate.now += 100;
    check(lostCreate.quiet() && lostCreate.owner.status().error == s::Error::RequestTimeout &&
        lostCreate.owner.status().remoteWorkMayRemain, "Lost create cannot cancel an unknown job or retry");
    check(!lostCreate.owner.complete(req.id, created()), "Late create does not revive terminal owner");

    s::ProvisioningSession::Options boundedOptions;
    boundedOptions.requestTimeoutMs = 30'000;
    Rig bounded(boundedOptions);
    bounded.create();
    bounded.owner.cancel();
    req = bounded.request();
    bounded.now += 19'999;
    check(bounded.owner.cancel(), "Repeated cancellation allowed");
    ++bounded.now;
    check(bounded.quiet() && bounded.owner.status().error == s::Error::CancellationTimeout &&
        !bounded.owner.status().cancellationConfirmed && bounded.owner.status().remoteWorkMayRemain,
        "Repeated cancel cannot extend deadline or invent success");

    Rig lostAck;
    lostAck.create();
    lostAck.owner.cancel();
    req = lostAck.request();
    lostAck.now += 10'000;
    check(lostAck.quiet() && lostAck.owner.status().error == s::Error::RequestTimeout &&
        lostAck.owner.status().remoteWorkMayRemain && !lostAck.owner.status().cancellationConfirmed,
        "Cancellation request timeout remains unconfirmed");
    check(!lostAck.owner.complete(req.id, cancelledBeforeAllocation()), "Late cancel ACK cannot revive owner");

    for (const bool api : {false, true})
    {
        Rig r;
        r.create();
        r.owner.cancel();
        req = r.request();
        if (api)
        {
            s::CancelledJob denied;
            denied.errorCode = 123;
            r.owner.complete(req.id, denied);
        }
        else r.owner.transportFailed(req.id);
        check(r.owner.status().state == s::State::Failed && !r.owner.status().cancellationConfirmed &&
            r.owner.status().remoteWorkMayRemain && r.quiet(), "Failed cancellation is not a successful stop");
    }
}
void fakeApiCancellationRace()
{
    Rig r;
    FakeApi api;
    FakePeer peer;
    api.polls = {update()};
    auto req = r.start();
    r.owner.complete(req.id, api.respond(req));
    r.now += 200;
    req = r.request();
    r.owner.cancel();
    r.owner.complete(req.id, api.respond(req)); // fake server allocated during cancellation
    req = r.request();
    r.owner.complete(req.id, api.respond(req));
    check(api.sent.size() == 3 && api.allocated && r.owner.status().cancellationConfirmed &&
        r.owner.status().remoteWorkMayRemain && r.owner.status().allocatedDuringCancellation &&
        !r.owner.takeAnswer() && peer.answers == 0, "Fake API allocation race never reaches the peer");
}
void construction()
{
    rejects([] { s::ProvisioningSession owner({}); });
    s::ProvisioningSession::Options options;
    options.requestTimeoutMs = 0;
    rejects([&] { s::ProvisioningSession owner([] { return 0; }, options); });
    options = {};
    options.overallTimeoutMs = -1;
    rejects([&] { s::ProvisioningSession owner([] { return 0; }, options); });
    options = {};
    options.cancellationTimeoutMs = 0;
    rejects([&] { s::ProvisioningSession owner([] { return 0; }, options); });
    options = {};
    options.maxPollDelayMs = 1499;
    rejects([&] { s::ProvisioningSession owner([] { return 0; }, options); });
    Rig r;
    rejects([&] { r.owner.start({""}); });
    rejects([&] { r.owner.start({std::string(s::ProvisioningSession::MaxCreateBytes + 1, 'x')}); });
    r.now = -1;
    rejects([&] { r.owner.start({CreateBody}); });
    check(r.owner.status().state == s::State::Idle && r.quiet(), "Invalid start cannot issue create");
}
}
int main()
{
    try
    {
        construction();
        schedulingAndHandoff();
        failures();
        invalidReplies();
        timeouts();
        cancellation();
        fakeApiCancellationRace();
        std::cout << "PASS: " << checks << " provisioning checks: scheduling, handoff, errors, deadlines, cancellation races and uncertain outcomes.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
