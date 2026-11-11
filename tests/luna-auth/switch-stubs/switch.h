#pragma once
#include <cstddef>
#include <cstdint>
using u32 = std::uint32_t;
using Result = std::uint32_t;
#define R_SUCCEEDED(result) ((result) == 0)
#define R_FAILED(result) ((result) != 0)
Result splCryptoInitialize();
void splCryptoExit();
Result splCryptoLockAesEngine(u32*);
Result splCryptoUnlockAesEngine(u32);
Result splCryptoGenerateAesKek(const void*, u32, u32, void*);
Result splCryptoLoadAesKey(const void*, const void*, u32);
Result splCryptoComputeCmac(const void*, std::size_t, u32, void*);
