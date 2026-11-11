#pragma once
#include "native_auth.hpp"
#include <array>
#include <functional>

namespace luna::auth
{
enum class LoginMemoryState { Empty, Saved, Restoring, SessionOnly, Unreadable, Unavailable, Rejected, RemovalFailed };
const char* loginMemoryMessage(LoginMemoryState state);

// The only bridge from the private auth core to persistent storage. No access
// token, profile token, password, code pair or launch payload is serialized.
class RememberedLogin
{
public:
    using Key = std::array<unsigned char, 16>;
    using KeyProvider = std::function<bool(Key&)>;
    using Random = std::function<bool(unsigned char*, std::size_t)>;
    RememberedLogin(std::string path, KeyProvider key, Random random);
    bool resume(NativeAuthProbe& auth, std::int64_t nowMs);
    void observe(const NativeAuthProbe& auth);
    bool forget();
    bool saved() const { return saved_; }
    LoginMemoryState state() const { return state_; }
private:
    bool save(const NativeAuthProbe& auth);
    std::string path_;
    KeyProvider key_;
    Random random_;
    LoginMemoryState state_ = LoginMemoryState::Empty;
    bool saved_ = false;
    std::uint64_t observedRevision_ = 0;
};
}
