#include "remembered_login.hpp"
#include <mbedtls/gcm.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <utility>

namespace luna::auth
{
namespace
{
constexpr std::size_t Header = 24, Tag = 16, MaxPlain = 32 + 32768;
constexpr unsigned char Magic[] = {'L', 'U', 'N', 'A', 'L', 'G', 'N', '1'};
void wipe(void* value, std::size_t size)
{
    auto* bytes = static_cast<volatile unsigned char*>(value);
    while (size--) *bytes++ = 0;
}
struct SecretBytes : std::vector<unsigned char>
{
    using std::vector<unsigned char>::vector;
    ~SecretBytes() { wipe(data(), size()); }
};
struct SecretKey
{
    RememberedLogin::Key bytes{};
    ~SecretKey() { wipe(bytes.data(), bytes.size()); }
};
struct Gcm
{
    mbedtls_gcm_context context;
    Gcm() { mbedtls_gcm_init(&context); }
    ~Gcm() { mbedtls_gcm_free(&context); }
};
bool valid(const std::string& serial, const std::string& token)
{
    return serial.size() == 32 && serial.find_first_not_of("0123456789abcdef") == std::string::npos &&
        !token.empty() && token.size() <= 32768 && std::all_of(token.begin(), token.end(),
            [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
}
bool rejected(const Status& s)
{
    // A timeout, temporary server error or malformed response must not erase
    // a working saved login. Only an explicit refresh-authorization rejection.
    return s.step == Step::Failed && s.failedAt == Step::Refresh && s.error == Error::Http &&
        (s.httpStatus == 401 || s.httpStatus == 403 || s.authError == AuthError::InvalidGrant ||
         s.authError == AuthError::InvalidToken || s.authError == AuthError::Unauthorized ||
         s.authError == AuthError::AccessDenied);
}
}
const char* loginMemoryMessage(LoginMemoryState state)
{
    switch (state)
    {
    case LoginMemoryState::Saved: return "Sign-in saved for this Switch. Your profile is selected each launch.";
    case LoginMemoryState::Restoring: return "Using your saved sign-in. Connecting securely to Amazon.";
    case LoginMemoryState::SessionOnly: return "Signed in for this session. The login could not be saved; check SD access and console encryption.";
    case LoginMemoryState::Unreadable: return "Saved sign-in could not be read on this Switch. Link again to replace it, or forget it in Settings.";
    case LoginMemoryState::Unavailable: return "Saved sign-in is unavailable. Check the SD card and console encryption, then retry.";
    case LoginMemoryState::Rejected: return "Amazon no longer accepts the saved sign-in. Link your account again.";
    case LoginMemoryState::RemovalFailed: return "The saved login could not be removed. Check SD access and try again.";
    default: return "Link once to remember your sign-in on this Switch.";
    }
}
RememberedLogin::RememberedLogin(std::string path, KeyProvider key, Random random) :
    path_(std::move(path)), key_(std::move(key)), random_(std::move(random)) {}
bool RememberedLogin::resume(NativeAuthProbe& auth, std::int64_t nowMs)
{
    if (auth.status_.step != Step::Idle || nowMs < 0) return false;
    observedRevision_ = 0;
    try
    {
        if (!std::filesystem::exists(path_)) { saved_ = false; state_ = LoginMemoryState::Empty; return false; }
        saved_ = true;
        state_ = LoginMemoryState::Unreadable;
        if (!std::filesystem::is_regular_file(path_) || std::filesystem::is_symlink(path_)) return false;
        const auto length = std::filesystem::file_size(path_);
        if (length < Header + Tag + 33 || length > Header + Tag + MaxPlain) return false;
        std::vector<unsigned char> file(static_cast<std::size_t>(length));
        std::ifstream input(path_, std::ios::binary);
        input.read(reinterpret_cast<char*>(file.data()), file.size());
        if (!input) { state_ = LoginMemoryState::Unavailable; return false; }
        if (!std::equal(std::begin(Magic), std::end(Magic), file.begin())) return false;
        std::uint32_t count = 0;
        for (unsigned i = 0; i < 4; ++i) count |= std::uint32_t(file[20 + i]) << (i * 8);
        if (count < 33 || count > MaxPlain || file.size() != Header + count + Tag) return false;
        SecretKey key;
        if (!key_(key.bytes)) { state_ = LoginMemoryState::Unavailable; return false; }
        Gcm gcm;
        SecretBytes plain(count);
        if (mbedtls_gcm_setkey(&gcm.context, MBEDTLS_CIPHER_ID_AES, key.bytes.data(), 128) ||
            mbedtls_gcm_auth_decrypt(&gcm.context, count, file.data() + 8, 12, file.data(), Header,
                file.data() + Header + count, Tag, file.data() + Header, plain.data())) return false;
        // Validate before copying decrypted secrets into the authentication core.
        if (!std::all_of(plain.begin(), plain.begin() + 32, [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            !std::all_of(plain.begin() + 32, plain.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; })) return false;
        auth.restoreLogin(std::string(plain.begin(), plain.begin() + 32),
            std::string(plain.begin() + 32, plain.end()), nowMs);
        observedRevision_ = auth.credentialRevision_;
        state_ = LoginMemoryState::Restoring;
        return true;
    }
    catch (...) { state_ = LoginMemoryState::Unavailable; return false; }
}
bool RememberedLogin::save(const NativeAuthProbe& auth)
{
    if (!valid(auth.serial_, auth.refreshToken_)) return false;
    try
    {
        SecretKey key;
        if (!key_(key.bytes)) return false;
        const auto count = auth.serial_.size() + auth.refreshToken_.size();
        std::vector<unsigned char> file(Header + count + Tag, 0);
        std::copy(std::begin(Magic), std::end(Magic), file.begin());
        if (!random_(file.data() + 8, 12)) return false;
        for (unsigned i = 0; i < 4; ++i) file[20 + i] = static_cast<unsigned char>(count >> (i * 8));
        SecretBytes plain(count);
        std::copy(auth.serial_.begin(), auth.serial_.end(), plain.begin());
        std::copy(auth.refreshToken_.begin(), auth.refreshToken_.end(), plain.begin() + 32);
        Gcm gcm;
        if (mbedtls_gcm_setkey(&gcm.context, MBEDTLS_CIPHER_ID_AES, key.bytes.data(), 128) ||
            mbedtls_gcm_crypt_and_tag(&gcm.context, MBEDTLS_GCM_ENCRYPT, count, file.data() + 8, 12,
                file.data(), Header, plain.data(), file.data() + Header, Tag, file.data() + Header + count)) return false;
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
        const auto temporary = path_ + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(file.data()), file.size());
        output.close();
        std::error_code error;
        if (output) std::filesystem::rename(temporary, path_, error);
        if (!output || error) { std::filesystem::remove(temporary, error); return false; }
        saved_ = true;
        return true;
    }
    catch (...) { return false; }
}
void RememberedLogin::observe(const NativeAuthProbe& auth)
{
    if (saved_ && rejected(auth.status_))
    {
        if (forget()) state_ = LoginMemoryState::Rejected;
        return;
    }
    if (auth.credentialRevision_ == 0) observedRevision_ = 0;
    if (!auth.refreshToken_.empty() && auth.credentialRevision_ != observedRevision_)
    {
        observedRevision_ = auth.credentialRevision_;
        state_ = save(auth) ? LoginMemoryState::Saved : LoginMemoryState::SessionOnly;
    }
    else if (state_ == LoginMemoryState::Restoring && (auth.status_.refreshed ||
        auth.status_.step == Step::Failed || auth.status_.step == Step::Cancelled))
        state_ = LoginMemoryState::Saved;
}
bool RememberedLogin::forget()
{
    try
    {
        std::filesystem::remove(path_ + ".tmp");
        std::filesystem::remove(path_);
        saved_ = false; observedRevision_ = 0; state_ = LoginMemoryState::Empty;
        return true;
    }
    catch (...) { state_ = LoginMemoryState::RemovalFailed; return false; }
}
}
