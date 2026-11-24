// SPDX-License-Identifier: MIT
// Checks dedup (RepeatFilter), the frame slot's drop counting, repeats sent
// while frame generation is off, and telling SDR from HDR (signal_kind.h), on
// synthetic P010, NV12 and YUY2 frames.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "capture.h"
#include "frame_slot.h"
#include "repeat_filter.h"
#include "signal_kind.h"

using namespace app;

namespace {

constexpr UINT32 W = 640, H = 360;
int g_failures = 0;

std::vector<BYTE> make_frame(UINT16 base10) {
    std::vector<BYTE> f(size_t(W) * H * 3);
    auto *y = reinterpret_cast<UINT16 *>(f.data());
    for (size_t i = 0; i < size_t(W) * H; ++i) y[i] = static_cast<UINT16>((base10 + (i % 7)) << 6);
    return f;
}

// Moves a 2x2 block of luma far, at (x, y): the smallest thing a frame can change.
void poke(std::vector<BYTE> &f, UINT32 x, UINT32 y, UINT16 value10) {
    auto *luma = reinterpret_cast<UINT16 *>(f.data());
    for (UINT32 dy = 0; dy < 2; ++dy)
        for (UINT32 dx = 0; dx < 2; ++dx) luma[size_t(y + dy) * W + x + dx] = static_cast<UINT16>(value10 << 6);
}

void expect(bool got, bool want, const char *what) {
    std::printf("%-58s %s\n", what, got == want ? "ok" : "FAIL");
    if (got != want) ++g_failures;
}

}  // namespace

int main() {
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto a = make_frame(100), b = make_frame(300), c = make_frame(500);
        expect(f.take(a.data()), true, "30-in-60: first A is sent");
        expect(f.take(a.data()), false, "30-in-60: repeated A is dropped");
        expect(f.take(b.data()), true, "30-in-60: B is sent");
        expect(f.take(b.data()), false, "30-in-60: repeated B is dropped");
        expect(f.take(c.data()), true, "30-in-60: C is sent");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto a = make_frame(100), noisy = make_frame(100);
        auto *y = reinterpret_cast<UINT16 *>(noisy.data());
        for (size_t i = 0; i < size_t(W) * H; i += 3) y[i] = static_cast<UINT16>(((y[i] >> 6) + 2) << 6);
        f.take(a.data());
        expect(f.take(noisy.data()), false, "noise of 2 codes counts as a repeat");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto a = make_frame(100);
        f.take(a.data());
        auto tiny = a;
        poke(tiny, 100, 100, 900);  // a 2x2 block on a sampled row and column
        expect(f.take(tiny.data()), false, "one 2x2 block changed (1 sample) stays a repeat");
        auto more = a;
        for (UINT32 i = 0; i < 4; ++i) poke(more, 100 + 8 * i, 200, 900);
        expect(f.take(more.data()), true, "four small spots changed (4 samples) is new");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto f0 = make_frame(100), f1 = make_frame(101), f2 = make_frame(102), f3 = make_frame(103);
        f.take(f0.data());
        expect(f.take(f1.data()), false, "slow fade: +1 code is a repeat");
        expect(f.take(f2.data()), false, "slow fade: +2 codes from the last sent is a repeat");
        expect(f.take(f3.data()), true, "slow fade: +3 codes from the last sent goes out");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 60);
        auto a = make_frame(100);
        f.take(a.data());
        expect(f.keepalive(), false, "keepalive: the first frame is new, not a keepalive");
        expect(f.take(a.data()), false, "keepalive: repeat right away is dropped");
        Sleep(120);
        expect(f.take(a.data()), true, "keepalive: repeat after the keepalive is sent");
        expect(f.keepalive(), true, "keepalive: and is marked as one (not a new frame)");
        expect(f.take(a.data()), false, "keepalive: and the next repeat is dropped again");
        auto b = make_frame(300);
        expect(f.take(b.data()) && !f.keepalive(), true, "keepalive: a new frame after it is new again");
    }
    {
        // A mode of 30 fps is taken at its word until the card sends more.
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000, true, 30.0);
        auto a = make_frame(100), b = make_frame(300);
        f.take(a.data());
        expect(f.take(a.data()), true, "mode's rate trusted: a still picture's frame is new");
        f.measured(59.94);
        expect(f.trusted(), true, "mode's rate: one fast second is not enough");
        f.measured(30.0);
        f.measured(59.94);
        expect(f.trusted(), true, "mode's rate: nor two with a slow one between");
        f.measured(59.94);
        expect(f.trusted(), false, "mode's rate: two fast seconds, 60 in a 30 mode");
        expect(f.take(b.data()), true, "60 in a 30 mode: B is sent");
        expect(f.take(b.data()), false, "60 in a 30 mode: repeated B is dropped");
        f.measured(29.97);
        f.measured(30.1);
        expect(f.trusted(), true, "mode's rate: trusted again at 30");
        expect(f.take(b.data()), true, "mode's rate trusted again: every frame is new");
    }
    {
        // FrameSlot's drop counting, which the frame generation add-on backs off on.
        auto a = make_frame(100);
        FrameGenLink link{};
        FrameSlot slot(a.size());
        slot.set_link(&link);
        UINT64 published = 0, dropped = 0;
        slot.publish(a.data(), a.size(), true);
        slot.take(0);
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 0 && link.dropped == 0, true, "slot: a frame taken before the next is not dropped");
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 1 && link.dropped == 1, true, "slot: a new frame overwritten before mpv took it is dropped");
        slot.take(0);
        slot.publish(a.data(), a.size(), false);
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 1, true, "slot: an overwritten keepalive repeat is not counted");
        expect(published == 4 && link.published == 4, true, "slot: keepalive repeats are not published as new");
        expect(link.stamps[3] >= link.stamps[0] && link.stamps[0] > 0, true, "slot: new frames are stamped in order");
        // A new frame (published last above) is waiting; a keepalive must not displace it.
        slot.publish(a.data(), a.size(), false);
        slot.counts(published, dropped);
        expect(dropped == 1 && link.dropped == 1, true, "slot: a keepalive behind a waiting new frame is no drop");
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 2, true, "slot: and the waiting new frame still counts if a new one replaces it");
    }
    {
        // Repeats sent while frame generation is off (the add-on sets send_repeats).
        auto a = make_frame(100), b = make_frame(300);
        FrameGenLink link{};
        FrameSlot slot(a.size());
        slot.set_link(&link);
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        const FrameSinks sinks{&f};
        UINT64 published = 0, dropped = 0;
        deliver_frame(slot, sinks, a.data(), a.size());
        slot.take(0);
        deliver_frame(slot, sinks, a.data(), a.size());
        expect(slot.take(0) != nullptr, false, "send repeats off: a repeat is not sent");
        link.send_repeats = 1;
        deliver_frame(slot, sinks, a.data(), a.size());
        expect(slot.take(0) != nullptr, true, "send repeats on: a repeat is sent");
        slot.counts(published, dropped);
        expect(published == 1 && link.published == 1, true, "send repeats on: the repeat is not stamped as new");
        deliver_frame(slot, sinks, b.data(), b.size());
        deliver_frame(slot, sinks, b.data(), b.size());
        slot.counts(published, dropped);
        expect(published == 2 && dropped == 0, true, "send repeats on: a repeat does not displace a waiting new frame");
        slot.take(0);
        deliver_frame(slot, sinks, b.data(), b.size());
        deliver_frame(slot, sinks, a.data(), a.size());
        slot.counts(published, dropped);
        expect(published == 3 && dropped == 0, true, "send repeats on: a new frame replacing a waiting repeat is no drop");
        link.send_repeats = 0;
        slot.take(0);
        deliver_frame(slot, sinks, a.data(), a.size());
        expect(slot.take(0) != nullptr, false, "send repeats off again: repeats are dropped again");
    }
    {
        // Every frame a game frame (frames.every_frame): no filter, so a still
        // picture goes out as new frames, each stamped into the link.
        expect(capture_rate_is_game_rate(30.0) && capture_rate_is_game_rate(29.97) && capture_rate_is_game_rate(24.0),
               true, "every frame: a capture of 30 fps or less is the game's rate");
        expect(capture_rate_is_game_rate(60.0) || capture_rate_is_game_rate(59.94) || capture_rate_is_game_rate(0),
               false, "every frame: not a capture of 60 fps, nor an unknown rate");
        auto a = make_frame(100);
        FrameGenLink link{};
        FrameSlot slot(a.size());
        slot.set_link(&link);
        const FrameSinks sinks{};
        UINT64 published = 0, dropped = 0;
        for (int i = 0; i < 3; ++i) {
            deliver_frame(slot, sinks, a.data(), a.size());
            expect(slot.take(0) != nullptr, true, "every frame: a still picture's frame is sent");
        }
        slot.counts(published, dropped);
        expect(published == 3 && link.published == 3 && dropped == 0, true, "every frame: each one is stamped as new");
    }
    {
        // The game's rhythm held (CadenceHold): a 30 fps game in a 60 fps
        // capture, then a picture that hardly moves.
        RepeatFilter f(PixelFormat::P010, W, H, 3, 100000, true);
        bool pairs_ok = true;
        for (int n = 0; n < 12; ++n) {
            auto a = make_frame(static_cast<UINT16>(100 + 20 * n));
            pairs_ok = f.take(a.data()) && pairs_ok;
            pairs_ok = !f.take(a.data()) && pairs_ok;
        }
        expect(pairs_ok, true, "hold: A A B B goes out as A B");
        expect(f.cadence() == CadenceHold::Mode::Alternate, true, "hold: and is seen as every other frame");
        auto still = make_frame(400);
        f.take(still.data());
        f.take(still.data());
        bool held = true;
        for (int n = 0; n < 20; ++n) {
            held = f.take(still.data()) && held;
            held = !f.take(still.data()) && held;
        }
        expect(held, true, "hold: a still picture keeps every other frame going out");
        // A slow fade: 1 code a game frame, under the threshold for two of three.
        bool fade = true;
        for (int n = 1; n <= 12; ++n) {
            auto a = make_frame(static_cast<UINT16>(400 + n));
            fade = f.take(a.data()) && fade;
            fade = !f.take(a.data()) && fade;
        }
        expect(fade, true, "hold: a slow fade keeps every other frame going out");
        // The game runs a frame late: A A A B B. B is new, and its turn from then on.
        auto b = make_frame(700), c = make_frame(900);
        f.take(still.data());
        expect(f.take(b.data()), true, "hold: a frame out of turn that moved is sent");
        expect(f.take(b.data()), false, "hold: and its repeat is not");
        expect(f.take(c.data()), true, "hold: the turn is the late frame's from then on");
        expect(f.take(c.data()), false, "hold: (its repeat dropped)");

        // The game goes to 60 fps: every frame new, also once the picture stands still.
        bool all = true;
        for (int n = 0; n < 20; ++n) {
            auto a = make_frame(static_cast<UINT16>(100 + 20 * n));
            all = f.take(a.data()) && all;
        }
        expect(all, true, "hold: a 60 fps game's frames all go out");
        expect(f.cadence() == CadenceHold::Mode::Every, true, "hold: and it is seen as every frame");
        auto last = make_frame(480);
        f.take(last.data());
        all = true;
        for (int n = 0; n < 10; ++n) all = f.take(last.data()) && all;
        expect(all, true, "hold: a still picture then keeps every frame going out");
        // Back to 30 fps.
        for (int n = 0; n < 12; ++n) {
            auto a = make_frame(static_cast<UINT16>(120 + 20 * n));
            f.take(a.data());
            f.take(a.data());
        }
        expect(f.cadence() == CadenceHold::Mode::Alternate, true, "hold: back at 30 fps it is every other frame again");

        // An uneven rate (A A B C C D): no rhythm, pictures compared one by one.
        RepeatFilter u(PixelFormat::P010, W, H, 3, 100000, true);
        bool uneven = true;
        for (int n = 0; n < 12; ++n) {
            auto a = make_frame(static_cast<UINT16>(100 + 40 * n)), b2 = make_frame(static_cast<UINT16>(120 + 40 * n));
            uneven = u.take(a.data()) && uneven;
            uneven = !u.take(a.data()) && uneven;
            uneven = u.take(b2.data()) && uneven;
        }
        expect(uneven && u.cadence() == CadenceHold::Mode::Free, true, "hold: an uneven rate is compared frame by frame");
        expect(u.take(make_frame(100 + 40 * 11 + 20).data()), false, "hold: (and a repeat there is dropped)");

        // Without the hold nothing changes: a still picture after A A B B is all repeats.
        RepeatFilter plain(PixelFormat::P010, W, H, 3, 100000);
        for (int n = 0; n < 12; ++n) {
            auto a = make_frame(static_cast<UINT16>(100 + 20 * n));
            plain.take(a.data());
            plain.take(a.data());
        }
        bool none = true;
        auto rest = make_frame(320);
        for (int n = 0; n < 8; ++n) none = !plain.take(rest.data()) && none;
        expect(none, true, "no hold: a still picture's repeats are dropped as before");
    }
    {
        // NV12 (SDR): 8-bit luma, compared in 10-bit code values.
        expect(frame_size(PixelFormat::NV12, W, H) == size_t(W) * H * 3 / 2 &&
                   frame_size(PixelFormat::P010, W, H) == size_t(W) * H * 3,
               true, "frame sizes: NV12 1.5 bytes a pixel, P010 3");
        auto nv12 = [](BYTE base) {
            std::vector<BYTE> f(frame_size(PixelFormat::NV12, W, H), 128);  // chroma stays grey
            for (size_t i = 0; i < size_t(W) * H; ++i) f[i] = static_cast<BYTE>(base + (i % 7));
            return f;
        };
        auto a = nv12(40), a1 = nv12(41), a2 = nv12(42), b = nv12(120);
        RepeatFilter f(PixelFormat::NV12, W, H, 2, 100000);
        expect(f.take(a.data()), true, "nv12 30-in-60: first A is sent");
        expect(f.take(a.data()), false, "nv12 30-in-60: repeated A is dropped");
        expect(f.take(b.data()), true, "nv12 30-in-60: B is sent");
        expect(f.take(b.data()), false, "nv12 30-in-60: repeated B is dropped");
        RepeatFilter g(PixelFormat::NV12, W, H, 2, 100000);
        g.take(a.data());
        expect(g.take(a1.data()), true, "nv12 threshold 2: one 8-bit step (4 codes) is new");
        RepeatFilter h(PixelFormat::NV12, W, H, 4, 100000);
        h.take(a.data());
        expect(h.take(a1.data()), false, "nv12 threshold 4: one 8-bit step is a repeat");
        expect(h.take(a2.data()), true, "nv12 threshold 4: two steps from the last sent go out");
        auto chroma = a;
        for (size_t i = size_t(W) * H; i < chroma.size(); ++i) chroma[i] = 200;
        RepeatFilter c(PixelFormat::NV12, W, H, 2, 100000);
        c.take(a.data());
        expect(c.take(chroma.data()), false, "nv12: the chroma plane is not compared (luma only)");
        auto spot = a;
        for (UINT32 i = 0; i < 4; ++i)
            for (UINT32 dy = 0; dy < 2; ++dy)
                for (UINT32 dx = 0; dx < 2; ++dx) spot[size_t(200 + dy) * W + 100 + 8 * i + dx] = 250;
        expect(c.take(spot.data()), true, "nv12: four small spots changed (4 samples) is new");
    }
    {
        // A source that is not bit-exact on repeats, like the PS5: every
        // refresh adds faint noise to a fraction of the pixels. Repeats must
        // still be dropped at the default threshold (3), and real changes -
        // a fade, a small HUD change - must still go out.
        UINT32 seed = 12345;
        auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;  // 24 bits
        };
        // Per pixel: +-1 step on 15%, +-2 on 3%, +-3 on 0.5% (in the format's own steps).
        auto noise = [&rnd]() {
            const UINT32 r = rnd() % 1000, sign = rnd() & 1;
            const int m = r < 5 ? 3 : r < 35 ? 2 : r < 185 ? 1 : 0;
            return sign ? -m : m;
        };
        auto nv12 = [](int base) {
            std::vector<BYTE> f(frame_size(PixelFormat::NV12, W, H), 128);
            for (UINT32 y = 0; y < H; ++y)
                for (UINT32 x = 0; x < W; ++x) f[size_t(y) * W + x] = static_cast<BYTE>(base + (x * 7 + y * 13) % 64);
            return f;
        };
        auto noisy_nv12 = [&](const std::vector<BYTE> &clean) {
            auto f = clean;
            for (size_t i = 0; i < size_t(W) * H; ++i) f[i] = static_cast<BYTE>(std::clamp(f[i] + noise(), 0, 255));
            return f;
        };
        RepeatFilter f(PixelFormat::NV12, W, H, 3, 100000);
        int sent = 0, sent_new = 0;
        for (int k = 0; k < 20; ++k) {
            const auto clean = nv12(30 + k * 3);  // each game frame moves up 3 steps
            sent_new += f.take(noisy_nv12(clean).data());
            sent += f.take(noisy_nv12(clean).data());
        }
        expect(sent_new == 20 && sent == 0, true, "noisy nv12 30-in-60: every new frame sent, no repeat");

        RepeatFilter s(PixelFormat::NV12, W, H, 3, 100000);
        auto a = nv12(40);
        s.take(noisy_nv12(a).data());
        for (int k = 0; k < 30; ++k) {
            if (s.take(noisy_nv12(a).data())) {
                expect(true, false, "noisy nv12: 30 repeats of a still frame dropped");
                break;
            }
            if (k == 29) expect(true, true, "noisy nv12: 30 repeats of a still frame dropped");
        }
        auto hud = a;  // a digit-sized change: 8x12 pixels from dark to bright
        for (UINT32 y = 300; y < 312; ++y)
            for (UINT32 x = 500; x < 508; ++x) hud[size_t(y) * W + x] = 230;
        expect(s.take(noisy_nv12(hud).data()), true, "noisy nv12: a digit-sized HUD change is new");
        auto fade = nv12(41);  // the whole picture one 8-bit step brighter (back without the HUD change)
        s.take(noisy_nv12(a).data());
        expect(s.take(noisy_nv12(fade).data()), true, "noisy nv12: a fade of one 8-bit step is new");

        // P010: the noise in 10-bit steps (+-1..3, as the 8-bit noise above but finer).
        auto p010 = [](int base) {
            std::vector<BYTE> f(frame_size(PixelFormat::P010, W, H));
            auto *y10 = reinterpret_cast<UINT16 *>(f.data());
            for (UINT32 y = 0; y < H; ++y)
                for (UINT32 x = 0; x < W; ++x)
                    y10[size_t(y) * W + x] = static_cast<UINT16>((base + (x * 7 + y * 13) % 256) << 6);
            return f;
        };
        auto noisy_p010 = [&](const std::vector<BYTE> &clean) {
            auto f = clean;
            auto *y10 = reinterpret_cast<UINT16 *>(f.data());
            for (size_t i = 0; i < size_t(W) * H; ++i)
                y10[i] = static_cast<UINT16>(std::clamp((y10[i] >> 6) + 2 * noise(), 0, 1023) << 6);
            return f;
        };
        RepeatFilter p(PixelFormat::P010, W, H, 3, 100000);
        sent = sent_new = 0;
        for (int k = 0; k < 20; ++k) {
            const auto clean = p010(100 + k * 8);
            sent_new += p.take(noisy_p010(clean).data());
            sent += p.take(noisy_p010(clean).data());
        }
        expect(sent_new == 20 && sent == 0, true, "noisy p010 30-in-60 (noise up to 6): new frames sent, no repeat");
    }
    {
        // YUY2 (cheap USB cards): packed Y U Y V, luma in every other byte.
        expect(frame_size(PixelFormat::YUY2, W, H) == size_t(W) * H * 2, true, "frame sizes: YUY2 2 bytes a pixel");
        auto yuy2 = [](BYTE base, BYTE chroma) {
            std::vector<BYTE> f(frame_size(PixelFormat::YUY2, W, H));
            for (size_t i = 0; i < size_t(W) * H; ++i) {
                f[i * 2] = static_cast<BYTE>(base + (i % 7));
                f[i * 2 + 1] = chroma;
            }
            return f;
        };
        auto a = yuy2(40, 128), b = yuy2(120, 128), a_chroma = yuy2(40, 200);
        RepeatFilter f(PixelFormat::YUY2, W, H, 2, 100000);
        expect(f.take(a.data()), true, "yuy2 30-in-60: first A is sent");
        expect(f.take(a.data()), false, "yuy2 30-in-60: repeated A is dropped");
        expect(f.take(a_chroma.data()), false, "yuy2: chroma bytes are not compared");
        expect(f.take(b.data()), true, "yuy2 30-in-60: B is sent");
        std::vector<BYTE> uyvy(a.size());
        for (size_t i = 0; i + 1 < a.size(); i += 2) {
            uyvy[i] = a[i + 1];
            uyvy[i + 1] = a[i];
        }
        expect(luma10(PixelFormat::UYVY, uyvy.data(), 5) == luma10(PixelFormat::YUY2, a.data(), 5), true,
               "uyvy: luma read from the odd bytes");
    }
    {
        // SDR or HDR, from a 10-bit frame (signal_kind.h). Grey chroma to start with.
        auto frame = [](UINT16 luma) {
            auto f = make_frame(luma);
            auto *c = reinterpret_cast<UINT16 *>(f.data()) + size_t(W) * H;
            for (size_t i = 0; i < size_t(W) * H / 2; ++i) c[i] = static_cast<UINT16>((512 + (i % 5) * 8) << 6);
            return f;
        };
        // A patch of 1 % of the picture: luma, or both chroma channels.
        auto patch = [](std::vector<BYTE> &f, bool chroma, UINT16 value) {
            auto *p = reinterpret_cast<UINT16 *>(f.data()) + (chroma ? size_t(W) * H : 0);
            const UINT32 rows = chroma ? H / 20 : H / 10;
            for (UINT32 y = 0; y < rows; ++y)
                for (UINT32 x = 0; x < W / 10; ++x) p[size_t(y) * W + x] = static_cast<UINT16>(value << 6);
        };
        auto says = [](const std::vector<BYTE> &f) { return frame_says(measure_signal(f.data(), W, H)); };
        auto hdr = frame(200);  // as a console's HDR looked: luma top near 570, colour top near 60
        patch(hdr, false, 572);
        const SignalStats s = measure_signal(hdr.data(), W, H);
        expect(s.luma_top >= 568 && s.luma_top <= 572 && s.colour_top >= 28 && s.colour_top <= 32 && s.mean > 200 &&
                   s.mean < 210,
               true, "signal: the top 0.1 % of luma and colour, and the mean");
        expect(says(hdr) == Signal::Hdr, true, "signal: lit, no white, weak colour: HDR");
        auto white = frame(300);
        patch(white, false, 936);
        expect(says(white) == Signal::Sdr, true, "signal: white near the top of the range: SDR");
        auto colour = frame(130);  // a dark SDR scene: no white, strong colour
        patch(colour, false, 750);
        patch(colour, true, 512 + 290);
        expect(says(colour) == Signal::Sdr, true, "signal: a dark scene with strong colour: SDR");
        auto blue = frame(130);
        patch(blue, true, 512 - 290);
        expect(says(blue) == Signal::Sdr, true, "signal: colour below grey counts the same");
        expect(says(frame(100)) == Signal::Unknown, true, "signal: a nearly black frame says nothing");
        auto between = frame(300);
        patch(between, false, 840);
        expect(says(between) == Signal::Unknown, true, "signal: luma between HDR's and SDR's says nothing");
        auto tinted = frame(300);
        patch(tinted, true, 512 + 224);
        expect(says(tinted) == Signal::Unknown, true, "signal: colour between HDR's and SDR's says nothing");

        // Believed only after a while without the other kind.
        const SignalStats is_hdr = measure_signal(hdr.data(), W, H), is_sdr = measure_signal(white.data(), W, H),
                          unsure = measure_signal(between.data(), W, H);
        SignalWatch watch(Signal::Hdr);
        ULONGLONG t = 1000;
        bool changed = false;
        for (int i = 0; i < 29 && !changed; ++i, t += 33) changed = watch.add(is_sdr, t);
        expect(!changed && watch.found() == Signal::Hdr, true, "watch: under a second of SDR frames is not believed");
        watch.add(is_hdr, t);  // one frame as shown: the wait starts again
        for (int i = 0; i < 29 && !changed; ++i, t += 33) changed = watch.add(is_sdr, t += 0);
        expect(!changed, true, "watch: an HDR frame in between starts the wait again");
        for (int i = 0; i < 5 && !changed; ++i, t += 33) changed = watch.add(i % 2 ? unsure : is_sdr, t);
        expect(changed && watch.found() == Signal::Sdr, true, "watch: SDR after a second; frames that say nothing do not reset it");
        changed = false;
        int frames = 0;
        for (; frames < 400 && !changed; ++frames, t += 33) changed = watch.add(is_hdr, t);
        expect(changed && watch.found() == Signal::Hdr && frames > 150 && frames < 160, true,
               "watch: HDR is believed after five seconds");
        SignalWatch slow(Signal::Hdr);
        expect(!slow.add(is_sdr, 0) && !slow.add(is_sdr, 5000) && !slow.add(is_sdr, 10000), true,
               "watch: three frames are too few, however far apart");
    }
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
