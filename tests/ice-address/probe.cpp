#include <rtc/candidate.hpp>
extern "C" {
#include "ice.h"
}

#include <cstring>
#include <iostream>
#include <stdexcept>

extern "C" unsigned lunanx_test_resolver_calls;
namespace
{
unsigned& resolverCalls = lunanx_test_resolver_calls;
constexpr const char* Ip = "198.51.100.9";
constexpr const char* Ptr = "ec2-example.test.invalid";
void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char**)
{
    try
    {
        rtc::Candidate candidate("candidate:1 1 UDP 2114977791 198.51.100.9 33438 typ host generation 0", "video");
        check(candidate.resolve(rtc::Candidate::ResolveMode::Simple), "Numeric candidate resolution");
        ice_candidate_t parsed{};
        check(ice_parse_candidate_sdp(std::string(candidate).c_str(), &parsed) == 0, "Candidate parser");
        if (argc > 1)
        {
            check(candidate.address() == Ptr && parsed.resolved.len == 0 && resolverCalls == 1,
                "Negative control did not reproduce PTR → empty destination");
            std::cout << "PASS: reproduced original Candidate → PTR hostname → zero-length libjuice destination.\n";
            return 0;
        }
        check(candidate.address() == Ip && candidate.port() == 33438 && resolverCalls == 0,
            "Numeric formatting must not use the resolver");
        check(candidate.mid() == "video" && candidate.candidate().find("generation 0") != std::string::npos,
            "Candidate metadata was changed");
        check(parsed.resolved.len == sizeof(sockaddr_in) && parsed.resolved.socktype == SOCK_DGRAM,
            "ICE destination is empty or has the wrong transport");
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&parsed.resolved.addr);
        in_addr expected{};
        inet_pton(AF_INET, Ip, &expected);
        check(ipv4->sin_addr.s_addr == expected.s_addr && ntohs(ipv4->sin_port) == 33438,
            "Wrong ICE destination");

        ice_candidate_t reflexive{};
        check(ice_create_local_candidate(ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE, 1, 0,
            &parsed.resolved, &reflexive, ICE_CANDIDATE_TRANSPORT_UDP) == 0,
            "Reflexive address formatting");
        check(std::strcmp(reflexive.hostname, Ip) == 0 && resolverCalls == 0,
            "A public/reflexive address became a hostname");
        char rendered[128];
        check(addr_record_to_string(&parsed.resolved, rendered, sizeof(rendered)) > 0 &&
            std::strcmp(rendered, "198.51.100.9:33438") == 0, "Address logging must stay numeric");

        char host[128], service[16];
        sockaddr_in6 ipv6{};
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(65535);
        inet_pton(AF_INET6, "2001:db8::1", &ipv6.sin6_addr);
        check(getnameinfo(reinterpret_cast<const sockaddr*>(&ipv6), sizeof(ipv6), host, sizeof(host),
            service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
            std::strcmp(host, "2001:db8::1") == 0 && std::strcmp(service, "65535") == 0,
            "IPv6 numeric address/port");
        ipv6.sin6_scope_id = 7;
        check(getnameinfo(reinterpret_cast<const sockaddr*>(&ipv6), sizeof(ipv6), host, sizeof(host),
            nullptr, 0, NI_NUMERICHOST) == 0 && std::strcmp(host, "2001:db8::1%7") == 0,
            "IPv6 scope ID");
        char small = 'X';
        check(getnameinfo(reinterpret_cast<const sockaddr*>(ipv4), sizeof(*ipv4), &small, 1,
            nullptr, 0, NI_NUMERICHOST) == EAI_OVERFLOW && small == 'X', "Bounded numeric output");
        check(getnameinfo(reinterpret_cast<const sockaddr*>(ipv4), 1, host, sizeof(host),
            nullptr, 0, NI_NUMERICHOST) == EAI_FAMILY, "Truncated socket address");
        check(getnameinfo(reinterpret_cast<const sockaddr*>(ipv4), sizeof(*ipv4), nullptr, 0,
            service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
            std::strcmp(service, "33438") == 0, "Service-only numeric output");
        check(resolverCalls == 0, "Numeric formatting performed a resolver call");
        check(getnameinfo(reinterpret_cast<const sockaddr*>(ipv4), sizeof(*ipv4), host, sizeof(host),
            nullptr, 0, 0) == 0 && std::strcmp(host, Ptr) == 0 && resolverCalls == 1,
            "Named lookups must retain the original resolver");
        std::cout << "PASS: real Candidate/libjuice chain stays numeric; destination IP/port, reflexive candidates, IPv6 scope, bounds and named-lookup fallback.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
