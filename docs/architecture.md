# Implementation and limitations

The mod runs in `explorer.exe`. At initialization it enumerates symbols in the
loaded Explorer, `twinui.pcshell.dll`, and `SnapLayout.dll` modules. It selects
declaring scopes containing both `snap` and `preview`, or `zonepreview`, excluding
picker, flyout, and snap-bar scopes. **These are candidate names, not confirmed
preview entry points.** No private function signature is called or hooked.

An x64 unwind-table entry must begin at the symbol address before its function
range is accepted. Ranges are normalized once. No symbols are downloaded or
resolved in the duration hook itself.

A temporary MTA thread creates a Windows.UI.Composition.Compositor and probes
six keyframe animation types. It queries the public IKeyFrameAnimation interface
and hooks its put_Duration implementation, deduplicating shared implementations.
Only code addresses survive the thread; COM objects are released in that thread.

The public ABI is defined in Microsoft's
[Windows.UI.Composition header](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.ui.composition.h).
IInspectable occupies vtable slots 0–5. ICompositor animation factory slots are
6 (color), 19 (quaternion), 20 (scalar), and 26–28 (vector2–4).
IKeyFrameAnimation put_Duration is slot 9. TimeSpan contains one signed 64-bit
count of 100 ns units. The mod is restricted to x64.

When a requested duration exceeds the configured cap, the hook captures up to
32 stack frames. It changes the duration only if a return address belongs to a
candidate function range. It preserves shorter, zero, and negative durations,
and leaves input validation and HRESULT results to Windows. The setting is
clamped to 40–250 ms so the mod does not request zero-duration animations.

This approach does not implement a new retargeting engine, interrupt queued
animations, fix compositor stalls, or increase display refresh rate. It only
shortens qualifying keyframe animations. Inlining, asynchronous construction,
lazy module loading, different symbol names, or a different animation API can
make it ineffective. A matching stack is still only a heuristic for preview
ownership. Windows-side observation is required before compatibility claims.

The single source file includes its portable policy so it can be pasted directly
into Windhawk. Tests compile that same policy with the Windows adapter excluded.
