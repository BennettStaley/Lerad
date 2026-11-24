// SPDX-License-Identifier: MIT
// Build check: our compiler settings, the generated version header and the
// vendored ReShade and mpv headers all compile together.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

#include <reshade.hpp>
#include <mpv/client.h>

#include "app_version.h"

static_assert(APP_VERSION_MAJOR >= 0, "version header missing");
static_assert(RESHADE_API_VERSION >= 1, "ReShade API header missing");
static_assert(MPV_CLIENT_API_VERSION >= MPV_MAKE_VERSION(2, 0), "mpv client header too old");

int main() {
    std::printf("%s %s (%s), ReShade API %d, mpv client API %lu.%lu\n",
                APP_NAME, APP_VERSION, APP_GIT_DESCRIBE, RESHADE_API_VERSION,
                MPV_CLIENT_API_VERSION >> 16, MPV_CLIENT_API_VERSION & 0xffff);
    // The names made from the product's name: the same letters, small for
    // files and pipes, capitals and an underscore for environment variables.
    const std::string name = APP_NAME, id = APP_ID, prefix = APP_ENV_PREFIX;
    bool ok = !name.empty() && id.size() == name.size() && prefix.size() == name.size() + 1 && prefix.back() == '_';
    for (size_t i = 0; ok && i < name.size(); ++i)
        ok = id[i] == std::tolower(static_cast<unsigned char>(name[i])) &&
             prefix[i] == std::toupper(static_cast<unsigned char>(name[i]));
    ok = ok && std::wstring(APP_NAME_W) == std::wstring(name.begin(), name.end()) &&
         std::strcmp(APP_EXE, (name + ".exe").c_str()) == 0 && std::strcmp(APP_INI, (id + ".ini").c_str()) == 0;
    return ok ? 0 : 1;
}
