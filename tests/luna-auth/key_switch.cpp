#include "login_key_switch.hpp"
#include <switch.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
int failStage = 0, stage = 0, opened = 0, locked = 0;
void require(bool condition)
{ if (!condition) throw std::runtime_error("Switch key-provider lifecycle or derivation parameters"); }
Result step(int expected)
{ require(++stage == expected); return failStage == stage ? 0x1234 : 0; }
}
Result splCryptoInitialize()
{ const auto result = step(1); if (!result) ++opened; return result; }
void splCryptoExit() { require(opened == 1 && locked == 0); --opened; }
Result splCryptoLockAesEngine(u32* slot)
{ const auto result = step(2); if (!result) { *slot = 2; ++locked; } return result; }
Result splCryptoUnlockAesEngine(u32 slot)
{ require(slot == 2 && locked == 1); --locked; return 0; }
Result splCryptoGenerateAesKek(const void* source, u32 generation, u32 option, void* sealed)
{
    require(source && generation == 1 && option == 1 && locked == 1);
    const auto result = step(3);
    if (!result) std::memset(sealed, 0x66, 16);
    return result;
}
Result splCryptoLoadAesKey(const void* sealed, const void* source, u32 slot)
{
    require(sealed && source && slot == 2 && locked == 1);
    return step(4);
}
Result splCryptoComputeCmac(const void* context, std::size_t count, u32 slot, void* key)
{
    require(context && count > 16 && count < 1024 && slot == 2 && locked == 1);
    std::memset(key, 0x77, 16); // Also test wiping a partially written failed result.
    return step(5);
}
int main()
{
    for (failStage = 0; failStage <= 5; ++failStage)
    {
        stage = 0;
        luna::auth::RememberedLogin::Key key; key.fill(0xaa);
        const bool success = luna::auth::switchLoginKey(key);
        require(success == (failStage == 0) && opened == 0 && locked == 0);
        require(std::all_of(key.begin(), key.end(), [](unsigned char value) { return value == (failStage ? 0 : 0x77); }));
    }
    std::cout << "PASS: Switch key-provider parameters, service/keyslot cleanup and failure wiping (SDK stubs).\n";
}
