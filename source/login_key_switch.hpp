#pragma once
#include "remembered_login.hpp"
namespace luna::auth
{
// The source constants identify LunaNX; the secret comes from device-unique
// SPL derivation. Neither a console serial nor a key file is used as a key.
bool switchLoginKey(RememberedLogin::Key& key);
}
