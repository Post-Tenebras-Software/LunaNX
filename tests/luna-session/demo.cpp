#include "fake_api.hpp"

#include <iostream>

int main()
{
    using namespace session_test;
    std::int64_t now = 0;
    s::ProvisioningSession owner([&] { return now; });
    FakeApi api;
    FakePeer peer; // represents the already-created peer that supplied fake-offer
    auto queued = update("ACTIVE_QUEUED", 400);
    queued.positionInLine = 2;
    queued.expectedWaitTimeSeconds = 1;
    api.polls = {queued, update("ACTIVE_CREATING_SESSION", 300), update()};
    owner.start({CreateBody});
    for (; now <= 2000; now += 100)
    {
        if (auto request = owner.nextRequest())
        {
            const char* operation = std::holds_alternative<s::CreateJob>(request->body)
                ? "create" : "poll";
            owner.complete(request->id, api.respond(*request));
            std::cout << now << " ms: fake " << operation;
            if (!owner.status().jobStatus.empty()) std::cout << " -> " << owner.status().jobStatus;
            std::cout << '\n';
        }
        if (auto answer = owner.takeAnswer())
        {
            peer.apply(*answer);
            std::cout << "Answer handed to original fake peer once. No network connection made.\n";
            return peer.answers == 1 ? 0 : 1;
        }
    }
    std::cerr << "Fake provisioning did not finish\n";
    return 1;
}
