// SPDX-License-Identifier: MIT
#include "vrr.h"

#include <windows.h>
#include <initguid.h>  // before devpkey.h: DEVPKEY_Device_InstanceId is defined here

#include <cfgmgr32.h>
#include <devpkey.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <format>
#include <memory>

#include "app_version.h"
#include "gpu.h"
#include "log.h"

namespace app {
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
// The structures and numbers below are the few the app needs from NVIDIA's
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

// ---- the driver's settings for one program
#pragma pack(push, 4)
struct NvDrsProfile {  // NVDRS_PROFILE_V1
    UINT32 version;
    UINT16 name[2048];
    UINT32 gpu_support, is_predefined, apps, settings;
};
struct NvDrsApplication {  // NVDRS_APPLICATION_V4
    UINT32 version, is_predefined;
    UINT16 name[2048], friendly_name[2048], launcher[2048], file_in_folder[2048];
    UINT32 flags;
    UINT16 command_line[2048];
};
#pragma pack(pop)
static_assert(sizeof(NvDrsProfile) == 4116 && sizeof(NvDrsApplication) == 20492);

constexpr UINT32 kNvDrsSaveSettings = 0xFCBC7E14, kNvDrsCreateProfile = 0xCC176068, kNvDrsDeleteProfile = 0x17093206,
                 kNvDrsProfileInfo = 0x61CD6FD6, kNvDrsFindProfile = 0x7E4A9A0B, kNvDrsCreateApplication = 0x4347A9DE,
                 kNvDrsFindApplication = 0xEEE566B2, kNvDrsSetSetting = 0x577DD202,
                 kNvDrsDeleteSetting = 0xE4A26362;
constexpr UINT32 kNvSettingVrrRequest = 0x1094F1F7;  // VRRREQUESTSTATE_ID: the same three values
constexpr const wchar_t *kOwnProfile = APP_NAME_W L" (mpv)";

void nv_string(const std::wstring &text, UINT16 (&out)[2048]) {
    const size_t n = std::min<size_t>(text.size(), 2047);
    for (size_t i = 0; i < n; ++i) out[i] = UINT16(text[i]);
    out[n] = 0;
}
std::wstring from_nv(const UINT16 (&text)[2048]) {
    std::wstring out;
    for (size_t i = 0; i < 2047 && text[i]; ++i) out += wchar_t(text[i]);
    return out;
}
// The same file: the driver keeps names in lower case, with either slash.
bool same_path(std::wstring a, std::wstring b) {
    const auto plain = [](std::wstring &s) {
        for (wchar_t &c : s) c = c == L'/' ? L'\\' : wchar_t(towlower(c));
    };
    plain(a);
    plain(b);
    return a == b;
}

// The driver's settings store, loaded; changes stay in it until save().
class Drs {
public:
    explicit Drs(const Nvapi &nv) : nv_(nv) {
        const auto create = nv.get<int (*)(void **)>(kNvDrsCreateSession);
        const auto load = nv.get<int (*)(void *)>(kNvDrsLoadSettings);
        if (!nv.ok() || !create || !load || create(&session_) != 0) return;
        ok_ = load(session_) == 0;
    }
    Drs(const Drs &) = delete;
    Drs &operator=(const Drs &) = delete;
    ~Drs() {
        const auto destroy = nv_.get<int (*)(void *)>(kNvDrsDestroySession);
        if (session_ && destroy) destroy(session_);
    }
    bool ok() const { return ok_; }

    // The profile that applies to the program, null when none names it.
    void *find(const std::wstring &exe, NvDrsApplication &app) const {
        const auto f = nv_.get<int (*)(void *, UINT16 *, void **, NvDrsApplication *)>(kNvDrsFindApplication);
        UINT16 name[2048];
        nv_string(exe, name);
        app = {};
        app.version = nv_version(app, 4);
        void *profile = nullptr;
        return f && f(session_, name, &profile, &app) == 0 ? profile : nullptr;
    }
    void *find_profile(const std::wstring &profile_name) const {
        const auto f = nv_.get<int (*)(void *, UINT16 *, void **)>(kNvDrsFindProfile);
        UINT16 name[2048];
        nv_string(profile_name, name);
        void *profile = nullptr;
        return f && f(session_, name, &profile) == 0 ? profile : nullptr;
    }
    bool info(void *profile, NvDrsProfile &p) const {
        const auto f = nv_.get<int (*)(void *, void *, NvDrsProfile *)>(kNvDrsProfileInfo);
        p = {};
        p.version = nv_version(p, 1);
        return f && f(session_, profile, &p) == 0;
    }
    // A new profile holding this program alone; the driver's error number
    // in `status` when it refuses.
    void *create(const std::wstring &profile_name, const std::wstring &exe, int &status) const {
        const auto make = nv_.get<int (*)(void *, NvDrsProfile *, void **)>(kNvDrsCreateProfile);
        const auto add = nv_.get<int (*)(void *, void *, NvDrsApplication *)>(kNvDrsCreateApplication);
        status = -1;
        if (!make || !add) return nullptr;
        void *profile = find_profile(profile_name);
        if (!profile) {
            NvDrsProfile p{};
            p.version = nv_version(p, 1);
            nv_string(profile_name, p.name);
            if ((status = make(session_, &p, &profile)) != 0) return nullptr;
        }
        const auto app = std::make_unique<NvDrsApplication>();
        app->version = nv_version(*app, 4);
        nv_string(exe, app->name);
        nv_string(L"mpv (" APP_NAME_W L")", app->friendly_name);
        status = add(session_, profile, app.get());
        return status == 0 ? profile : nullptr;
    }
    // The global settings, which a program without a profile gets.
    void *base() const {
        const auto f = nv_.get<int (*)(void *, void **)>(kNvDrsBaseProfile);
        void *profile = nullptr;
        return f && f(session_, &profile) == 0 ? profile : nullptr;
    }
    bool remove(void *profile) const {
        const auto f = nv_.get<int (*)(void *, void *)>(kNvDrsDeleteProfile);
        return f && f(session_, profile) == 0;
    }

    // A setting as it is in effect for the profile, -1 when it cannot be
    // read; own: set in this profile, not inherited.
    int get(void *profile, UINT32 id, bool &own) const {
        const auto f = nv_.get<int (*)(void *, void *, UINT32, NvDrsSetting *)>(kNvDrsGetSetting);
        const auto s = std::make_unique<NvDrsSetting>();
        s->version = nv_version(*s, 1);
        own = false;
        if (!f || f(session_, profile, id, s.get()) != 0 || s->current.u32 > 2) return -1;
        own = s->location == 0;  // NVDRS_CURRENT_PROFILE_LOCATION
        return int(s->current.u32);
    }
    bool set(void *profile, UINT32 id, UINT32 value) const {
        const auto f = nv_.get<int (*)(void *, void *, NvDrsSetting *)>(kNvDrsSetSetting);
        const auto s = std::make_unique<NvDrsSetting>();
        s->version = nv_version(*s, 1);
        s->id = id;
        s->type = 0;  // NVDRS_DWORD_TYPE
        s->current.u32 = value;
        return f && f(session_, profile, s.get()) == 0;
    }
    // Back to what the profile inherits.
    bool erase(void *profile, UINT32 id) const {
        const auto f = nv_.get<int (*)(void *, void *, UINT32)>(kNvDrsDeleteSetting);
        return f && f(session_, profile, id) == 0;
    }
    bool save() const {
        const auto f = nv_.get<int (*)(void *)>(kNvDrsSaveSettings);
        return f && f(session_) == 0;
    }

private:
    const Nvapi &nv_;
    void *session_ = nullptr;
    bool ok_ = false;
};

// The launcher's survey thread and the main thread may both ask.
SRWLOCK g_nvapi_lock = SRWLOCK_INIT;
struct NvapiLock {
    NvapiLock() { AcquireSRWLockExclusive(&g_nvapi_lock); }
    ~NvapiLock() { ReleaseSRWLockExclusive(&g_nvapi_lock); }
};

NvidiaAppVrr app_vrr(const Drs &drs, const std::wstring &exe) {
    NvidiaAppVrr out;
    if (!drs.ok()) return out;
    out.read = true;
    const auto app = std::make_unique<NvDrsApplication>();
    void *profile = drs.find(exe, *app);
    if (profile) {
        out.has_profile = true;
        out.own_path = same_path(from_nv(app->name), exe);
        NvDrsProfile p;
        if (drs.info(profile, p)) out.profile = from_nv(p.name);
    } else {
        // No profile of its own: the global settings are what it gets.
        profile = drs.base();
    }
    if (!profile) return out;
    out.mode = drs.get(profile, kNvSettingVrrMode, out.mode_own);
    out.requested = drs.get(profile, kNvSettingVrrRequest, out.requested_own);
    if (!out.has_profile) out.mode_own = out.requested_own = false;
    return out;
}

const wchar_t *vrr_mode_text(int mode) {
    return mode == 0 ? L"off" : mode == 1 ? L"full screen only" : mode == 2 ? L"windowed and full screen" : L"unknown";
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
    {
        const NvapiLock lock;
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
    return out;
}

NvidiaAppVrr read_nvidia_app_vrr(const std::wstring &exe) {
    const NvapiLock lock;
    const Nvapi nv;
    const Drs drs(nv);
    return app_vrr(drs, exe);
}

std::wstring nvidia_app_vrr_text(const NvidiaAppVrr &v) {
    if (!v.read) return L"NVIDIA driver: its program settings could not be read.";
    const std::wstring where = !v.has_profile ? L"no program profile (the global settings apply)"
                                              : L"profile \"" + v.profile + L"\"" +
                                                    (v.own_path ? L"" : L" (for every program of this name)");
    return L"NVIDIA driver, G-SYNC for mpv.exe: " + std::wstring(vrr_mode_text(v.mode)) +
           (v.requested != v.mode ? std::wstring(L", requested ") + vrr_mode_text(v.requested) : L"") + L"; " + where +
           L".";
}

namespace {

// The backup: what the two settings were in the program's own profile
// (-1: not set there), and whether the app made the profile.
struct ProfileBackup {
    std::wstring exe;
    int created = 0, mode = -1, requested = -1;
};
bool read_backup(const std::wstring &file, ProfileBackup &b) {
    FILE *f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rt, ccs=UTF-8") != 0) f = nullptr;
    if (!f) return false;
    wchar_t line[4096];
    int seen = 0;
    while (fgetws(line, 4096, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
        const size_t eq = l.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = l.substr(0, eq), value = l.substr(eq + 1);
        if (key == L"exe") b.exe = value, ++seen;
        else if (key == L"created") b.created = _wtoi(value.c_str()), ++seen;
        else if (key == L"mode") b.mode = _wtoi(value.c_str()), ++seen;
        else if (key == L"requested") b.requested = _wtoi(value.c_str()), ++seen;
    }
    fclose(f);
    return seen == 4 && b.mode >= -1 && b.mode <= 2 && b.requested >= -1 && b.requested <= 2;
}
bool write_backup(const std::wstring &file, const ProfileBackup &b) {
    FILE *f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wt, ccs=UTF-8") != 0) f = nullptr;
    if (!f) return false;
    const bool ok = fwprintf(f, L"exe=%ls\ncreated=%d\nmode=%d\nrequested=%d\n", b.exe.c_str(), b.created, b.mode,
                             b.requested) > 0;
    return fclose(f) == 0 && ok;
}

}  // namespace

bool allow_nvidia_windowed_vrr(const std::wstring &exe, const std::wstring &backup_file, std::wstring &error) {
    const NvapiLock lock;
    const Nvapi nv;
    ProfileBackup backup;
    backup.exe = exe;
    {
        const Drs drs(nv);
        if (!drs.ok()) return error = L"The NVIDIA driver did not answer (no NVIDIA GPU, or no driver)", false;
        const auto app = std::make_unique<NvDrsApplication>();
        void *profile = drs.find(exe, *app);
        std::wstring shared;  // a profile that names every mpv.exe
        if (profile && !same_path(from_nv(app->name), exe)) {
            NvDrsProfile p;
            if (drs.info(profile, p)) shared = from_nv(p.name);
            profile = nullptr;
        }
        if (!profile) {
            int status = 0;
            profile = drs.create(kOwnProfile, exe, status);
            if (!profile) {
                error = shared.empty()
                            ? std::format(L"The NVIDIA driver did not take a profile for {} (error {})", exe, status)
                            : std::format(L"The NVIDIA driver's profile \"{}\" covers every mpv.exe and it took none "
                                          L"for this one alone (error {}). Set \"GSYNC - Application Mode\" and "
                                          L"\"GSYNC - Global Mode\" to \"Fullscreen and Windowed\" in that profile "
                                          L"with NVIDIA Profile Inspector",
                                          shared, status);
                return false;
            }
            backup.created = 1;
        } else {
            bool own = false;
            const int mode = drs.get(profile, kNvSettingVrrMode, own);
            backup.mode = own ? mode : -1;
            const int requested = drs.get(profile, kNvSettingVrrRequest, own);
            backup.requested = own ? requested : -1;
        }
        // The first state is the one to go back to: a second fix keeps it.
        ProfileBackup before;
        if (!(read_backup(backup_file, before) && same_path(before.exe, exe)) && !write_backup(backup_file, backup))
            return error = L"Could not write " + backup_file + L"; nothing was changed", false;
        if (!drs.set(profile, kNvSettingVrrMode, 2) || !drs.set(profile, kNvSettingVrrRequest, 2) || !drs.save())
            return error = L"The NVIDIA driver did not save the settings (it may need " APP_NAME_W L" run as administrator "
                           L"once for this)",
                   false;
    }
    // Read back with the store loaded anew.
    const Drs check(nv);
    const NvidiaAppVrr now = app_vrr(check, exe);
    if (!now.windowed() || !now.own_path)
        return error = L"The NVIDIA driver saved the settings, but reads them back as: " + nvidia_app_vrr_text(now),
               false;
    return true;
}

bool restore_nvidia_app_vrr(const std::wstring &exe, const std::wstring &backup_file, std::wstring &error) {
    ProfileBackup backup;
    if (!read_backup(backup_file, backup) || !same_path(backup.exe, exe))
        return error = L"There is nothing to put back: " APP_NAME_W L" has not changed the NVIDIA driver's settings for " + exe,
               false;
    const NvapiLock lock;
    const Nvapi nv;
    const Drs drs(nv);
    if (!drs.ok()) return error = L"The NVIDIA driver did not answer", false;
    const auto app = std::make_unique<NvDrsApplication>();
    void *profile = drs.find(exe, *app);
    // No profile for the path any more: someone removed it, nothing to undo.
    if (profile && same_path(from_nv(app->name), exe)) {
        NvDrsProfile p;
        const bool known = drs.info(profile, p);
        bool ok = true;
        if (backup.created && known && from_nv(p.name) == kOwnProfile && p.apps <= 1 && p.settings <= 2) {
            ok = drs.remove(profile);  // the app's, with nothing added since
        } else {
            ok = (backup.mode < 0 ? drs.erase(profile, kNvSettingVrrMode)
                                  : drs.set(profile, kNvSettingVrrMode, UINT32(backup.mode))) &&
                 (backup.requested < 0 ? drs.erase(profile, kNvSettingVrrRequest)
                                       : drs.set(profile, kNvSettingVrrRequest, UINT32(backup.requested)));
        }
        if (!ok || !drs.save())
            return error = L"The NVIDIA driver did not save the settings; the backup is kept in " + backup_file, false;
    }
    DeleteFileW(backup_file.c_str());
    return true;
}

void read_vrr(DisplayInfo &d, const std::wstring &monitor_device_path, bool hdmi) {
    const std::vector<unsigned char> bytes = read_edid(monitor_device_path);
    const EdidVrr edid = parse_edid_vrr(bytes.data(), bytes.size());
    NvidiaVrr nvidia;
    if (d.gpu_vendor == kVendorNvidia) nvidia = read_nvidia_vrr(d.device);
    apply_vrr(d, edid.valid ? &edid : nullptr, nvidia.read ? &nvidia : nullptr, hdmi);
}

}  // namespace app
