#include "wasapi_util.h"

#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace sr {

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool NameContainsAll(const std::wstring& name, const std::wstring& a, const std::wstring& b) {
    std::wstring n = ToLower(name);
    if (!a.empty() && n.find(ToLower(a)) == std::wstring::npos) return false;
    if (!b.empty() && n.find(ToLower(b)) == std::wstring::npos) return false;
    return true;
}

bool IsVirtualAudioName(const std::wstring& name) {
    return NameContainsAll(name, L"voicemeeter") || NameContainsAll(name, L"soundradar");
}

static std::wstring ReadFriendlyName(IMMDevice* dev) {
    Microsoft::WRL::ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return L"";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        name = v.pwszVal;
    }
    PropVariantClear(&v);
    return name;
}

static std::wstring ReadEndpointId(IMMDevice* dev) {
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

std::vector<DeviceInfo> EnumerateEndpoints(EDataFlow flow) {
    std::vector<DeviceInfo> out;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enu;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enu)))) {
        return out;
    }
    Microsoft::WRL::ComPtr<IMMDeviceCollection> col;
    if (FAILED(enu->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;

    Microsoft::WRL::ComPtr<IMMDevice> deflt = GetDefaultEndpoint(flow);
    std::wstring defId = deflt ? ReadEndpointId(deflt.Get()) : L"";

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        DeviceInfo info;
        info.id = ReadEndpointId(dev.Get());
        info.name = ReadFriendlyName(dev.Get());
        info.isDefault = (!defId.empty() && info.id == defId);
        out.push_back(std::move(info));
    }
    return out;
}

Microsoft::WRL::ComPtr<IMMDevice> FindEndpointByName(EDataFlow flow,
                                                     const std::wstring& a,
                                                     const std::wstring& b) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enu;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enu)))) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IMMDeviceCollection> col;
    if (FAILED(enu->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        if (NameContainsAll(ReadFriendlyName(dev.Get()), a, b)) return dev;
    }
    return nullptr;
}

Microsoft::WRL::ComPtr<IMMDevice> GetDefaultEndpoint(EDataFlow flow) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enu;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enu)))) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IMMDevice> dev;
    if (FAILED(enu->GetDefaultAudioEndpoint(flow, eMultimedia, &dev))) return nullptr;
    return dev;
}

std::vector<DeviceInfo> SelectCaptureEndpoints(const std::wstring& configValue) {
    std::vector<DeviceInfo> all = EnumerateEndpoints(eCapture);
    std::vector<DeviceInfo> out;
    std::wstring needle = configValue.empty() ? L"SoundRadar" : configValue;
    bool isDefault = NameContainsAll(L"SoundRadar", needle) &&
                     needle.size() == 10; // case-insensitive "SoundRadar"

    auto appendMatches = [&](const std::wstring& a, const std::wstring& b) {
        for (const DeviceInfo& d : all)
            if (NameContainsAll(d.name, a, b)) out.push_back(d);
    };
    appendMatches(needle, isDefault ? L"loopback" : L"");
    if (out.empty() && isDefault) {
        appendMatches(L"Voicemeeter Out B1", L"");
        if (out.empty()) appendMatches(L"Voicemeeter Output", L"");
    }
    return out;
}

PcmFormat InspectFormat(const WAVEFORMATEX* wfx) {
    PcmFormat f;
    if (!wfx) return f;
    WORD tag = wfx->wFormatTag;
    GUID sub = {};
    bool haveSub = false;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        sub = ext->SubFormat;
        haveSub = true;
    }
    bool isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT) ||
                   (haveSub && sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    bool isPcm = (tag == WAVE_FORMAT_PCM) ||
                 (haveSub && sub == KSDATAFORMAT_SUBTYPE_PCM);
    f.sampleRate = wfx->nSamplesPerSec;
    f.channels = wfx->nChannels;
    f.bits = wfx->wBitsPerSample;
    f.blockAlign = wfx->nBlockAlign;
    if (isFloat && f.bits == 32) {
        f.isFloat = true;
        f.valid = true;
    } else if (isPcm && (f.bits == 16 || f.bits == 32)) {
        f.isFloat = false;
        f.valid = true;
    }
    return f;
}

uint32_t ConvertToFloat(const BYTE* src, uint32_t frames, const PcmFormat& fmt, float* dst) {
    if (fmt.isFloat) {
        std::memcpy(dst, src, static_cast<size_t>(frames) * fmt.channels * sizeof(float));
    } else if (fmt.bits == 16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(src);
        size_t n = static_cast<size_t>(frames) * fmt.channels;
        for (size_t i = 0; i < n; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
    } else { // 32-bit PCM
        const int32_t* s = reinterpret_cast<const int32_t*>(src);
        size_t n = static_cast<size_t>(frames) * fmt.channels;
        for (size_t i = 0; i < n; ++i) dst[i] = s[i] * (1.0f / 2147483648.0f);
    }
    return frames;
}

void ConvertFromFloat(const float* src, uint32_t frames, const PcmFormat& fmt, BYTE* dst) {
    size_t n = static_cast<size_t>(frames) * fmt.channels;
    if (fmt.isFloat) {
        std::memcpy(dst, src, n * sizeof(float));
    } else if (fmt.bits == 16) {
        int16_t* d = reinterpret_cast<int16_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            float x = src[i];
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            d[i] = static_cast<int16_t>(x * 32767.0f);
        }
    } else {
        int32_t* d = reinterpret_cast<int32_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            float x = src[i];
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            d[i] = static_cast<int32_t>(x * 2147483647.0);
        }
    }
}

std::wstring DescribeFormat(const WAVEFORMATEX* wfx) {
    if (!wfx) return L"(null)";
    PcmFormat f = InspectFormat(wfx);
    wchar_t buf[160];
    const wchar_t* kind = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ? L"float"
                        : (wfx->wFormatTag == WAVE_FORMAT_PCM) ? L"pcm"
                        : (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                            ? (f.isFloat ? L"extensible/float" : L"extensible/pcm")
                            : L"other";
    swprintf_s(buf, L"%lu Hz, %lu ch, %s, %lu bit%s",
               static_cast<unsigned long>(f.sampleRate),
               static_cast<unsigned long>(f.channels), kind,
               static_cast<unsigned long>(f.bits),
               f.valid ? L"" : L" (UNSUPPORTED)");
    return buf;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace sr
