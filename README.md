# USB Port Viewer

A small Win32 tool that lists every port on every USB hub in the PC
(root hubs and external hubs) and shows what is plugged into each one.

| Column | Meaning |
|---|---|
| Hub | Hub number and its Windows name, e.g. `#2  USB Root Hub (USB 3.0)` |
| Port | Port number on that hub |
| Status | Empty, Connected, Connected but disabled, driver problem, or a port error (overcurrent, not enough power, ...) |
| Speed | Speed the device is running at: Low / Full / High / SuperSpeed / SuperSpeed+ |
| Port type | What the port supports (USB 2.0 / 3.x), and `internal` for ports with no external connector (built-in webcam, Bluetooth, ...) |
| VID:PID | Vendor and product ID in hex |
| Product (from device) | Product name the device reports about itself |
| Windows device name | Name shown in Device Manager |

Rows are colour-coded: **green** connected, **red** disabled or error,
**orange** enumerating/resetting, **grey** empty.

* Refreshes on its own when a device is plugged in or removed (no polling).
* **F5** / *Refresh* rescans, **Ctrl+C** / *Copy* copies the selected rows
  (or all rows) as tab-separated text, ready to paste into Excel.
* *Show empty ports* hides unused ports.

Note: a USB 3 port shows up twice on a USB 3 root hub, once as a USB 3.x port
and once as its USB 2.0 companion. Which one a device appears on depends on
the speed it connects at.

## Building

With MinGW-w64 (e.g. MSYS2), run `build.bat`, or:

```sh
windres app.rc -O coff -o app.res
g++ ACTIVEUSBPORTS.cpp app.res -std=c++17 -O2 -municode -mwindows -static -lsetupapi -lcfgmgr32 -lcomctl32 -o ACTIVEUSBPORTS.exe
```

`app.res` embeds a manifest that gives the window modern-looking controls
and sharp text on high-DPI screens. It's optional: leave `app.res` out of
the `g++` line and it still builds and works, just with older-style controls.
`-static` makes the `.exe` run without MinGW DLLs next to it.

`legacy/` holds the earlier experiments this version was built from.
