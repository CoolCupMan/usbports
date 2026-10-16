// ACTIVEUSBPORTS.cpp - USB Port Viewer
//
// Lists every port of every USB hub (root hubs and external hubs) and shows
// what is plugged into it: connection status, negotiated speed, port type,
// VID:PID, the product string reported by the device itself and the name
// Windows gives the device. Refreshes automatically when devices are
// plugged in or removed.
//
// Build (MinGW-w64 / MSYS2), see build.bat:
//   windres app.rc -O coff -o app.res
//   g++ ACTIVEUSBPORTS.cpp app.res -std=c++17 -O2 -municode -mwindows -static ^
//       -lsetupapi -lcfgmgr32 -lcomctl32 -o ACTIVEUSBPORTS.exe

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif

#include <windows.h>
#include <initguid.h>   // must come before usbiodef.h so the GUIDs get defined
#include <usbiodef.h>
#include <usbioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <commctrl.h>
#include <dbt.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

enum class PortState { Empty, Connected, Disabled, Problem, Busy };

struct PortInfo {
    std::wstring hubKey;      // hub device path, stable across refreshes
    std::wstring hubLabel;
    ULONG port = 0;
    PortState state = PortState::Empty;
    std::wstring status;
    std::wstring speed;
    std::wstring portType;
    std::wstring vidPid;
    std::wstring product;     // string descriptor read from the device
    std::wstring deviceName;  // name Windows shows in Device Manager

    std::wstring Key() const { return hubKey + L"#" + std::to_wstring(port); }

    bool operator==(const PortInfo& o) const {
        return hubKey == o.hubKey && hubLabel == o.hubLabel && port == o.port &&
               state == o.state && status == o.status && speed == o.speed &&
               portType == o.portType && vidPid == o.vidPid &&
               product == o.product && deviceName == o.deviceName;
    }
    bool operator!=(const PortInfo& o) const { return !(*this == o); }
};

struct DevNodeInfo {
    std::wstring name;
    bool disabled = false;
    ULONG problem = 0;        // CM_PROB_* code, 0 = no problem
};

struct ScanResult {
    std::vector<PortInfo> ports;
    int hubCount = 0;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::wstring ToUpper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::wstring GetRegProp(HDEVINFO devs, SP_DEVINFO_DATA* dev, DWORD prop) {
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(devs, dev, prop, nullptr, nullptr, 0, &size);
    if (size == 0) return L"";
    std::vector<BYTE> buf(size + sizeof(WCHAR), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(devs, dev, prop, nullptr, buf.data(), size, nullptr))
        return L"";
    return reinterpret_cast<const wchar_t*>(buf.data());
}

static std::wstring GetDeviceName(HDEVINFO devs, SP_DEVINFO_DATA* dev) {
    std::wstring name = GetRegProp(devs, dev, SPDRP_FRIENDLYNAME);
    if (name.empty()) name = GetRegProp(devs, dev, SPDRP_DEVICEDESC);
    return name;
}

// Maps driver key names ("{36fc9e60-...}\0005") of all USB devices to their
// Device Manager name and state. The hub IOCTLs give us the driver key of the
// device on each port, which is how we find the device's real name.
static std::map<std::wstring, DevNodeInfo> BuildDriverKeyMap() {
    std::map<std::wstring, DevNodeInfo> map;
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return map;

    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); i++) {
        std::wstring driverKey = GetRegProp(devs, &dev, SPDRP_DRIVER);
        if (driverKey.empty()) continue;

        DevNodeInfo info;
        info.name = GetDeviceName(devs, &dev);
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) == CR_SUCCESS &&
            (status & DN_HAS_PROBLEM)) {
            info.problem = problem;
            info.disabled = (problem == CM_PROB_DISABLED);
        }
        map[ToUpper(driverKey)] = info;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return map;
}

// ---------------------------------------------------------------------------
// Hub IOCTL wrappers
// ---------------------------------------------------------------------------

static std::wstring GetDriverKeyName(HANDLE hub, ULONG port) {
    USB_NODE_CONNECTION_DRIVERKEY_NAME probe = {};
    probe.ConnectionIndex = port;
    DWORD got = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,
                         &probe, sizeof(probe), &probe, sizeof(probe), &got, nullptr))
        return L"";
    if (probe.ActualLength <= sizeof(probe)) return L"";

    std::vector<BYTE> buf(probe.ActualLength, 0);
    auto name = reinterpret_cast<PUSB_NODE_CONNECTION_DRIVERKEY_NAME>(buf.data());
    name->ConnectionIndex = port;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,
                         name, (DWORD)buf.size(), name, (DWORD)buf.size(), &got, nullptr))
        return L"";

    size_t maxChars = (buf.size() - offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName)) / sizeof(WCHAR);
    return std::wstring(name->DriverKeyName, wcsnlen(name->DriverKeyName, maxChars));
}

// Reads a raw string descriptor from the device on the given port.
// Returns the descriptor bytes (bLength, bDescriptorType, payload...) or empty.
static std::vector<BYTE> GetRawStringDescriptor(HANDLE hub, ULONG port, UCHAR index, USHORT langId) {
    const DWORD maxLen = 255;  // bLength is a single byte
    std::vector<BYTE> buf(sizeof(USB_DESCRIPTOR_REQUEST) + maxLen, 0);
    auto req = reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(buf.data());
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = 0x80;  // device-to-host, standard, device
    req->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    req->SetupPacket.wValue = (USHORT)((USB_STRING_DESCRIPTOR_TYPE << 8) | index);
    req->SetupPacket.wIndex = langId;
    req->SetupPacket.wLength = (USHORT)maxLen;

    DWORD got = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                         buf.data(), (DWORD)buf.size(), buf.data(), (DWORD)buf.size(), &got, nullptr))
        return {};
    if (got < sizeof(USB_DESCRIPTOR_REQUEST) + 2) return {};

    const BYTE* desc = buf.data() + sizeof(USB_DESCRIPTOR_REQUEST);
    DWORD dataLen = got - sizeof(USB_DESCRIPTOR_REQUEST);
    BYTE bLength = desc[0];
    if (desc[1] != USB_STRING_DESCRIPTOR_TYPE || bLength < 2 || bLength > dataLen || (bLength & 1))
        return {};
    return std::vector<BYTE>(desc, desc + bLength);
}

static std::wstring GetStringDescriptor(HANDLE hub, ULONG port, UCHAR index) {
    if (index == 0) return L"";

    // String descriptor 0 lists the languages the device supports.
    USHORT langId = 0x0409;  // English (US) fallback
    std::vector<BYTE> langs = GetRawStringDescriptor(hub, port, 0, 0);
    if (langs.size() >= 4) langId = (USHORT)(langs[2] | (langs[3] << 8));

    std::vector<BYTE> desc = GetRawStringDescriptor(hub, port, index, langId);
    if (desc.size() < 2) return L"";

    std::wstring s;
    for (size_t i = 2; i + 1 < desc.size(); i += 2) {
        wchar_t ch = (wchar_t)(desc[i] | (desc[i + 1] << 8));
        if (ch == 0) break;
        s += ch;
    }
    return Trim(s);
}

static std::wstring ConnectionStatusText(USB_CONNECTION_STATUS st) {
    switch (st) {
    case NoDeviceConnected:        return L"Empty";
    case DeviceConnected:          return L"Connected";
    case DeviceFailedEnumeration:  return L"Error: enumeration failed";
    case DeviceGeneralFailure:     return L"Error: general failure";
    case DeviceCausedOvercurrent:  return L"Error: overcurrent";
    case DeviceNotEnoughPower:     return L"Error: not enough power";
    case DeviceNotEnoughBandwidth: return L"Error: not enough bandwidth";
    case DeviceHubNestedTooDeeply: return L"Error: hubs nested too deeply";
    case DeviceInLegacyHub:        return L"Error: in legacy hub";
    case DeviceEnumerating:        return L"Enumerating...";
    case DeviceReset:              return L"Resetting...";
    default:                       return L"Unknown status (" + std::to_wstring((int)st) + L")";
    }
}

static std::wstring SpeedText(UCHAR speed) {
    switch (speed) {
    case UsbLowSpeed:   return L"Low (1.5 Mbps)";
    case UsbFullSpeed:  return L"Full (12 Mbps)";
    case UsbHighSpeed:  return L"High (480 Mbps)";
    case UsbSuperSpeed: return L"SuperSpeed (5 Gbps)";
    default:            return L"Unknown";
    }
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

static void ScanPort(HANDLE hub, ULONG port, const std::map<std::wstring, DevNodeInfo>& devices, PortInfo& pi) {
    // Port capabilities (Windows 8+): which USB versions the port supports and
    // whether the device is actually running at SuperSpeed.
    USB_NODE_CONNECTION_INFORMATION_EX_V2 v2 = {};
    v2.ConnectionIndex = port;
    v2.Length = sizeof(v2);
    v2.SupportedUsbProtocols.Usb110 = 1;
    v2.SupportedUsbProtocols.Usb200 = 1;
    v2.SupportedUsbProtocols.Usb300 = 1;
    DWORD got = 0;
    bool haveV2 = DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2,
                                  &v2, sizeof(v2), &v2, sizeof(v2), &got, nullptr) != FALSE;
    if (haveV2) {
        if (v2.SupportedUsbProtocols.Usb300)      pi.portType = L"USB 3.x";
        else if (v2.SupportedUsbProtocols.Usb200) pi.portType = L"USB 2.0";
        else if (v2.SupportedUsbProtocols.Usb110) pi.portType = L"USB 1.1";
    }

    // Is the port wired to an external connector, or internal (webcam, BT, ...)?
    BYTE connBuf[sizeof(USB_PORT_CONNECTOR_PROPERTIES) + 512] = {};
    auto connProps = reinterpret_cast<PUSB_PORT_CONNECTOR_PROPERTIES>(connBuf);
    connProps->ConnectionIndex = port;
    if (DeviceIoControl(hub, IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES,
                        connBuf, sizeof(connBuf), connBuf, sizeof(connBuf), &got, nullptr) &&
        !connProps->UsbPortProperties.PortIsUserConnectable) {
        pi.portType += pi.portType.empty() ? L"internal" : L" (internal)";
    }

    USB_NODE_CONNECTION_INFORMATION_EX conn = {};
    conn.ConnectionIndex = port;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                         &conn, sizeof(conn), &conn, sizeof(conn), &got, nullptr)) {
        pi.state = PortState::Problem;
        pi.status = L"Query failed (error " + std::to_wstring(GetLastError()) + L")";
        return;
    }

    pi.status = ConnectionStatusText(conn.ConnectionStatus);
    switch (conn.ConnectionStatus) {
    case NoDeviceConnected: pi.state = PortState::Empty;     return;
    case DeviceConnected:   pi.state = PortState::Connected; break;
    case DeviceEnumerating:
    case DeviceReset:       pi.state = PortState::Busy;      return;
    default:                pi.state = PortState::Problem;   return;
    }

    pi.speed = SpeedText(conn.Speed);
    if (haveV2) {
        if (v2.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher)
            pi.speed = L"SuperSpeed+ (10+ Gbps)";
        else if (v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher)
            pi.speed = L"SuperSpeed (5 Gbps)";
    }

    wchar_t ids[32];
    swprintf(ids, 32, L"%04X:%04X", conn.DeviceDescriptor.idVendor, conn.DeviceDescriptor.idProduct);
    pi.vidPid = ids;

    pi.product = GetStringDescriptor(hub, port, conn.DeviceDescriptor.iProduct);
    if (conn.DeviceIsHub && pi.product.empty()) pi.product = L"(USB hub)";

    std::wstring driverKey = GetDriverKeyName(hub, port);
    auto it = devices.find(ToUpper(driverKey));
    if (driverKey.empty() || it == devices.end()) {
        pi.deviceName = L"(no driver installed)";
        return;
    }
    pi.deviceName = it->second.name;
    if (it->second.disabled) {
        pi.state = PortState::Disabled;
        pi.status = L"Connected, disabled";
    } else if (it->second.problem) {
        pi.state = PortState::Problem;
        pi.status = L"Connected, driver problem (code " + std::to_wstring(it->second.problem) + L")";
    }
}

static ScanResult ScanUsbPorts() {
    ScanResult result;
    std::map<std::wstring, DevNodeInfo> devices = BuildDriverKeyMap();

    const GUID hubGuid = GUID_DEVINTERFACE_USB_HUB;
    HDEVINFO hubs = SetupDiGetClassDevsW(&hubGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hubs == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hubs, nullptr, &hubGuid, idx, &ifData); idx++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(hubs, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        std::vector<BYTE> detailBuf(needed, 0);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA hubDev = {};
        hubDev.cbSize = sizeof(hubDev);
        if (!SetupDiGetDeviceInterfaceDetailW(hubs, &ifData, detail, needed, nullptr, &hubDev)) continue;

        HANDLE hub = CreateFileW(detail->DevicePath, GENERIC_WRITE, FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (hub == INVALID_HANDLE_VALUE) continue;

        USB_NODE_INFORMATION nodeInfo = {};
        nodeInfo.NodeType = UsbHub;
        DWORD got = 0;
        if (DeviceIoControl(hub, IOCTL_USB_GET_NODE_INFORMATION,
                            &nodeInfo, sizeof(nodeInfo), &nodeInfo, sizeof(nodeInfo), &got, nullptr)) {
            result.hubCount++;
            std::wstring hubName = GetDeviceName(hubs, &hubDev);
            if (hubName.empty()) hubName = L"USB Hub";
            std::wstring hubLabel = L"#" + std::to_wstring(result.hubCount) + L"  " + hubName;

            ULONG portCount = nodeInfo.u.HubInformation.HubDescriptor.bNumberOfPorts;
            for (ULONG port = 1; port <= portCount; port++) {
                PortInfo pi;
                pi.hubKey = detail->DevicePath;
                pi.hubLabel = hubLabel;
                pi.port = port;
                ScanPort(hub, port, devices, pi);
                result.ports.push_back(pi);
            }
        }
        CloseHandle(hub);
    }

    SetupDiDestroyDeviceInfoList(hubs);
    return result;
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------

enum : UINT {
    IDC_REFRESH = 101,
    IDC_SHOWEMPTY,
    IDC_COPY,
    IDC_LIST,
    IDC_STATUS,
};
static const UINT_PTR TIMER_DEVCHANGE = 1;  // debounces WM_DEVICECHANGE bursts
static const UINT DEVCHANGE_DELAY_MS = 700;

static HWND g_hwnd, g_list, g_status, g_btnRefresh, g_chkEmpty, g_btnCopy;
static HFONT g_font;
static int g_dpi = 96;
static ScanResult g_scan;
static std::vector<size_t> g_visible;  // list row -> index into g_scan.ports

static int S(int px) { return MulDiv(px, g_dpi, 96); }

struct Column { const wchar_t* title; int width; int fmt; };
static const Column kColumns[] = {
    { L"Hub",                 210, LVCFMT_LEFT  },
    { L"Port",                 45, LVCFMT_RIGHT },
    { L"Status",              170, LVCFMT_LEFT  },
    { L"Speed",               140, LVCFMT_LEFT  },
    { L"Port type",           105, LVCFMT_LEFT  },
    { L"VID:PID",              80, LVCFMT_LEFT  },
    { L"Product (from device)", 200, LVCFMT_LEFT  },
    { L"Windows device name", 230, LVCFMT_LEFT  },
};
static const int kColumnCount = sizeof(kColumns) / sizeof(kColumns[0]);

static std::wstring CellText(const PortInfo& p, int col) {
    switch (col) {
    case 0: return p.hubLabel;
    case 1: return std::to_wstring(p.port);
    case 2: return p.status;
    case 3: return p.speed;
    case 4: return p.portType;
    case 5: return p.vidPid;
    case 6: return p.product;
    case 7: return p.deviceName;
    default: return L"";
    }
}

static COLORREF StateColor(PortState s) {
    switch (s) {
    case PortState::Connected: return RGB(0, 130, 0);
    case PortState::Disabled:  return RGB(200, 0, 0);
    case PortState::Problem:   return RGB(200, 0, 0);
    case PortState::Busy:      return RGB(200, 120, 0);
    default:                   return RGB(140, 140, 140);
    }
}

static bool ShowEmptyPorts() {
    return SendMessageW(g_chkEmpty, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void UpdateStatusBar() {
    int connected = 0, problems = 0;
    for (const PortInfo& p : g_scan.ports) {
        if (p.state == PortState::Connected) connected++;
        if (p.state == PortState::Problem || p.state == PortState::Disabled) problems++;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t text[256];
    if (g_scan.hubCount == 0) {
        swprintf(text, 256, L"  No USB hubs could be opened. Try running as administrator.");
    } else {
        swprintf(text, 256, L"  %d hubs  \x00B7  %d ports  \x00B7  %d devices connected  \x00B7  %d with problems  \x00B7  updated %02d:%02d:%02d",
                 g_scan.hubCount, (int)g_scan.ports.size(), connected, problems,
                 t.wHour, t.wMinute, t.wSecond);
    }
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)text);
}

// Rebuilds the list from g_scan, keeping selection and scroll position.
static void FillList() {
    std::set<std::wstring> selected;
    for (int i = -1; (i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) != -1;)
        if ((size_t)i < g_visible.size()) selected.insert(g_scan.ports[g_visible[i]].Key());
    int topIndex = ListView_GetTopIndex(g_list);

    bool showEmpty = ShowEmptyPorts();
    g_visible.clear();
    for (size_t i = 0; i < g_scan.ports.size(); i++)
        if (showEmpty || g_scan.ports[i].state != PortState::Empty) g_visible.push_back(i);

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (size_t row = 0; row < g_visible.size(); row++) {
        const PortInfo& p = g_scan.ports[g_visible[row]];
        std::wstring first = CellText(p, 0);
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)row;
        item.pszText = const_cast<wchar_t*>(first.c_str());
        item.lParam = (LPARAM)row;
        int idx = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        for (int col = 1; col < kColumnCount; col++) {
            std::wstring text = CellText(p, col);
            ListView_SetItemText(g_list, idx, col, const_cast<wchar_t*>(text.c_str()));
        }
        if (selected.count(p.Key()))
            ListView_SetItemState(g_list, idx, LVIS_SELECTED, LVIS_SELECTED);
    }

    if (topIndex > 0 && !g_visible.empty()) {
        RECT rc = {};
        if (ListView_GetItemRect(g_list, 0, &rc, LVIR_BOUNDS))
            ListView_Scroll(g_list, 0, topIndex * (rc.bottom - rc.top));
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
}

static void Refresh(bool force) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    ScanResult scan = ScanUsbPorts();
    SetCursor(old);

    bool changed = force || scan.hubCount != g_scan.hubCount || scan.ports != g_scan.ports;
    g_scan = std::move(scan);
    if (changed) FillList();
    UpdateStatusBar();
}

// Copies the selected rows (or all visible rows) as tab-separated text.
static void CopyToClipboard() {
    std::vector<int> rows;
    for (int i = -1; (i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) != -1;) rows.push_back(i);
    if (rows.empty())
        for (size_t i = 0; i < g_visible.size(); i++) rows.push_back((int)i);

    std::wstring text;
    for (int col = 0; col < kColumnCount; col++)
        text += std::wstring(kColumns[col].title) + (col + 1 < kColumnCount ? L"\t" : L"\r\n");
    for (int row : rows) {
        if ((size_t)row >= g_visible.size()) continue;
        const PortInfo& p = g_scan.ports[g_visible[row]];
        for (int col = 0; col < kColumnCount; col++)
            text += CellText(p, col) + (col + 1 < kColumnCount ? L"\t" : L"\r\n");
    }

    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();

    wchar_t msg[128];
    swprintf(msg, 128, L"  Copied %d row(s) to the clipboard.", (int)rows.size());
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)msg);
}

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(g_status, &sb);
    int statusH = sb.bottom - sb.top;

    int m = S(8), btnH = S(26);
    int x = m;
    MoveWindow(g_btnRefresh, x, m, S(100), btnH, TRUE); x += S(100) + m;
    MoveWindow(g_btnCopy,    x, m, S(100), btnH, TRUE); x += S(100) + m * 2;
    MoveWindow(g_chkEmpty,   x, m, S(160), btnH, TRUE);

    int top = m + btnH + m;
    MoveWindow(g_list, m, top, rc.right - 2 * m, std::max(0, (int)rc.bottom - top - statusH - m), TRUE);
}

static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, UINT id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                             parent, (HMENU)(UINT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void CreateControls(HWND hwnd) {
    g_btnRefresh = MakeChild(hwnd, L"BUTTON", L"&Refresh (F5)", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    g_btnCopy    = MakeChild(hwnd, L"BUTTON", L"&Copy", BS_PUSHBUTTON | WS_TABSTOP, IDC_COPY);
    g_chkEmpty   = MakeChild(hwnd, L"BUTTON", L"Show &empty ports", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SHOWEMPTY);
    SendMessageW(g_chkEmpty, BM_SETCHECK, BST_CHECKED, 0);

    g_list = MakeChild(hwnd, WC_LISTVIEWW, L"",
                       WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, IDC_LIST);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    for (int i = 0; i < kColumnCount; i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t*>(kColumns[i].title);
        col.cx = S(kColumns[i].width);
        col.fmt = kColumns[i].fmt;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }

    g_status = MakeChild(hwnd, STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, IDC_STATUS);
}

static LRESULT OnListCustomDraw(LPNMLVCUSTOMDRAW cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        size_t row = (size_t)cd->nmcd.lItemlParam;
        if (row < g_visible.size()) cd->clrText = StateColor(g_scan.ports[g_visible[row]].state);
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateControls(hwnd);

        // WM_DEVICECHANGE / DBT_DEVNODES_CHANGED is broadcast to top-level
        // windows anyway; registering for USB device interfaces as well makes
        // arrival/removal notifications reliable.
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
        RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        return 0;
    }

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = S(480);
        mmi->ptMinTrackSize.y = S(240);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH:  Refresh(true);     return 0;
        case IDC_COPY:     CopyToClipboard(); return 0;
        case IDC_SHOWEMPTY:
            FillList();
            UpdateStatusBar();
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto hdr = reinterpret_cast<LPNMHDR>(lp);
        if (hdr->hwndFrom == g_list && hdr->code == NM_CUSTOMDRAW)
            return OnListCustomDraw(reinterpret_cast<LPNMLVCUSTOMDRAW>(lp));
        break;
    }

    case WM_DEVICECHANGE:
        // Devices arrive in bursts (hub + interfaces + drivers); wait until it settles.
        SetTimer(hwnd, TIMER_DEVCHANGE, DEVCHANGE_DELAY_MS, nullptr);
        return TRUE;

    case WM_TIMER:
        if (wp == TIMER_DEVCHANGE) {
            KillTimer(hwnd, TIMER_DEVCHANGE);
            Refresh(false);
        }
        return 0;

    case WM_SETFOCUS:
        SetFocus(g_list);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"UsbPortViewerClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"RegisterClassEx failed", L"USB Port Viewer", MB_ICONERROR);
        return 1;
    }

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"USB Port Viewer", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1100), S(560),
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"CreateWindowEx failed", L"USB Port Viewer", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    Refresh(true);

    ACCEL accels[] = {
        { FVIRTKEY,             VK_F5, IDC_REFRESH },
        { FVIRTKEY | FCONTROL,  'C',   IDC_COPY    },
    };
    HACCEL accel = CreateAcceleratorTableW(accels, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(g_hwnd, accel, &msg)) continue;
        if (IsDialogMessageW(g_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyAcceleratorTable(accel);
    DeleteObject(g_font);
    return (int)msg.wParam;
}
