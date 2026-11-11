#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int __real_getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t,
                       char *, socklen_t, int);

// Horizon's resolver can return a PTR hostname even for NI_NUMERICHOST.
// ICE subsequently feeds this text to a numeric-only parser. Format numeric
// requests locally; leave requests for actual names/services with libnx.
int __wrap_getnameinfo(const struct sockaddr *address, socklen_t length,
                       char *host, socklen_t host_length,
                       char *service, socklen_t service_length, int flags)
{
    if (!(flags & NI_NUMERICHOST) || (service && !(flags & NI_NUMERICSERV)) ||
        (!host && !service))
        return __real_getnameinfo(address, length, host, host_length,
                                  service, service_length, flags);

    if (!address || length < sizeof(struct sockaddr)) return EAI_FAMILY;
    char node[INET6_ADDRSTRLEN + 12]; // IPv6 address plus a numeric scope ID.
    char port_text[6];
    const void *bytes;
    unsigned port;
    unsigned scope = 0;
    if (address->sa_family == AF_INET)
    {
        if (length < sizeof(struct sockaddr_in)) return EAI_FAMILY;
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
        bytes = &ipv4->sin_addr;
        port = ntohs(ipv4->sin_port);
    }
    else if (address->sa_family == AF_INET6)
    {
        if (length < sizeof(struct sockaddr_in6)) return EAI_FAMILY;
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)address;
        bytes = &ipv6->sin6_addr;
        port = ntohs(ipv6->sin6_port);
        scope = ipv6->sin6_scope_id;
    }
    else return EAI_FAMILY;

    if (!inet_ntop(address->sa_family, bytes, node, sizeof(node))) return EAI_SYSTEM;
    if (scope)
    {
        const size_t used = strlen(node);
        snprintf(node + used, sizeof(node) - used, "%%%u", scope);
    }
    snprintf(port_text, sizeof(port_text), "%u", port);
    if ((host && strlen(node) >= host_length) ||
        (service && strlen(port_text) >= service_length)) return EAI_OVERFLOW;
    if (host) memcpy(host, node, strlen(node) + 1);
    if (service) memcpy(service, port_text, strlen(port_text) + 1);
    return 0;
}
