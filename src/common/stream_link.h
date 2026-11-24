// SPDX-License-Identifier: MIT
// The stream window: a second, ordinary SDR window with a live copy of the
// picture, for sharing in Discord or OBS while the main window stays HDR.
//
// Two processes take part. The add-on in mpv.exe (src/overlay/stream_out.h)
// draws each game frame, scaled and tone mapped to SDR, into a texture both
// can open. The app (src/app/stream_window.h) owns the window and shows that
// texture. They meet through three named objects of the session:
//
//   kOn       an event, set while the window is wanted. The app makes it
//             before mpv starts; either side sets or resets it (the menu's
//             switch, the window's close box), and both follow it.
//   kFrame    an event the add-on sets after each frame it has drawn.
//   kTexture  the texture (B8G8R8A8, kWidth x kHeight, with a keyed mutex:
//             key 0 for both sides), made by the add-on when first wanted.
#pragma once
#include "app_version.h"

namespace app::stream {

inline constexpr wchar_t kOn[] = L"Local\\" APP_ID_W L"-stream-on";
inline constexpr wchar_t kFrame[] = L"Local\\" APP_ID_W L"-stream-frame";
inline constexpr wchar_t kTexture[] = APP_ID_W L"-stream-texture";

// The copy's size: 1440 lines, the most Discord sends.
inline constexpr unsigned kWidth = 2560, kHeight = 1440;

// The largest w x h of the copy's shape that fits into max_w x max_h, not
// larger than the copy itself.
inline void fit(int max_w, int max_h, int &w, int &h) {
    w = int(kWidth), h = int(kHeight);
    if (max_w < w) {
        w = max_w;
        h = int((long long)(w) * kHeight / kWidth);
    }
    if (max_h < h) {
        h = max_h;
        w = int((long long)(h) * kWidth / kHeight);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

// Which of `count` monitors the window goes to: never the one the picture is
// on (`play`) while there is another; the primary one if it may, else the
// first other. -1 only without any monitor.
inline int pick_monitor(int count, int play, int primary) {
    if (count <= 0) return -1;
    if (primary >= 0 && primary < count && primary != play) return primary;
    for (int i = 0; i < count; ++i)
        if (i != play) return i;
    return play >= 0 && play < count ? play : 0;
}

}  // namespace app::stream
