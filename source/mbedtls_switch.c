#include <switch.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform_time.h>
#include <stdio.h>

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(
        armTicksToNs(svcGetSystemTick()) / 1000000);
}

int mbedtls_hardware_poll(void* data, unsigned char* output,
                          size_t len, size_t* olen)
{
    (void)data;
    *olen = 0;

    Result result = csrngInitialize();
    if (R_FAILED(result))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    result = csrngGetRandomBytes(output, len);
    csrngExit();

    if (R_FAILED(result))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    *olen = len;
    return 0;
}

void testMbedTlsPlatform(void)
{
    unsigned char bytes[32];
    size_t written = 0;

    int result = mbedtls_hardware_poll(
        NULL, bytes, sizeof(bytes), &written);

    printf("Secure RNG: result=%d, bytes=%zu\n", result, written);

    mbedtls_ms_time_t start = mbedtls_ms_time();
    svcSleepThread(100000000);
    mbedtls_ms_time_t elapsed = mbedtls_ms_time() - start;

    printf("Clock advanced: %lld ms\n", (long long)elapsed);
}