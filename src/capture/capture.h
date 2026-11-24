// SPDX-License-Identifier: MIT
// Reading frames from the card (Media Foundation) or from a raw file or pipe
// (--input-raw, for testing without a card), and handing them to the slot.
#pragma once
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>

#include "devices.h"
#include "frame_slot.h"
#include "repeat_filter.h"
#include "signal_kind.h"

namespace app {

// Opens the device and selects exactly this mode. source is kept so it can be
// shut down: that releases the card and cancels a read still waiting.
HRESULT open_reader(const CaptureDevice &device, const VideoMode &mode,
                    Microsoft::WRL::ComPtr<IMFMediaSource> &source,
                    Microsoft::WRL::ComPtr<IMFSourceReader> &reader);

struct FrameLayout {
    PixelFormat format = PixelFormat::NV12;
    UINT32 width = 0, height = 0;
    size_t bytes() const { return frame_size(format, width, height); }
};

// The card is in use by another app: it opens, but the first read fails.
constexpr HRESULT kCardBusy = static_cast<HRESULT>(0xC00D3704);   // MF_E_HW_MFT_FAILED_START_STREAMING
// Another app took the card while the app was reading it.
constexpr HRESULT kCardTaken = static_cast<HRESULT>(0xC00D3EA3);  // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
// The card's frames come in a layout the app cannot pass on (row pitch, size).
constexpr HRESULT kUnreadableFrames = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);

inline bool card_in_use(HRESULT hr) { return hr == kCardBusy || hr == kCardTaken; }

// Follows whether a 10-bit capture carries SDR or HDR (signal_kind.h): the
// capture thread shows it the frames it sends on, any thread reads found.
// env SIGNAL_LOG=1 logs what it measures every two seconds (else when what
// the frames say changes, and once a minute).
class SignalMonitor {
public:
    // shown: what the picture is shown as now.
    SignalMonitor(Signal shown, UINT32 width, UINT32 height);
    void frame(const BYTE *data);  // capture thread
    std::atomic<Signal> found;

private:
    SignalWatch watch_;
    UINT32 width_, height_;
    bool verbose_ = false;
    ULONGLONG measured_ms_ = 0, window_ms_ = 0, logged_ms_ = 0;
    Signal logged_ = Signal::Unknown;
    bool logged_once_ = false;
};

// What the capture thread passes frames through on their way to mpv.
struct FrameSinks {
    RepeatFilter *repeats = nullptr;  // dedup, or nullptr to send every frame
    SignalMonitor *signal = nullptr;  // SDR or HDR, for video.mode = auto
};

// Hands a frame to the slot, minus repeats when deduplicating - unless the
// frame generation add-on asks for them.
// arrived: when the frame came from the card (QueryPerformanceCounter), 0 for now.
void deliver_frame(FrameSlot &slot, const FrameSinks &sinks, const BYTE *data, size_t bytes, LONG64 arrived = 0);

// Both run until the source ends or fails, or the slot is stopped, and then
// stop the slot. status is S_OK when stopped, S_FALSE when the source ended,
// else the error.
void capture_loop(IMFSourceReader *reader, const FrameLayout &layout, FrameSlot &slot, const FrameSinks &sinks,
                  std::atomic<UINT64> &frames, std::atomic<HRESULT> &status);
void raw_loop(HANDLE in, const FrameLayout &layout, FrameSlot &slot, const FrameSinks &sinks,
              std::atomic<UINT64> &frames, std::atomic<HRESULT> &status);

}  // namespace app
