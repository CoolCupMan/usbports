#define _UNICODE
#include <initguid.h>
#include <devguid.h>
#include <windows.h>
#include <usbioctl.h>
#include <setupapi.h>
#include <commctrl.h>
#include <vector>
#include <sstream>
#include <string>

struct UsbPortInfo {
    int hubId;
    ULONG portIndex;
    bool devicePresent;
    std::wstring deviceName;
};

std::vector<UsbPortInfo> EnumerateUsbPorts();
void PopulateTreeView(HWND hTreeView);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
std::wstring GetDeviceFriendlyName(HDEVINFO devInfo, SP_DEVINFO_DATA& devData);

static UINT_PTR g_timerId = 300;
static HWND g_hWndMain = nullptr;
static HWND g_hTreeView = nullptr;
static std::vector<UsbPortInfo> g_ports;

int main_entry() {
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandle(NULL);

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"UsbPortViewerClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    g_hWndMain = CreateWindowEx(
        0, wc.lpszClassName, L"USB Port Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 500,
        NULL, NULL, hInst, NULL
    );

    if (!g_hWndMain) return 1;

    // CREATE TREEVIEW
    g_hTreeView = CreateWindowEx(
        0,
        WC_TREEVIEW,
        L"",
        WS_VISIBLE | WS_CHILD | WS_BORDER |
        TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
        10, 10, 660, 430,
        g_hWndMain,
        NULL,
        hInst,
        NULL
    );

    ShowWindow(g_hWndMain, SW_SHOW);
    UpdateWindow(g_hWndMain);

    // Start Timer
    SetTimer(g_hWndMain, g_timerId, 4000, NULL);

    // Initial population
    g_ports = EnumerateUsbPorts();
    if (g_hTreeView) {
        PopulateTreeView(g_hTreeView);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nCmdShow) {
    return main_entry();
}

std::wstring GetDeviceFriendlyName(HDEVINFO devInfo, SP_DEVINFO_DATA& devData) {
    WCHAR friendlyName[256] = { 0 };

    // Try friendly name property first
    if (SetupDiGetDeviceRegistryProperty(devInfo, &devData, SPDRP_FRIENDLYNAME,
        NULL, (PBYTE)friendlyName, sizeof(friendlyName), NULL)) {
        return friendlyName;
    }

    // Fall back to device description if no friendly name
    if (SetupDiGetDeviceRegistryProperty(devInfo, &devData, SPDRP_DEVICEDESC,
        NULL, (PBYTE)friendlyName, sizeof(friendlyName), NULL)) {
        return friendlyName;
    }

    return L"Unknown Device";
}

std::vector<UsbPortInfo> EnumerateUsbPorts() {
    std::vector<UsbPortInfo> list;
    GUID guidUsbHub = GUID_DEVINTERFACE_USB_HUB;
    HDEVINFO devInfo = SetupDiGetClassDevs(&guidUsbHub, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return list;

    for (DWORD idx = 0;; idx++) {
        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, &guidUsbHub, idx, &ifData)) break;

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        std::vector<BYTE> buf(needed);
        auto pDetail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(&buf[0]);
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        SP_DEVINFO_DATA devData = {};
        devData.cbSize = sizeof(devData);

        if (!SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, pDetail, needed, NULL, &devData)) continue;

        HANDLE hHub = CreateFile(
            pDetail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (hHub == INVALID_HANDLE_VALUE) continue;

        USB_NODE_INFORMATION hubInfo = {};
        hubInfo.NodeType = UsbHub;
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_INFORMATION,
            &hubInfo, sizeof(hubInfo),
            &hubInfo, sizeof(hubInfo),
            &bytesReturned, NULL)) {
            CloseHandle(hHub);
            continue;
        }

        ULONG portCount = hubInfo.u.HubInformation.HubDescriptor.bNumberOfPorts;
        for (ULONG port = 1; port <= portCount; port++) {
            USB_NODE_CONNECTION_INFORMATION_EX conn = {};
            conn.ConnectionIndex = port;
            DWORD outBytes = 0;
            BOOL ok2 = DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                &conn, sizeof(conn),
                &conn, sizeof(conn), &outBytes, NULL);

            UsbPortInfo pi;
            pi.hubId = (int)idx;
            pi.portIndex = port;
            pi.devicePresent = false;
            pi.deviceName.clear();

            if (ok2 && conn.ConnectionStatus == DeviceConnected) {
                pi.devicePresent = true;

                // Get device friendly name
                std::wstring friendlyName = GetDeviceFriendlyName(devInfo, devData);

                std::wstringstream wss;
                wss << friendlyName << L" (VendorID: " << conn.DeviceDescriptor.idVendor
                    << L", ProductID: " << conn.DeviceDescriptor.idProduct << L")";

                pi.deviceName = wss.str();
            }
            list.push_back(pi);
        }

        CloseHandle(hHub);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return list;
}

void PopulateTreeView(HWND hTreeView) {
    TreeView_DeleteAllItems(hTreeView);

    for (const auto& port : g_ports) {
        TVINSERTSTRUCT tvis = {};
        tvis.hParent = TVI_ROOT;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT;
        std::wstringstream wss;
        wss << L"Hub " << port.hubId << L", Port " << port.portIndex;
        std::wstring hubPortLabel = wss.str();
        tvis.item.pszText = const_cast<LPWSTR>(hubPortLabel.c_str());
        HTREEITEM hItem = TreeView_InsertItem(hTreeView, &tvis);

        if (port.devicePresent) {
            TVINSERTSTRUCT child = {};
            child.hParent = hItem;
            child.hInsertAfter = TVI_LAST;
            child.item.mask = TVIF_TEXT;
            child.item.pszText = const_cast<LPWSTR>(port.deviceName.c_str());
            TreeView_InsertItem(hTreeView, &child);
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == g_timerId) {
            g_ports = EnumerateUsbPorts();
            if (g_hTreeView) {
                PopulateTreeView(g_hTreeView);
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}
