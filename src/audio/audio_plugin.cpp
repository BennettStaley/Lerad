// SPDX-License-Identifier: MIT
// capture_audio.dll: the capture card's sound, played by mpv's process with
// low latency. An mpv C plugin: mpv loads it from portable_config\scripts.
//
// It captures the card's audio endpoint and plays it on the default output
// device with WASAPI in shared mode, on its own thread, independently of the
// video. The output stream belongs to mpv's process, so apps that capture an
// app's sound (Discord, OBS) see the app. Volume and mute follow mpv's
// properties; boost and the audio delay come from the settings file and the keys.
//
// Based on native-audio/msc_audio.cpp of the WASAPI audio plugin by z-er
// (Josh S) for MPV-SW-Capture, https://github.com/z-er/MPV-SW-Capture,
// MIT licence, Copyright (c) 2026 TyRaS-SW. Modified for this app: audio delay,
// boost to 400 %, endpoint chosen by the app, capture period option, log file.
//
// Options (script-opts, set by the exe):
//   capture_audio-device  endpoint ID or exact name; empty = no audio
//   capture_audio-delay   ms the sound is held back (0-1000)
//   capture_audio-boost   percent (100-400)
//   capture_audio-period  minimum (lowest latency) or default (steadier)
//   capture_audio-log     log file
//   capture_audio-missing 1: the card's audio input was not found (shown)
// Script messages: app-audio-delay +N|-N|N, app-audio-boost +N|-N|N,
// app-audio-restart.

#define MPV_CPLUGIN_DYNAMIC_SYM  // mpv fills in its API when it loads the plugin
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <share.h>
#include <stdexcept>
#include <string>
#include <thread>

#include <mpv/client.h>

#include "app_version.h"
#include "audio_core.h"

namespace {

using namespace app::audio;

constexpr int kMaxDelayMs = 1000;

// ---------------------------------------------------------------- log

std::mutex g_log_mutex;
FILE *g_log = nullptr;

void log(const std::string &text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) return;
    fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text.c_str());
    fflush(g_log);
}

std::string narrow(const wchar_t *w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string &s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ---------------------------------------------------------------- WASAPI helpers

template <class T>
struct Com {
    T *p = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T *operator->() const { return p; }
    T **out() {
        if (p) p->Release();
        p = nullptr;
        return &p;
    }
    void **out_void() { return reinterpret_cast<void **>(out()); }
};

struct Event {
    HANDLE h;
    explicit Event(bool manual = false) : h(CreateEventW(nullptr, manual, FALSE, nullptr)) {
        if (!h) throw std::runtime_error("CreateEvent failed");
    }
    ~Event() { CloseHandle(h); }
};

// Milliseconds on a steady clock (GetTickCount64 steps by up to 16 ms).
double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) throw std::runtime_error(std::format("{} (0x{:08X})", what, static_cast<unsigned long>(hr)));
}

struct Format {
    WAVEFORMATEX *p = nullptr;
    bool floating = false;
    ~Format() { CoTaskMemFree(p); }
    void validate() {
        WORD tag = p->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && p->cbSize >= 22) {
            auto ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(p);
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
                tag = WAVE_FORMAT_PCM;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        if ((!floating && tag != WAVE_FORMAT_PCM) || (floating && p->wBitsPerSample != 32) ||
            (!floating && p->wBitsPerSample != 16 && p->wBitsPerSample != 24 && p->wBitsPerSample != 32) ||
            !p->nChannels || p->nSamplesPerSec < 8000 || p->nSamplesPerSec > 768000 ||
            p->nBlockAlign != p->nChannels * (p->wBitsPerSample / 8))
            throw std::runtime_error(std::format("unsupported audio format: tag {} bits {} channels {} rate {}",
                                                 p->wFormatTag, p->wBitsPerSample, p->nChannels,
                                                 p->nSamplesPerSec));
    }
};

// ---------------------------------------------------------------- engine

class Engine {
public:
    ~Engine() { halt(); }

    std::atomic<double> gain{1.0};
    std::atomic<int> delay_ms{0};
    // underruns: the queue ran dry. starved: the output device was left
    // with nothing. skipped: the card said it lost sound. dropped: samples
    // thrown away to catch up.
    std::atomic<uint64_t> underruns{0}, starved{0}, skipped{0}, dropped{0};
    std::atomic<double> queue_ms{0}, spare_ms{0};

    void start(const std::string &device, bool minimum_period) {
        halt();
        ResetEvent(stop_.h);
        underruns = 0;
        starved = 0;
        skipped = 0;
        dropped = 0;
        set_status("starting");
        thread_ = std::thread([this, device, minimum_period] {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(init)) {
                set_status("error: COM could not start");
                return;
            }
            DWORD task = 0;
            HANDLE priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
            try {
                run(device, minimum_period);
            } catch (const std::exception &e) {
                set_status(std::string("error: ") + e.what());
            }
            if (priority) AvRevertMmThreadCharacteristics(priority);
            CoUninitialize();
        });
    }

    void halt() {
        SetEvent(stop_.h);
        if (thread_.joinable()) thread_.join();
    }

    std::string status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

private:
    void set_status(std::string s) {
        log(s);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::move(s);
    }

    // Returns the period in frames.
    static UINT32 initialize(Com<IAudioClient> &client, IMMDevice *endpoint, Format &fmt, HANDLE event,
                             bool minimum_period) {
        check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.out_void()), "Activate endpoint");
        check(client->GetMixFormat(&fmt.p), "GetMixFormat");
        fmt.validate();
        UINT32 period = 0;
        HRESULT hr = E_FAIL;
        if (minimum_period) {
            // The shortest period the engine allows (often ~3 ms instead of 10).
            Com<IAudioClient3> modern;
            hr = client->QueryInterface(__uuidof(IAudioClient3), modern.out_void());
            if (SUCCEEDED(hr)) {
                UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
                hr = modern->GetSharedModeEnginePeriod(fmt.p, &def, &fundamental, &minimum, &maximum);
                if (SUCCEEDED(hr)) {
                    period = minimum;
                    hr = modern->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, fmt.p,
                                                             nullptr);
                }
            }
            if (FAILED(hr))  // a fresh client: a half-initialized one cannot be retried
                check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.out_void()),
                      "Reactivate endpoint");
        }
        if (FAILED(hr)) {
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt.p, nullptr),
                  "Initialize shared audio");
            REFERENCE_TIME def = 0;
            check(client->GetDevicePeriod(&def, nullptr), "GetDevicePeriod");
            period = UINT32(def * fmt.p->nSamplesPerSec / 10000000);
        }
        check(client->SetEventHandle(event), "SetEventHandle");
        return period;
    }

    void run(const std::string &name, bool minimum_period) {
        Com<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                               enumerator.out_void()),
              "Create device enumerator");
        Com<IMMDeviceCollection> devices;
        check(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.out()), "List capture devices");
        UINT count = 0;
        check(devices->GetCount(&count), "Count capture devices");
        Com<IMMDevice> input, output;
        const std::wstring requested = widen(name);
        std::string input_name;
        for (UINT i = 0; i < count && !input.p; ++i) {
            Com<IMMDevice> candidate;
            check(devices->Item(i, candidate.out()), "Read capture device");
            Com<IPropertyStore> properties;
            std::wstring friendly;
            if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, properties.out()))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                    friendly = value.pwszVal;
                PropVariantClear(&value);
            }
            LPWSTR id = nullptr;
            bool match = !friendly.empty() && friendly == requested;
            if (SUCCEEDED(candidate->GetId(&id))) {
                match = match || requested == id;
                CoTaskMemFree(id);
            }
            if (match) {
                input.p = candidate.p;
                input.p->AddRef();
                input_name = narrow(friendly.c_str());
            }
        }
        if (!input.p) throw std::runtime_error("the card's audio device is not connected (" + name + ")");
        check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, output.out()), "Get default output");

        Event capture_event, render_event;
        Com<IAudioClient> capture_client, render_client;
        Format in, out;
        const UINT32 capture_period = initialize(capture_client, input.p, in, capture_event.h, minimum_period);
        const UINT32 render_period = initialize(render_client, output.p, out, render_event.h, minimum_period);
        Com<IAudioCaptureClient> capture;
        Com<IAudioRenderClient> render;
        check(capture_client->GetService(__uuidof(IAudioCaptureClient), capture.out_void()), "Get capture service");
        check(render_client->GetService(__uuidof(IAudioRenderClient), render.out_void()), "Get render service");
        Com<IAudioSessionControl> session;
        if (SUCCEEDED(render_client->GetService(__uuidof(IAudioSessionControl), session.out_void())))
            session->SetDisplayName(APP_NAME_W, nullptr);

        UINT32 buffer_frames = 0;
        check(render_client->GetBufferSize(&buffer_frames), "Get output buffer size");
        const double input_rate = in.p->nSamplesPerSec, output_rate = out.p->nSamplesPerSec;
        const double nominal_step = input_rate / output_rate;
        // About one output period queued, not the endpoint's whole buffer;
        // 2 ms more each time the device was left with nothing (to 10 ms).
        const UINT32 output_least = std::min(buffer_frames, render_period + UINT32(output_rate * .002));
        const UINT32 output_most = std::min(buffer_frames, render_period + UINT32(output_rate * .012));
        UINT32 output_target = output_least;
        const size_t base = size_t(std::max(input_rate * .012, capture_period + render_period * nominal_step));
        Ring ring(size_t(input_rate * (0.2 + kMaxDelayMs / 1000.0)));
        Margin margin(input_rate);
        Smooth smooth(output_rate);
        spare_ms = margin.ms();
        log(std::format("audio: \"{}\" {} Hz {} ch -> default output {} Hz {} ch; periods {:.2f} / {:.2f} ms "
                        "({}), queue {:.2f} ms + {:.0f} ms spare (more when it runs dry) + delay",
                        input_name, in.p->nSamplesPerSec, in.p->nChannels, out.p->nSamplesPerSec, out.p->nChannels,
                        capture_period * 1000 / input_rate, render_period * 1000 / output_rate,
                        minimum_period ? "minimum" : "default", base * 1000 / input_rate, margin.ms()));

        BYTE *initial = nullptr;
        check(render->GetBuffer(output_target, &initial), "Prime output buffer");
        check(render->ReleaseBuffer(output_target, AUDCLNT_BUFFERFLAGS_SILENT), "Release primed buffer");
        check(capture_client->Start(), "Start capture");
        struct StopClient {
            IAudioClient *p;
            ~StopClient() { p->Stop(); }
        } stop_capture{capture_client.p};
        check(render_client->Start(), "Start output");
        StopClient stop_render{render_client.p};
        set_status("running");

        bool primed = false, first_packet = true;
        double current_gain = 0;
        HANDLE events[] = {stop_.h, capture_event.h, render_event.h};
        double last_capture = now_ms(), last_turn = last_capture;
        // What went wrong is written once a minute, only when something did.
        struct Report {
            double at = 0, longest_wait = 0, longest_turn = 0;
            uint64_t underruns = 0, starved = 0, skipped = 0, dropped = 0;
        } report;
        report.at = last_capture;
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(3, events, FALSE, 1000);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) throw std::runtime_error("audio event wait failed");
            const double now = now_ms();
            const double turn = now - last_turn;  // how long this thread was away
            last_turn = now;
            report.longest_turn = std::max(report.longest_turn, turn);
            UINT32 packet = 0;
            check(capture->GetNextPacketSize(&packet), "Get capture packet size");
            while (packet) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Read capture packet");
                const unsigned bytes = in.p->wBitsPerSample / 8;
                for (UINT32 f = 0; f < frames; ++f) {
                    Stereo sample;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        const BYTE *frame = data + f * in.p->nBlockAlign;
                        sample.left = decode(frame, in.p->wBitsPerSample, in.floating);
                        sample.right =
                            in.p->nChannels > 1 ? decode(frame + bytes, in.p->wBitsPerSample, in.floating) : sample.left;
                    }
                    ring.push(sample);
                }
                check(capture->ReleaseBuffer(frames), "Release capture packet");
                // The first packet after a start always carries the flag.
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !first_packet) ++skipped;
                first_packet = false;
                report.longest_wait = std::max(report.longest_wait, now - last_capture);
                last_capture = now;
                check(capture->GetNextPacketSize(&packet), "Get capture packet size");
            }
            const double waited = now - last_capture;  // for the card's next packet, so far
            if (waited > 5000) throw std::runtime_error("the card stopped sending sound");

            // The queue holds what the devices need, the spare and the
            // delay. Raising the delay waits (silence) until the queue has
            // filled to it; lowering it, or a stall, drops the oldest sound.
            const size_t target = std::min(queue_target(input_rate, base + margin.samples(), delay_ms.load()),
                                           ring.capacity() - 1);
            if (ring.size() > target + size_t(input_rate * .030)) {
                ring.trim(target);
                smooth.cut();
            }
            if (primed && ring.size() + size_t(input_rate * .020) < target) primed = false;
            if (!primed && ring.size() >= target) primed = true;

            UINT32 padding = 0;
            check(render_client->GetCurrentPadding(&padding), "Get output padding");
            // Nothing left in the device: it has played a gap. When this
            // thread came late by a few ms (not a stall), keep more in it.
            if (!padding) {
                ++starved;
                if (turn < Margin::kStallMs)
                    output_target = std::min(output_most, output_target + UINT32(output_rate * .002));
            }
            const UINT32 frames = padding < output_target ? output_target - padding : 0;
            if (frames) {
                BYTE *data = nullptr;
                check(render->GetBuffer(frames, &data), "Get output buffer");
                memset(data, 0, frames * out.p->nBlockAlign);
                // A gentle rate correction keeps the queue at its target
                // although the two devices' clocks differ slightly.
                const double correction =
                    std::clamp((double(ring.size()) - double(target)) / double(target) * .001, -.002, .002);
                const double step = nominal_step * (1 + correction);
                const double requested_gain = gain.load();
                const double gain_step = (requested_gain - current_gain) / std::max(1.0, output_rate * .005);
                bool ran_dry = false;
                for (UINT32 f = 0; f < frames; ++f) {
                    Stereo sample;
                    if (primed && ring.read(step, sample)) {
                        sample = smooth.play(sample);
                    } else {
                        if (primed) {
                            primed = false;
                            ran_dry = true;
                            ++underruns;
                        }
                        sample = smooth.gap();  // dies away: a hard cut is a pop
                    }
                    if (std::abs(requested_gain - current_gain) <= std::abs(gain_step))
                        current_gain = requested_gain;
                    else
                        current_gain += gain_step;
                    BYTE *frame = data + f * out.p->nBlockAlign;
                    if (out.p->nChannels == 1) {
                        encode(frame, float((sample.left + sample.right) * .5 * current_gain), out.p->wBitsPerSample,
                               out.floating);
                    } else {
                        encode(frame, float(sample.left * current_gain), out.p->wBitsPerSample, out.floating);
                        encode(frame + out.p->wBitsPerSample / 8, float(sample.right * current_gain),
                               out.p->wBitsPerSample, out.floating);
                    }
                }
                check(render->ReleaseBuffer(frames, 0), "Release output buffer");
                if (ran_dry)
                    margin.ran_dry(now, std::max(waited, turn));
                else if (primed)
                    margin.left(now, ring.size());
                spare_ms = margin.ms();
            }
            queue_ms = ring.size() * 1000 / input_rate;
            dropped = ring.dropped;

            if (now - report.at >= 60000) {
                const uint64_t u = underruns.load(), s = starved.load(), k = skipped.load(), d = dropped.load();
                if (u != report.underruns || s != report.starved || k != report.skipped || d != report.dropped)
                    log(std::format("last minute: queue ran dry {}, output left empty {}, card lost sound {}, "
                                    "{} samples dropped; longest wait for the card {:.1f} ms, longest this thread "
                                    "was away {:.1f} ms; now {:.0f} ms spare, {:.1f} ms kept in the output",
                                    u - report.underruns, s - report.starved, k - report.skipped, d - report.dropped,
                                    report.longest_wait, report.longest_turn, margin.ms(),
                                    output_target * 1000 / output_rate));
                report = {now, 0, 0, u, s, k, d};
            }
        }
        set_status("stopped");
    }

    Event stop_{true};
    std::thread thread_;
    std::mutex mutex_;
    std::string status_ = "idle";
};

// ---------------------------------------------------------------- mpv side

std::string script_opt(mpv_handle *mpv, const char *key) {
    mpv_node node;
    std::string out;
    if (mpv_get_property(mpv, "script-opts", MPV_FORMAT_NODE, &node) < 0) return out;
    if (node.format == MPV_FORMAT_NODE_MAP)
        for (int i = 0; i < node.u.list->num; ++i)
            if (!strcmp(node.u.list->keys[i], key) && node.u.list->values[i].format == MPV_FORMAT_STRING)
                out = node.u.list->values[i].u.string;
    mpv_free_node_contents(&node);
    return out;
}

int to_int(const std::string &s, int fallback) {
    try {
        return s.empty() ? fallback : std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

// "+10", "-10" or "40" applied to value.
int adjust(int value, const char *arg) {
    if (!arg || !*arg) return value;
    const int n = to_int(arg, 0);
    return (arg[0] == '+' || arg[0] == '-') ? value + n : n;
}

// A message over the picture: the app's overlay shows it (after DLSS 5 and
// frame generation), not mpv's OSD.
void show(mpv_handle *mpv, const std::string &text, int ms = 1500) {
    const std::string duration = std::to_string(ms);
    const char *cmd[] = {"script-message", "app-toast", text.c_str(), duration.c_str(), nullptr};
    mpv_command_async(mpv, 0, cmd);
}

void publish(mpv_handle *mpv, const char *key, const std::string &value) {
    mpv_set_property_string(mpv, key, value.c_str());
}

}  // namespace

extern "C" MPV_EXPORT int mpv_open_cplugin(mpv_handle *mpv) {
    const std::string log_path = script_opt(mpv, "capture_audio-log");
    // Shared, so a bug report can read it while playing.
    if (!log_path.empty()) g_log = _wfsopen(widen(log_path).c_str(), L"w", _SH_DENYNO);
    const std::string device = script_opt(mpv, "capture_audio-device");
    const bool minimum_period = script_opt(mpv, "capture_audio-period") != "default";
    int delay = std::clamp(to_int(script_opt(mpv, "capture_audio-delay"), 0), 0, kMaxDelayMs);
    int boost = std::clamp(to_int(script_opt(mpv, "capture_audio-boost"), 100), 100, 400);

    Engine engine;
    double volume = 100;
    bool muted = false;
    auto update_gain = [&] { engine.gain = output_gain(volume, boost, muted); };
    engine.delay_ms = delay;
    update_gain();
    ULONGLONG missing_notice = 0;  // when to say the card's audio input was not found
    publish(mpv, "user-data/app/audio-delay", std::to_string(delay));
    publish(mpv, "user-data/app/audio-boost", std::to_string(boost));
    if (device.empty()) {
        log("audio off (no device)");
        publish(mpv, "user-data/app/audio-status", "off");
        // Said a moment later: the overlay that shows it connects once
        // the picture is up.
        if (script_opt(mpv, "capture_audio-missing") == "1") missing_notice = GetTickCount64() + 4000;
    } else {
        engine.start(device, minimum_period);
    }

    mpv_observe_property(mpv, 1, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 2, "mute", MPV_FORMAT_FLAG);
    std::string last_status;
    ULONGLONG published = 0;
    for (;;) {
        mpv_event *event = mpv_wait_event(mpv, 0.25);
        if (event->event_id == MPV_EVENT_SHUTDOWN) break;
        if (missing_notice && GetTickCount64() >= missing_notice) {
            missing_notice = 0;
            show(mpv, "No sound: the capture card's audio input was not found. Set [audio] device in " APP_INI ".",
                 8000);
        }
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto *property = static_cast<mpv_event_property *>(event->data);
            if (property->data) {
                if (event->reply_userdata == 1) volume = *static_cast<double *>(property->data);
                if (event->reply_userdata == 2) muted = *static_cast<int *>(property->data) != 0;
            }
            update_gain();
        }
        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            auto *message = static_cast<mpv_event_client_message *>(event->data);
            const char *what = message->num_args > 0 ? message->args[0] : "";
            const char *arg = message->num_args > 1 ? message->args[1] : nullptr;
            // "quiet": from the in-game menu, which shows the value itself.
            const bool quiet = message->num_args > 2 && !strcmp(message->args[2], "quiet");
            if (!strcmp(what, "app-audio-delay")) {
                delay = std::clamp(adjust(delay, arg), 0, kMaxDelayMs);
                engine.delay_ms = delay;
                publish(mpv, "user-data/app/audio-delay", std::to_string(delay));
                if (!quiet) show(mpv, std::format("Audio delay: {} ms", delay));
                log(std::format("audio delay {} ms", delay));
            } else if (!strcmp(what, "app-audio-boost")) {
                boost = std::clamp(adjust(boost, arg), 100, 400);
                update_gain();
                publish(mpv, "user-data/app/audio-boost", std::to_string(boost));
                if (!quiet) show(mpv, std::format("Audio boost: {}%", boost));
                log(std::format("audio boost {}%", boost));
            } else if (!strcmp(what, "app-audio-restart") && !device.empty()) {
                show(mpv, "Audio: restarting");
                engine.start(device, minimum_period);
            }
        }
        if (GetTickCount64() - published >= 500) {
            published = GetTickCount64();
            const std::string status = device.empty() ? "off" : engine.status();
            if (status != last_status) {
                publish(mpv, "user-data/app/audio-status", status);
                if (status.rfind("error:", 0) == 0) show(mpv, "Audio " + status, 8000);
                last_status = status;
            }
            publish(mpv, "user-data/app/audio-stats",
                    std::format("queue={:.1f}ms spare={:.0f}ms underruns={} starved={} skipped={} dropped={} gain={:.3f}",
                                engine.queue_ms.load(), engine.spare_ms.load(), engine.underruns.load(),
                                engine.starved.load(), engine.skipped.load(), engine.dropped.load(),
                                engine.gain.load()));
        }
    }
    engine.halt();
    log(std::format("audio closed: queue ran dry {}, output left empty {}, card lost sound {}, {} samples dropped",
                    engine.underruns.load(), engine.starved.load(), engine.skipped.load(), engine.dropped.load()));
    if (g_log) fclose(g_log);
    g_log = nullptr;
    return 0;
}
