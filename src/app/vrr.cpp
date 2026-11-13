// SPDX-License-Identifier: MIT
#include "vrr.h"

#include <windows.h>
#include <initguid.h>  // before devpkey.h: DEVPKEY_Device_InstanceId is defined here

#include <cfgmgr32.h>
#include <devpkey.h>

#include <format>

#include "gpu.h"
#include "log.h"

namespace glasir {
namespace {

// ---------------------------------------------------------------- EDID

constexpr unsigned kOuiAmd = 0x00001A;
constexpr unsigned kOuiHdmiForum = 0xC45DD8;

// The HDMI Forum's block after its OUI (the vendor-specific block and the
// "sink capability" block share the layout): the VRR range is in its bytes
// 5 and 6. A VRRmin of 0 means no VRR; a VRRmax of 0, no limit of its own.
void hdmi_forum(const unsigned char *x, size_t length, EdidVrr &out) {
    if (length < 7) return;
    const unsigned min = x[5] & 0x3F, max = unsigned(x[5] & 0xC0) << 2 | x[6];
    if (min == 0) return;
    out.hdmi_vrr = true;
    out.hdmi_min_hz = min;
    out.hdmi_max_hz = max;
}

// AMD's block after its OUI: version, flags (bit 0: FreeSync), the lowest
// and highest rate (the highest is 10 bits wide from version 3).
void amd(const unsigned char *x, size_t length, EdidVrr &out) {
    if (length < 4 || !(x[1] & 1)) return;
    const unsigned min = x[2], max = x[0] > 2 && length > 11 ? unsigned(x[11] & 3) << 8 | x[10] : x[3];
    if (min == 0 || max <= min) return;
    out.freesync = true;
    out.freesync_min_hz = min;
    out.freesync_max_hz = max;
}

// One CTA-861 extension block (128 bytes): its data blocks.
void cta_block(const unsigned char *b, EdidVrr &out) {
    const size_t end = b[2];  // where the data blocks stop
    if (end < 4 || end > 127) return;
    for (size_t i = 4; i < end;) {
        const unsigned tag = b[i] >> 5;
        const size_t len = b[i] & 0x1F;
        if (i + 1 + len > end) return;  // a block that runs past the end: stop
        const unsigned char *p = b + i + 1;
        if (tag == 3 && len >= 3) {
            const unsigned oui = p[0] | unsigned(p[1]) << 8 | unsigned(p[2]) << 16;
            if (oui == kOuiAmd) amd(p + 3, len - 3, out);
            if (oui == kOuiHdmiForum) hdmi_forum(p + 3, len - 3, out);
        } else if (tag == 7 && len >= 3 && p[0] == 0x79) {
            hdmi_forum(p + 3, len - 3, out);  // extended tag, two reserved bytes, then the same
        }
        i += 1 + len;
    }
}

// ---------------------------------------------------------------- NVAPI
//
// The structures and numbers below are the few Glasir needs from NVIDIA's
// NVAPI SDK (github.com/NVIDIA/nvapi: nvapi.h, nvapi_interface.h,
// NvApiDriverSettings.h; MIT, Copyright (c) NVIDIA Corporation & Affiliates),
// written out here so the SDK is not needed to build. nvapi64.dll exports
// one function, nvapi_QueryInterface, which gives the others by number.

#pragma pack(push, 8)
struct NvMonitorCapabilities {  // NV_MONITOR_CAPABILITIES_V1
    UINT32 version;
    UINT16 size;
    UINT32 info_type;
    UINT32 connector_type;
    UINT8 valid : 1;
    UINT8 data[49];  // NV_MONITOR_CAPS_GENERIC is its first byte
};
struct NvAdaptiveSyncData {  // NV_GET_ADAPTIVE_SYNC_DATA_V1
    UINT32 version;
    UINT32 max_frame_interval_us;
    UINT32 flags;
    UINT32 last_flip_refresh_count;
    UINT64 last_flip_timestamp;
    UINT32 flip_token;
    UINT32 reserved_ex[3];
};
#pragma pack(pop)
#pragma pack(push, 4)
struct NvDrsSetting {  // NVDRS_SETTING_V1
    UINT32 version;
    UINT16 name[2048];
    UINT32 id, type, location, is_current_predefined, is_predefined_valid;
    union Value {
        UINT32 u32;
        UINT8 raw[4100];  // the largest member, a binary value
    } predefined, current;
};
#pragma pack(pop)
static_assert(sizeof(NvMonitorCapabilities) == 68 && sizeof(NvAdaptiveSyncData) == 40 && sizeof(NvDrsSetting) == 12320);
// Not used: NvAPI_Disp_GetVRRInfo ("VRR is active on the display now"). On
// the PC this was written on (RTX 5090) it answered "not active" for a screen
// with G-SYNC on while mpv played in a focused window on it, and "possible"
// for the screen the driver offers no VRR on. Whether G-SYNC had taken hold
// in that window could not be checked another way, so its answer is left out
// rather than trusted.

template <class T>
UINT32 nv_version(const T &, UINT32 version) {
    return UINT32(sizeof(T)) | version << 16;
}

constexpr UINT32 kNvInitialize = 0x0150E828, kNvUnload = 0xD22BDD7E, kNvDisplayIdByName = 0xAE457190,
                 kNvMonitorCapabilities = 0x3B05C7E1, kNvAdaptiveSyncData = 0xB73D1EE9,
                 kNvDrsCreateSession = 0x0694D52E, kNvDrsDestroySession = 0xDAD9CFF8, kNvDrsLoadSettings = 0x375DBD6B,
                 kNvDrsBaseProfile = 0xDA8466A0, kNvDrsGetSetting = 0x73BF8338;
constexpr UINT32 kNvCapsGeneric = 0x1002;      // NV_MONITOR_CAPS_TYPE_GENERIC
constexpr UINT32 kNvSettingVrrMode = 0x1194F158;  // VRR_MODE_ID: 0 off, 1 full screen only, 2 and windowed

// nvapi64.dll while it is in use.
class Nvapi {
public:
    Nvapi() {
        dll_ = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) return;
        query_ = reinterpret_cast<Query>(GetProcAddress(dll_, "nvapi_QueryInterface"));
        const auto initialize = get<int (*)()>(kNvInitialize);
        ok_ = initialize && initialize() == 0;
    }
    Nvapi(const Nvapi &) = delete;
    Nvapi &operator=(const Nvapi &) = delete;
    ~Nvapi() {
        if (ok_)
            if (const auto unload = get<int (*)()>(kNvUnload)) unload();
        if (dll_) FreeLibrary(dll_);
    }
    bool ok() const { return ok_; }
    // A function by its number, or null (an older driver without it).
    template <class F>
    F get(UINT32 id) const {
        return query_ ? reinterpret_cast<F>(query_(id)) : nullptr;
    }

private:
    using Query = void *(*)(UINT32);
    HMODULE dll_ = nullptr;
    Query query_ = nullptr;
    bool ok_ = false;
};

// "Enable G-SYNC" from the driver's global profile, -1 if it is not there.
int nvidia_vrr_mode(const Nvapi &nv) {
    const auto create = nv.get<int (*)(void **)>(kNvDrsCreateSession);
    const auto destroy = nv.get<int (*)(void *)>(kNvDrsDestroySession);
    const auto load = nv.get<int (*)(void *)>(kNvDrsLoadSettings);
    const auto base = nv.get<int (*)(void *, void **)>(kNvDrsBaseProfile);
    const auto setting = nv.get<int (*)(void *, void *, UINT32, NvDrsSetting *)>(kNvDrsGetSetting);
    void *session = nullptr;
    if (!create || !destroy || !load || !base || !setting || create(&session) != 0) return -1;
    int mode = -1;
    void *profile = nullptr;
    if (load(session) == 0 && base(session, &profile) == 0) {
        static NvDrsSetting s;  // 12 KB: not on the stack; only one thread asks at a time (below)
        s = {};
        s.version = nv_version(s, 1);
        if (setting(session, profile, kNvSettingVrrMode, &s) == 0 && s.current.u32 <= 2) mode = int(s.current.u32);
    }
    destroy(session);
    return mode;
}

std::wstring hz(double v) { return std::format(L"{:.0f}", v); }

}  // namespace

EdidVrr parse_edid_vrr(const unsigned char *e, size_t size) {
    EdidVrr out;
    static const unsigned char header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (!e || size < 128 || memcmp(e, header, 8) != 0) return out;
    out.valid = true;
    // The range limits descriptor of the first block; from EDID 1.4 a flag
    // says the monitor takes every rate in the range.
    const bool continuous = e[18] == 1 && e[19] >= 4 && (e[24] & 1);
    for (size_t at = 54; at + 18 <= 126; at += 18) {
        const unsigned char *d = e + at;
        if (d[0] || d[1] || d[2] || d[3] != 0xFD) continue;
        const double min = d[5] + ((d[4] & 3) == 3 ? 255 : 0), max = d[6] + ((d[4] & 2) ? 255 : 0);
        if (continuous && min > 0 && max > min) {
            out.range = true;
            out.range_min_hz = min;
            out.range_max_hz = max;
        }
    }
    for (size_t block = 1; (block + 1) * 128 <= size; ++block)
        if (e[block * 128] == 0x02) cta_block(e + block * 128, out);
    return out;
}

void apply_vrr(DisplayInfo &d, const EdidVrr *edid, const NvidiaVrr *nvidia, bool hdmi) {
    d.vrr = DisplayInfo::Vrr::Unknown;
    d.vrr_min_hz = d.vrr_max_hz = 0;
    d.vrr_kind.clear();
    d.vrr_source.clear();
    d.vrr_windowed = DisplayInfo::Tri::Unknown;
    auto source = [&](const std::wstring &text) { d.vrr_source += (d.vrr_source.empty() ? L"" : L"; ") + text; };

    // The screen: what it can do and its range. Over HDMI the HDMI range is
    // the one in use; else FreeSync's (DisplayPort Adaptive-Sync screens
    // carry it too).
    bool screen_can = false;
    if (edid && edid->valid) {
        const bool use_hdmi = edid->hdmi_vrr && (hdmi || !edid->freesync);
        if (use_hdmi) {
            d.vrr_kind = L"HDMI VRR";
            d.vrr_min_hz = edid->hdmi_min_hz;
            d.vrr_max_hz = edid->hdmi_max_hz;
            source(L"EDID: HDMI VRR from " + hz(d.vrr_min_hz) +
                   (d.vrr_max_hz > 0 ? L" to " + hz(d.vrr_max_hz) + L" Hz" : L" Hz up"));
        } else if (edid->freesync) {
            d.vrr_kind = L"FreeSync";
            d.vrr_min_hz = edid->freesync_min_hz;
            d.vrr_max_hz = edid->freesync_max_hz;
            source(L"EDID: FreeSync " + hz(d.vrr_min_hz) + L"-" + hz(d.vrr_max_hz) + L" Hz");
        } else {
            source(L"EDID: no FreeSync or HDMI VRR block");
        }
        screen_can = use_hdmi || edid->freesync;
        if (screen_can) d.vrr = DisplayInfo::Vrr::Supported;
    }

    if (!nvidia || !nvidia->read) return;
    if (!nvidia->supported) {
        // The driver has the last word on what works; a screen that can
        // (its EDID) is then one the driver has it off for.
        d.vrr = screen_can ? DisplayInfo::Vrr::Off : DisplayInfo::Vrr::Unsupported;
        source(screen_can ? L"NVIDIA driver: not offered on this screen (G-SYNC is not set up for it, or it is off "
                            L"in the monitor's own menu)"
                          : L"NVIDIA driver: this screen has none");
        return;
    }
    d.vrr_kind = nvidia->true_gsync ? L"G-SYNC" : d.vrr_kind.empty() ? L"G-SYNC Compatible" : d.vrr_kind;
    if (d.vrr_min_hz <= 0 && nvidia->min_hz > 0) d.vrr_min_hz = nvidia->min_hz;
    if (d.vrr_max_hz <= 0 && d.vrr_min_hz > 0 && edid && edid->range) d.vrr_max_hz = edid->range_max_hz;
    if (!nvidia->capable_now) {
        d.vrr = DisplayInfo::Vrr::Off;
        source(L"NVIDIA driver: not at the display mode set now");
    } else if (nvidia->mode == 0) {
        d.vrr = DisplayInfo::Vrr::Off;
        source(L"NVIDIA driver: G-SYNC is switched off");
    } else if (nvidia->mode > 0) {
        d.vrr = DisplayInfo::Vrr::On;
        d.vrr_windowed = nvidia->mode == 2 ? DisplayInfo::Tri::Yes : DisplayInfo::Tri::No;
        source(nvidia->mode == 2 ? L"NVIDIA driver: G-SYNC on, for windowed and full screen mode"
                                 : L"NVIDIA driver: G-SYNC on, for full screen mode only");
    } else {
        d.vrr = DisplayInfo::Vrr::Supported;
        source(L"NVIDIA driver: the screen can; the G-SYNC setting could not be read");
    }
}

std::vector<unsigned char> read_edid(const std::wstring &path) {
    std::vector<unsigned char> edid;
    if (path.empty()) return edid;
    // The monitor's device, then its hardware key ("Device Parameters"),
    // where the monitor driver keeps the EDID it read.
    wchar_t instance[MAX_DEVICE_ID_LEN + 1] = {};
    ULONG size = sizeof(instance) - sizeof(wchar_t);
    DEVPROPTYPE type = 0;
    DEVINST node = 0;
    if (CM_Get_Device_Interface_PropertyW(path.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<BYTE *>(instance), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING || CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return edid;
    HKEY key = nullptr;
    if (CM_Open_DevNode_Key(node, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_HARDWARE) != CR_SUCCESS)
        return edid;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes >= 128 &&
        bytes <= 64 * 1024) {
        edid.resize(bytes);
        if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid.data(), &bytes) != ERROR_SUCCESS) edid.clear();
    }
    RegCloseKey(key);
    return edid;
}

NvidiaVrr read_nvidia_vrr(const std::wstring &device) {
    NvidiaVrr out;
    static SRWLOCK lock = SRWLOCK_INIT;  // the launcher's survey thread and the main thread may both ask
    AcquireSRWLockExclusive(&lock);
    {
        Nvapi nv;
        const auto id_by_name = nv.get<int (*)(const char *, UINT32 *)>(kNvDisplayIdByName);
        const auto capabilities = nv.get<int (*)(UINT32, NvMonitorCapabilities *)>(kNvMonitorCapabilities);
        UINT32 id = 0;
        NvMonitorCapabilities caps{};
        caps.version = nv_version(caps, 1);
        caps.info_type = kNvCapsGeneric;
        if (nv.ok() && id_by_name && capabilities && id_by_name(narrow(device).c_str(), &id) == 0 &&
            capabilities(id, &caps) == 0 && caps.valid) {
            out.read = true;
            // NV_MONITOR_CAPS_GENERIC: bit 0 supportVRR, bit 2 isTrueGsync,
            // bit 4 currentlyCapableOfVRR.
            out.supported = caps.data[0] & 0x01;
            out.true_gsync = caps.data[0] & 0x04;
            out.capable_now = caps.data[0] & 0x10;
            if (out.supported) {
                NvAdaptiveSyncData sync{};
                sync.version = nv_version(sync, 1);
                const auto adaptive = nv.get<int (*)(UINT32, NvAdaptiveSyncData *)>(kNvAdaptiveSyncData);
                if (adaptive && adaptive(id, &sync) == 0 && sync.max_frame_interval_us > 0)
                    out.min_hz = 1e6 / sync.max_frame_interval_us;
                out.mode = nvidia_vrr_mode(nv);
            }
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return out;
}

void read_vrr(DisplayInfo &d, const std::wstring &monitor_device_path, bool hdmi) {
    const std::vector<unsigned char> bytes = read_edid(monitor_device_path);
    const EdidVrr edid = parse_edid_vrr(bytes.data(), bytes.size());
    NvidiaVrr nvidia;
    if (d.gpu_vendor == kVendorNvidia) nvidia = read_nvidia_vrr(d.device);
    apply_vrr(d, edid.valid ? &edid : nullptr, nvidia.read ? &nvidia : nullptr, hdmi);
}

}  // namespace glasir
