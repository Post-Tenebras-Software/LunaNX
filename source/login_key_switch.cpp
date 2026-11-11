#include "login_key_switch.hpp"
#include <switch.h>
#include <cstdio>

namespace luna::auth
{
bool switchLoginKey(RememberedLogin::Key& key)
{
    key.fill(0);
    auto result = splCryptoInitialize();
    if (R_FAILED(result))
    {
        printf("Login key: stage=initialize result=0x%08X\n", static_cast<unsigned>(result));
        return false;
    }
    // Public domain-separation inputs, not Nintendo keys or embedded secrets.
    alignas(16) constexpr unsigned char kekSource[16] = {
        'L','u','n','a','N','X',' ','l','o','g','i','n',' ','v','1','!'};
    alignas(16) constexpr unsigned char keySource[16] = {
        'S','D',' ','c','r','e','d','e','n','t','i','a','l','s','1','!'};
    constexpr char context[] = "LunaNX remembered sign-in AES-GCM key v1";
    alignas(16) unsigned char sealed[16]{};
    u32 slot = 0;
    result = splCryptoLockAesEngine(&slot);
    const bool locked = R_SUCCEEDED(result);
    const char* stage = "lock";
    // Fixed generation 1 (firmware 1.0 key generation); option bit 0 selects
    // device-unique derivation. Boot-random sealed handles are never persisted.
    if (R_SUCCEEDED(result)) { stage = "derive"; result = splCryptoGenerateAesKek(kekSource, 1, 1, sealed); }
    if (R_SUCCEEDED(result)) { stage = "load"; result = splCryptoLoadAesKey(sealed, keySource, slot); }
    if (R_SUCCEEDED(result)) { stage = "cmac"; result = splCryptoComputeCmac(context, sizeof(context) - 1, slot, key.data()); }
    const bool success = R_SUCCEEDED(result);
    if (locked) splCryptoUnlockAesEngine(slot);
    auto* bytes = static_cast<volatile unsigned char*>(sealed);
    for (unsigned i = 0; i < sizeof(sealed); ++i) bytes[i] = 0;
    splCryptoExit();
    if (!success)
    {
        key.fill(0);
        printf("Login key: stage=%s result=0x%08X\n", stage, static_cast<unsigned>(result));
    }
    return success;
}
}
