#define SMOOTH_SNAP_POLICY_TEST
#include "../mods/smooth-snap-preview.wh.cpp"
#include <cassert>
#include <climits>
#include <iostream>
#include <limits>

int main() {
    using namespace snap_policy;
    // Millisecond conversion, clamping, invalid input, overflow boundaries.
    assert(CapDuration(2500000, 90) == 900000);
    assert(CapDuration(600000, 90) == 600000);
    assert(CapDuration(0, 90) == 0);
    assert(CapDuration(-1, 90) == -1);
    assert(CapDuration(INT64_MAX, INT_MAX) == 2500000);
    assert(CapDuration(INT64_MAX, INT_MIN) == 400000);
    assert(CapDuration(1, 40) == 1);
    // Property: no positive duration can be extended or disabled by this mod.
    unsigned cases = 0;
    for (int cap : {-100, 0, 40, 90, 250, 10000}) {
        for (std::int64_t duration = 1; duration < 10000000; duration += 7919) {
            auto result = CapDuration(duration, cap);
            assert(result > 0 && result <= duration);
            assert(result <= static_cast<std::int64_t>(ClampDurationMs(cap)) * 10000);
            ++cases;
        }
    }
    // Synthetic symbol names test the classifier, not real Windows compatibility.
    assert(IsPreviewCandidate(L"public: void __cdecl CSnapPreview::Animate(void)"));
    assert(IsPreviewCandidate(L"void winrt::Shell::ZonePreview::Move(int)"));
    assert(!IsPreviewCandidate(L"void Taskbar::Animate(class CSnapPreview *)"));
    assert(!IsPreviewCandidate(L"class CSnapPreview * Other::Get(void)"));
    assert(!IsPreviewCandidate(L"void SnapLayoutPickerPreview::Animate(void)"));
    assert(!IsPreviewCandidate(L"void SnapFlyoutPreview::Animate(void)"));
    assert(!IsPreviewCandidate(L"void SnapBarPreview::Animate(void)"));
    assert(!IsPreviewCandidate(L"void TaskView::Animate(void)"));
    assert(!IsPreviewCandidate(L"CSnapPreview::`vftable'"));
    assert(!IsPreviewCandidate(L""));
    // Boundary checks prevent neighboring, unrelated functions matching.
    std::vector<Range> ranges{{200, 240}, {100, 130}, {100, 120},
        {125, 150}, {150, 160}, {500, 500}, {900, 850}};
    NormalizeRanges(ranges);
    assert(ranges.size() == 2);
    assert(!Contains({}, 100));
    assert(!Contains(ranges, 99));
    assert(Contains(ranges, 100));
    assert(Contains(ranges, 159));
    assert(!Contains(ranges, 160));
    assert(!Contains(ranges, 199));
    assert(Contains(ranges, 200));
    assert(Contains(ranges, 239));
    assert(!Contains(ranges, 240));
    assert(!Contains(ranges, UINTPTR_MAX));
    std::cout << "PASS: duration policy, symbol scope, function ranges, "
              << cases << " duration cases\n";
}
