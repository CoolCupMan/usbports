#define _UNICODE
// Do NOT re‑define UNICODE if it's already defined by compiler flags.

// Define GUIDs
#define INITGUID
#include <initguid.h>
#include <devguid.h>

#include <windows.h>
#include <usbioctl.h>
#include <setupapi.h>
#include <vector>
#include <sstream>

// Simple struct
struct UsbPortInfo {
    int hubId;
    ULONG portIndex;
    bool devicePresent;
    bool deviceEnabled;  // Added to track if device is enabled
};

std::vector<UsbPortInfo> EnumerateUsbPorts();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void DoRefresh(HWND);

std::vector<UsbPortInfo> g_ports;
UINT_PTR g_timerId = 300;
HWND g_hWndMain = nullptr;

int main_entry() {
    HINSTANCE hInst = GetModuleHandle(NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"UsbPortViewerClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) {
        MessageBox(NULL, L"RegisterClass failed", L"Error", MB_ICONERROR);
        return 1;
    }

    g_hWndMain = CreateWindowEx(
        0, wc.lpszClassName, L"USB Port Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 400,
        NULL, NULL, hInst, NULL
    );
    if (!g_hWndMain) {
        MessageBox(NULL, L"CreateWindow failed", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hWndMain, SW_SHOW);
    UpdateWindow(g_hWndMain);
    SetTimer(g_hWndMain, g_timerId, 4000, NULL);

    g_ports = EnumerateUsbPorts();

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

std::vector<UsbPortInfo> EnumerateUsbPorts() {
    std::vector<UsbPortInfo> list;
    GUID guidUsbHub = GUID_DEVINTERFACE_USB_HUB;
    HDEVINFO devInfo = SetupDiGetClassDevs(&guidUsbHub, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return list;

    for (DWORD idx = 0;; idx++) {
        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, &guidUsbHub, idx, &ifData)) {
            break;
        }
        DWORD reqLen = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, NULL, 0, &reqLen, NULL);
        if (reqLen == 0) continue;
        std::vector<BYTE> buf(reqLen);
        auto pDetail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(&buf[0]);
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        SP_DEVINFO_DATA devData = {};
        devData.cbSize = sizeof(devData);
        if (!SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, pDetail, reqLen, NULL, &devData)) {
            continue;
        }
        HANDLE hHub = CreateFile(pDetail->DevicePath, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hHub == INVALID_HANDLE_VALUE) {
            continue;
        }

        USB_NODE_INFORMATION hubInfo = {};
        hubInfo.NodeType = UsbHub;
        DWORD bytesReturned;
        if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_INFORMATION, &hubInfo, sizeof(hubInfo), &hubInfo, sizeof(hubInfo), &bytesReturned, NULL)) {
            CloseHandle(hHub);
            continue;
        }
        ULONG portCount = hubInfo.u.HubInformation.HubDescriptor.bNumberOfPorts;
        for (ULONG port = 1; port <= portCount; port++) {
            USB_NODE_CONNECTION_INFORMATION_EX conn = {};
            conn.ConnectionIndex = port;
            DWORD outBytes;
            BOOL ok = DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, &conn, sizeof(conn), &conn, sizeof(conn), &outBytes, NULL);
            UsbPortInfo pi;
            pi.hubId = (int)idx;
            pi.portIndex = port;
            pi.devicePresent = false;
            pi.deviceEnabled = false;

            if (ok) {
                if (conn.ConnectionStatus == DeviceConnected) {
                    pi.devicePresent = true;
                    pi.deviceEnabled = true;
                }
                // No else if for DeviceDisabled since it may not be defined
                // Other statuses treated as no device or inactive
            }
            list.push_back(pi);
        }
        CloseHandle(hHub);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return list;
}

void DoRefresh(HWND hWnd) {
    g_ports = EnumerateUsbPorts();
    InvalidateRect(hWnd, NULL, TRUE);
}

void RenderPorts(HDC hdc, RECT &rc) {
    SetBkMode(hdc, TRANSPARENT);
    int y = rc.top + 10;
    for (auto &p : g_ports) {
        COLORREF color = RGB(128,128,128); // Default grey (passive)
        if (p.devicePresent && p.deviceEnabled)
            color = RGB(0,200,0);         // Green (active)
        else if (p.devicePresent && !p.deviceEnabled)
            color = RGB(200,0,0);         // Red (deactivated)
        SetTextColor(hdc, color);
        std::wstringstream wss;
        wss << L"Hub " << p.hubId << L", Port " << p.portIndex;
        std::wstring ws = wss.str();
        TextOut(hdc, rc.left + 10, y, ws.c_str(), (int)ws.size());
        y += 20;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == g_timerId) {
            DoRefresh(hWnd);
        }
        return 0;

    case WM_CREATE:
        CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE, 500, 330, 80, 25, hWnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == 1) {
            DoRefresh(hWnd);
        }
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            RenderPorts(hdc, rc);
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}
