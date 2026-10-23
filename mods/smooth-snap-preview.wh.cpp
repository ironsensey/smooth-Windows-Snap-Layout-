// ==WindhawkMod==
// @id              smooth-snap-preview
// @name            Smooth Snap Preview (Experimental)
// @description     Shortens identified native snap-preview compositor animations while preserving their easing.
// @version         0.1.0
// @author          ironsensey
// @github          https://github.com/ironsensey
// @license         MIT
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Smooth Snap Preview — experimental

Targets the large translucent placement preview shown while moving between
Windows 11 snap targets. Keeps native rendering and easing; caps identified
preview animations at 90 ms by default. It does not replace Windows' snapping
logic, force an FPS, disable system animations, or guarantee stutter-free output.

**This version is a prototype, not a verified fix.** No Windows build has yet
been runtime-tested. It discovers candidate preview functions from Microsoft
symbols, then changes a compositor duration only when a candidate is on the
calling stack. The relationship between those candidates and the screenshot's
preview still needs confirmation on a Windows PC. If discovery finds nothing,
initialization fails rather than applying a system-wide timing change. Even
successful initialization does not prove that the preview uses this API.

Use Windows 11 x64 and Windhawk 1.4 or later. Open the snap bar once before
loading the mod so lazy shell modules can be discovered. Internet access to
Microsoft's symbol server may be needed the first time. Enable debug logging
in Windhawk and the Diagnostics setting, then drag across several snap targets.
Look for `Preview duration changed`; `Hook installed` alone is not validation.
Report your winver version/build and the log if there is no visible effect.

Settings: start at 90 ms; try 60 ms for a faster transition or 120 ms for a
softer one. Disable the mod to return to native timings. Animations already
created retain their duration until Windows replaces them.

See the repository README for installation and the Windows validation checklist.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- maximumDurationMs: 90
  $name: Maximum preview animation duration (ms)
  $description: Clamped to 40–250 ms. Shorter native animations are left alone. Animations remain enabled.
- diagnostics: false
  $name: Diagnostics
  $description: Log the first 20 matching duration changes per load. Also enable debug logging in Windhawk.
*/
// ==/WindhawkModSettings==

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The portable policy is compiled directly by the tests; there is no duplicate
// test implementation. Keep this file self-contained for Windhawk's editor.
namespace snap_policy {
constexpr int ClampDurationMs(int value) {
    return std::clamp(value, 40, 250);
}

constexpr std::int64_t CapDuration(std::int64_t ticks, int maximumMs) {
    // TimeSpan uses 100 ns units. Preserve zero/negative values so Windows,
    // rather than the mod, remains responsible for validating invalid input.
    return ticks <= 0 ? ticks : std::min(ticks,
        static_cast<std::int64_t>(ClampDurationMs(maximumMs)) * 10000);
}

bool IsPreviewCandidate(std::wstring_view symbol) {
    // Inspect only the function's declaring scope, not parameter/return types.
    // This is a discovery heuristic, NOT evidence of build compatibility.
    auto args = symbol.find(L'(');
    if (args == std::wstring_view::npos) return false;
    auto scopeEnd = symbol.rfind(L"::", args);
    if (scopeEnd == std::wstring_view::npos) return false;
    auto start = symbol.rfind(L' ', scopeEnd);
    start = start == std::wstring_view::npos ? 0 : start + 1;
    std::wstring scope(symbol.substr(start, scopeEnd - start));
    for (auto& c : scope) {
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    }
    // Picker, flyout, and bar animations are not the placement preview.
    for (auto excluded : {L"picker", L"flyout", L"snapbar"}) {
        if (scope.find(excluded) != std::wstring::npos) return false;
    }
    return (scope.find(L"snap") != std::wstring::npos &&
            scope.find(L"preview") != std::wstring::npos) ||
           scope.find(L"zonepreview") != std::wstring::npos;
}

struct Range {
    std::uintptr_t begin;
    std::uintptr_t end; // exclusive, from the x64 runtime function table
};

void NormalizeRanges(std::vector<Range>& ranges) {
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
        [](auto r) { return r.begin >= r.end; }), ranges.end());
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) {
        return a.begin < b.begin;
    });
    std::size_t output = 0;
    for (auto r : ranges) {
        if (output && r.begin <= ranges[output - 1].end) {
            ranges[output - 1].end = std::max(ranges[output - 1].end, r.end);
        } else {
            ranges[output++] = r;
        }
    }
    ranges.resize(output);
}

bool Contains(const std::vector<Range>& ranges, std::uintptr_t pc) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), pc,
        [](auto address, auto r) { return address < r.begin; });
    return it != ranges.begin() && pc < std::prev(it)->end;
}
} // namespace snap_policy

#ifndef SMOOTH_SNAP_POLICY_TEST
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <windhawk_api.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>

namespace {
std::atomic<int> g_maximumMs{90};
std::atomic<bool> g_diagnostics{false};
std::atomic<unsigned> g_changed{0};
std::atomic<unsigned> g_logged{0};
std::vector<snap_policy::Range> g_ranges; // immutable once hooks become active
std::vector<HMODULE> g_modules; // keep all inspected/hooked code mapped

// Stable public WinRT ABI, from Microsoft's Windows.UI.Composition IDL.
// IInspectable occupies slots 0–5. IKeyFrameAnimation::put_Duration is slot 9.
// TimeSpan is a struct containing one signed 64-bit Duration field (100 ns).
// Sources and slot derivation are documented in docs/architecture.md.
constexpr GUID kCompositor = {0xb403ca50, 0x7f8c, 0x4e83,
    {0x98, 0x5f, 0xcc, 0x45, 0x06, 0x00, 0x36, 0xd8}};
constexpr GUID kKeyFrameAnimation = {0x126e7f22, 0x3ae9, 0x4540,
    {0x9a, 0x8a, 0xde, 0xae, 0x8a, 0x4a, 0x4a, 0x84}};
struct TimeSpan { INT64 Duration; };
using PutDuration = HRESULT (STDMETHODCALLTYPE*)(IUnknown*, TimeSpan);
using CreateAnimation = HRESULT (STDMETHODCALLTYPE*)(IUnknown*, IUnknown**);
constexpr std::array<std::size_t, 6> kFactorySlots{6, 19, 20, 26, 27, 28};
std::array<void*, 6> g_targets{};
std::array<PutDuration, 6> g_originals{};
std::size_t g_hookCount = 0;

struct ReleaseCom {
    void operator()(IUnknown* p) const { if (p) p->Release(); }
};
using ComPtr = std::unique_ptr<IUnknown, ReleaseCom>;

bool IsPreviewCall() {
    void* frames[32];
    const auto count = CaptureStackBackTrace(1, ARRAYSIZE(frames), frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        auto pc = reinterpret_cast<std::uintptr_t>(frames[i]);
        // A captured return address can equal a function's exclusive end.
        if (pc && snap_policy::Contains(g_ranges, pc - 1)) return true;
    }
    return false;
}

template<std::size_t Index>
HRESULT STDMETHODCALLTYPE PutDurationHook(IUnknown* self, TimeSpan value) {
    const auto originalTicks = value.Duration;
    const auto capped = snap_policy::CapDuration(originalTicks, g_maximumMs.load());
    if (capped != originalTicks && IsPreviewCall()) {
        value.Duration = capped;
        const HRESULT result = g_originals[Index](self, value);
        if (SUCCEEDED(result)) {
            g_changed.fetch_add(1);
            if (g_diagnostics.load() && g_logged.fetch_add(1) < 20) {
                Wh_Log(L"Preview duration changed: %lld -> %lld ticks (100 ns)",
                       static_cast<long long>(originalTicks),
                       static_cast<long long>(capped));
            }
        }
        return result;
    }
    return g_originals[Index](self, value);
}

constexpr std::array<PutDuration, 6> kHooks{
    PutDurationHook<0>, PutDurationHook<1>, PutDurationHook<2>,
    PutDurationHook<3>, PutDurationHook<4>, PutDurationHook<5>};

void LoadSettings() {
    g_maximumMs = snap_policy::ClampDurationMs(Wh_GetIntSetting(L"maximumDurationMs"));
    g_diagnostics = Wh_GetIntSetting(L"diagnostics") != 0;
}

void ReleaseModules() {
    for (auto module : g_modules) FreeLibrary(module);
    g_modules.clear();
}

bool RetainAddress(void* address) {
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(address), &module)) return false;
    g_modules.push_back(module);
    return true;
}

void DiscoverCandidates(HMODULE module, PCWSTR moduleName) {
    WH_FIND_SYMBOL symbol{};
    HANDLE search = Wh_FindFirstSymbol(module, nullptr, &symbol);
    if (!search) {
        Wh_Log(L"Symbols unavailable for %s", moduleName);
        return;
    }
    unsigned logged = 0;
    do {
        if (!symbol.symbol || !snap_policy::IsPreviewCandidate(symbol.symbol)) continue;
        const auto address = reinterpret_cast<DWORD64>(symbol.address);
        DWORD64 imageBase = 0;
        auto entry = RtlLookupFunctionEntry(address, &imageBase, nullptr);
        // Never infer a size from the next PDB symbol. Skip data symbols,
        // interior labels and leaf functions with no unwind entry.
        if (!entry || imageBase + entry->BeginAddress != address) continue;
        g_ranges.push_back({static_cast<std::uintptr_t>(address),
            static_cast<std::uintptr_t>(imageBase + entry->EndAddress)});
        if (logged++ < 30) Wh_Log(L"Candidate [%s]: %s", moduleName, symbol.symbol);
    } while (Wh_FindNextSymbol(search, &symbol));
    Wh_FindCloseSymbol(search);
}

bool ProbeDurationImplementations() {
    // Use our own MTA thread. Never change Explorer's UI apartment or retain
    // any WinRT object across apartment boundaries. Only code pointers survive.
    HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(init)) return false;
    bool success = false;
    {
        HSTRING name = nullptr;
        constexpr wchar_t className[] = L"Windows.UI.Composition.Compositor";
        HRESULT hr = WindowsCreateString(className, ARRAYSIZE(className) - 1, &name);
        IInspectable* instanceRaw = nullptr;
        if (SUCCEEDED(hr)) hr = RoActivateInstance(name, &instanceRaw);
        WindowsDeleteString(name);
        ComPtr instance(instanceRaw);
        IUnknown* compositorRaw = nullptr;
        if (SUCCEEDED(hr)) hr = instance->QueryInterface(kCompositor,
            reinterpret_cast<void**>(&compositorRaw));
        ComPtr compositor(compositorRaw);
        if (SUCCEEDED(hr)) {
            success = true;
            auto vtable = *reinterpret_cast<void***>(compositor.get());
            for (const auto slot : kFactorySlots) {
                IUnknown* animationRaw = nullptr;
                hr = reinterpret_cast<CreateAnimation>(vtable[slot])(
                    compositor.get(), &animationRaw);
                ComPtr animation(animationRaw);
                IUnknown* keyframeRaw = nullptr;
                if (SUCCEEDED(hr)) hr = animation->QueryInterface(kKeyFrameAnimation,
                    reinterpret_cast<void**>(&keyframeRaw));
                ComPtr keyframe(keyframeRaw);
                if (FAILED(hr)) { success = false; break; }
                auto target = (*reinterpret_cast<void***>(keyframe.get()))[9];
                auto end = g_targets.begin() + g_hookCount;
                if (std::find(g_targets.begin(), end, target) != end) continue;
                if (!RetainAddress(target)) { success = false; break; }
                g_targets[g_hookCount++] = target;
            }
        }
        if (FAILED(hr)) Wh_Log(L"Compositor probe failed: 0x%08X", static_cast<unsigned>(hr));
    }
    RoUninitialize();
    return success && g_hookCount != 0;
}
} // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    // Inspect loaded modules only. Loading private shell libraries just to
    // probe them can have side effects and cannot prove they own this preview.
    for (auto name : {L"explorer.exe", L"twinui.pcshell.dll", L"SnapLayout.dll"}) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(0, name, &module)) {
            Wh_Log(L"%s is not loaded. Open the snap bar and reload the mod.", name);
            continue;
        }
        g_modules.push_back(module);
        DiscoverCandidates(module, name);
    }
    snap_policy::NormalizeRanges(g_ranges);
    if (g_ranges.empty()) {
        Wh_Log(L"Unsupported/unidentified preview path: no candidate functions. No hooks installed.");
        ReleaseModules();
        return FALSE;
    }
    bool probed = false;
    try {
        std::thread worker([&] { probed = ProbeDurationImplementations(); });
        worker.join();
    } catch (...) {
        Wh_Log(L"Could not start compositor probe thread.");
    }
    if (!probed) {
        ReleaseModules();
        return FALSE;
    }
    for (std::size_t i = 0; i < g_hookCount; ++i) {
        if (!Wh_SetFunctionHook(g_targets[i], reinterpret_cast<void*>(kHooks[i]),
                reinterpret_cast<void**>(&g_originals[i]))) {
            // Windhawk discards pending hooks if initialization returns FALSE.
            Wh_Log(L"Failed to register compositor hook.");
            ReleaseModules();
            return FALSE;
        }
    }
    Wh_Log(L"Hook installed (%zu implementation(s), %zu candidate range(s)). "
           L"Experimental: verify actual preview-duration hits before claiming compatibility.",
           g_hookCount, g_ranges.size());
    return TRUE;
}

void Wh_ModSettingsChanged() { LoadSettings(); }

void Wh_ModUninit() {
    Wh_Log(L"Unloaded. Successful matching duration changes: %u", g_changed.load());
    ReleaseModules();
}
#endif // !SMOOTH_SNAP_POLICY_TEST
