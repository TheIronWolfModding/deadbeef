# Windows (GTK3) TODO

Caveats for the changes on the `tiw` / `w-*` branches when built for Windows with
MSYS2 (the `windowsbuild.yml` CI target). The GTK3 UI changes themselves are
compiled into `ddb_gui_GTK3` on Windows — these are the gaps that still need work.

## 1. Fix browser links from the MusicBrainz tagger

`plugins/gtkui/mbtagger.c` → `_open_url()` uses `xdg-open` via `system()` on all
non-Apple platforms, including Windows, with POSIX redirection
(`>/dev/null 2>&1 &`). Under `cmd.exe` this fails, so "open in browser" links
from the tagger don't work. Add a `_WIN32` branch using `ShellExecuteW` (or
`cmd /c start ""`).

## 2. PulseAudio preference fields are inert on Windows

The latency and device-filter widgets added to `plugins/gtkui/prefwin/prefwinsound.c`
and `interface.c` are plain GTK, so they compile on Windows. But the Windows CI
does not install `libpulse-simple`, so the `pulse` output plugin is auto-disabled
and those settings have no effect. Either hide that section on Windows or leave
it as-is (harmless but non-functional).

## 3. Output-device selector is Linux-only

`plugins/outdev/outdev.c` is not part of `premake5.lua` (autotools only), and the
`plugins/pulse/pulse.c` changes require the disabled pulse plugin. Neither ships
in the Windows build. If the output-device UI is needed on Windows, add the
plugin to premake and provide a Windows-capable output backend.
