# Smooth Snap Preview for Windhawk

**Experimental prototype. No Windows build has been runtime-verified.** This is
source code for testing, not a confirmed fix for Windows Snap Layout lag.

The recorded issue is a preview that falls behind when targets change during an
animation. Version 0.1.0 only caps durations; it does **not** implement immediate
retargeting. Broad Windows 11 compatibility is the goal, not an established
property of this prototype.

The goal is to make the large translucent placement preview respond faster when
you drag a window between snap targets. The mod caps identified native compositor
animations at 90 ms, keeping their native easing and rendering. It does not alter
the layout choices or window placement logic.

The preview-function identification is a symbol-name heuristic. It may find no
matching functions, or the preview on your Windows build may use a different
animation API. Successful loading alone does not establish that the mod works.

## Try the source

Requires Windows 11 x64 and Windhawk 1.4 or later.

1. Open the Windows snap bar once so its shell modules are loaded.
2. In Windhawk, choose **Create a new mod** from the main menu.
3. Replace the template with the entire contents of
   [smooth-snap-preview.wh.cpp](mods/smooth-snap-preview.wh.cpp).
4. Compile and enable the mod in Windhawk's editor.
5. Start with the default **Maximum preview animation duration: 90 ms**.
   Try 60 ms for a shorter transition, or 120 ms for a longer one.

Disable the mod to return to native behavior for newly created animations.
There is no prebuilt DLL or Windhawk catalog release in this repository.

## Verify on Windows

To identify the preview's owning process and window classes on any Windows 11
test machine, run the read-only diagnostic from a PowerShell terminal in this
repository:

```powershell
.\scripts\Collect-SnapDiagnostics.ps1
```

For 20 seconds, drag a window between the snap targets as in the recording. The
script writes `snap-diagnostics.json` in the current directory, recording the OS
build, selected system DLL versions, and visible shell window classes/rectangles.
It changes no system settings and collects no window titles, screenshots,
keystrokes, or browser URLs. It refuses to overwrite an existing report; move the
old report or specify another `-OutputPath` for subsequent runs. Respect your
machine's script-execution policy rather than disabling it to run this script.

This is diagnostic tooling, not the fix. Its PowerShell syntax and embedded C#
compilation can be checked on Linux; Windows API execution still requires a
Windows test machine. The snapshots are sampled at approximately 10 Hz, so they
identify candidate shell windows, not exact animation timings. Some compositor
visuals do not have their own window and will require further investigation.

Enable Windhawk debug logging and the mod's **Diagnostics** setting. Drag a
window rapidly between the snap bar's zones. Look for `Preview duration changed`
messages and compare with the mod disabled. A `Hook installed` message alone
does not demonstrate that the placement preview was changed.

Check that the preview follows each target, release snaps the window correctly,
Escape cancels, and taskbar/flyout animations remain unchanged. Test multiple
monitors and mixed display scaling if applicable. Record the Version and OS Build
from `winver`, the relevant log, and your observations. If no candidates are
found, initialization fails without installing animation hooks.

## Validation performed

- Windows x64 source compilation passed using LLVM-MinGW and upstream Windhawk
  API headers during development. This was an object compilation, not a
  Windows runtime or Windhawk editor test.
- Portable tests cover duration limits, symbol-scope filtering, and function
  range boundaries. They use synthetic symbol names, not verified Windows
  preview symbols.
- Windows behavior, frame pacing, and perceived smoothness remain untested.

Run the portable checks on Linux with GCC:

```sh
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -g tests/policy_tests.cpp -o build/policy-tests
./build/policy-tests
```

See [architecture notes](docs/architecture.md) for the implementation and limits.
