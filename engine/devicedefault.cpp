// devicedefault.cpp - IPolicyConfig::SetDefaultEndpoint wrapper.
#include "devicedefault.h"

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "log.h"
#include "wasapi_util.h"

namespace sr {

namespace {

// IPolicyConfig vtable layout (reverse-engineered, stable since Vista).
// Only SetDefaultEndpoint is used; the rest keep the vtable offsets correct.
// IID below is the interface id; the COM class id is kClsidPolicyConfig.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId,
                                                         ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

// PolicyConfig COM class (same GUID family).
const CLSID kClsidPolicyConfig = { 0x870af99c, 0x171d, 0x4f9e,
                                   { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

std::wstring ToLowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

} // namespace

std::wstring RenderCounterpartNeedle(const std::wstring& captureName) {
    std::wstring n = ToLowerW(captureName);
    if (n.find(L"cable output") != std::wstring::npos) return L"CABLE In";
    if (n.find(L"voicemeeter out") != std::wstring::npos) return L"Voicemeeter Input";
    if (n.find(L"soundradar") != std::wstring::npos &&
        n.find(L"loopback") != std::wstring::npos)
        return L"SoundRadar"; // matched against render side below, minus loopback
    return L"";
}

bool SetDefaultRenderDevice(const std::wstring& nameSubstring) {
    if (nameSubstring.empty()) return false;
    // find the endpoint id (SoundRadar: skip the loopback-named ones)
    std::wstring id, name;
    for (const DeviceInfo& d : EnumerateEndpoints(eRender)) {
        std::wstring low = ToLowerW(d.name);
        std::wstring needle = ToLowerW(nameSubstring);
        if (low.find(needle) == std::wstring::npos) continue;
        if (needle == L"soundradar" && low.find(L"loopback") != std::wstring::npos)
            continue;
        id = d.id;
        name = d.name;
        break;
    }
    if (id.empty()) {
        Log("setdefault: no render endpoint matching '%s'", ToUtf8(nameSubstring).c_str());
        return false;
    }

    Microsoft::WRL::ComPtr<IPolicyConfig> pc;
    HRESULT hr = CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&pc));
    if (FAILED(hr)) {
        Log("setdefault: CoCreateInstance(IPolicyConfig) hr=0x%08lx", (unsigned long)hr);
        return false;
    }
    HRESULT hr1 = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    HRESULT hr2 = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    Log("setdefault: '%s' -> console hr=0x%08lx multimedia hr=0x%08lx",
        ToUtf8(name).c_str(), (unsigned long)hr1, (unsigned long)hr2);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2);
}

} // namespace sr
