#include <netdb.h>
#include <string.h>

unsigned lunanx_test_resolver_calls;

// Separate object so GNU ld can wrap the probe's undefined getnameinfo calls.
// Simulates a PTR result without DNS or an Amazon request.
int getnameinfo(const struct sockaddr *address, socklen_t length,
                char *host, socklen_t host_length,
                char *service, socklen_t service_length, int flags)
{
    (void)address; (void)length; (void)flags;
    ++lunanx_test_resolver_calls;
    const char *ptr = "ec2-example.test.invalid";
    if (host)
    {
        if (host_length <= strlen(ptr)) return EAI_OVERFLOW;
        strcpy(host, ptr);
    }
    if (service)
    {
        if (service_length < 6) return EAI_OVERFLOW;
        strcpy(service, "33438");
    }
    return 0;
}
