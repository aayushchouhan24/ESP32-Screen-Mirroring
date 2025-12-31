#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shcore.lib")

const int DISPLAY_WIDTH = 240;
const int DISPLAY_HEIGHT = 135;
const int CHUNK_SIZE = 1400;
const int FRAME_SIZE = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;
const int UDP_PORT = 3333;

#define COLOR_BG RGB(15, 15, 15)
#define COLOR_TEXT RGB(180, 180, 180)
#define COLOR_TEXT_BRIGHT RGB(220, 220, 220)
#define COLOR_ACCENT RGB(190, 255, 60)
#define COLOR_STOP RGB(220, 60, 60)
#define COLOR_BORDER RGB(45, 45, 45)
#define COLOR_INPUT_BG RGB(25, 25, 25)
#define COLOR_INPUT_BORDER RGB(55, 55, 55)

#define ID_START_STOP_BUTTON 1001
#define ID_CURSOR_CHECK 1002
#define ID_FPS_COMBO 1003
#define ID_SCREEN_COMBO 1005

struct MonitorInfo {
    HMONITOR hMonitor;
    RECT rect;
    std::string name;
    bool isPrimary;
};

std::vector<MonitorInfo> g_monitors;

HWND g_hwndMain, g_hwndStatus, g_hwndFPS, g_hwndIP, g_hwndStartStop;
HWND g_hwndCursorCheck, g_hwndFPSCombo, g_hwndPreview, g_hwndScreenCombo;
HBRUSH g_hBrushBg;
HFONT g_hFontLarge, g_hFontNormal, g_hFontSmall;
std::thread* g_streamThread = nullptr;
std::vector<uint16_t> g_previewBuffer;

std::atomic<bool> g_streaming(false);
std::atomic<bool> g_showCursor(true);
std::atomic<int> g_targetFPS(30);
std::atomic<int> g_selectedScreen(0);
std::atomic<int> g_lastSelectedScreen(0);

inline uint8_t clamp(int val) {
    return (val < 0) ? 0 : (val > 255) ? 255 : val;
}

// Standard RGB888 to RGB565 conversion for ST7789V2
inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rgb = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (rgb >> 8) | (rgb << 8);
}

void UpdateStatus(const char* text) {
    SetWindowTextA(g_hwndStatus, text);
}

void UpdateFPS(float fps) {
    char buf[64];
    sprintf(buf, "%.1f FPS", fps);
    SetWindowTextA(g_hwndFPS, buf);
}

class ScreenStreamer {
private:
    SOCKET sock;
    sockaddr_in dest_addr;
    HDC hdcScreen, hdcMem;
    HBITMAP hbmScreen;
    BITMAPINFOHEADER bi;
    std::vector<uint8_t> screenBuffer;
    std::vector<uint16_t> frameBuffer;
    std::vector<uint8_t> sendBuffer;
    int screenWidth, screenHeight;
    int currentScreenIdx;
    int monitorLeft, monitorTop;

    void setupCapture() {
        // Clean up ALL existing resources
        if (hbmScreen) {
            DeleteObject(hbmScreen);
            hbmScreen = NULL;
        }
        if (hdcMem) {
            DeleteDC(hdcMem);
            hdcMem = NULL;
        }
        if (hdcScreen) {
            ReleaseDC(NULL, hdcScreen);
            hdcScreen = NULL;
        }
        
        currentScreenIdx = g_selectedScreen.load();
        g_lastSelectedScreen = currentScreenIdx;
        
        // Get selected monitor info
        if (currentScreenIdx >= 0 && currentScreenIdx < (int)g_monitors.size()) {
            RECT monRect = g_monitors[currentScreenIdx].rect;
            screenWidth = monRect.right - monRect.left;
            screenHeight = monRect.bottom - monRect.top;
            monitorLeft = monRect.left;
            monitorTop = monRect.top;
        } else {
            screenWidth = GetSystemMetrics(SM_CXSCREEN);
            screenHeight = GetSystemMetrics(SM_CYSCREEN);
            monitorLeft = 0;
            monitorTop = 0;
        }
        
        // Get fresh DC for the entire virtual desktop
        hdcScreen = GetDC(NULL);

        hdcMem = CreateCompatibleDC(hdcScreen);
        hbmScreen = CreateCompatibleBitmap(hdcScreen, screenWidth, screenHeight);
        SelectObject(hdcMem, hbmScreen);

        ZeroMemory(&bi, sizeof(BITMAPINFOHEADER));
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = screenWidth;
        bi.biHeight = -screenHeight;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        screenBuffer.resize(screenWidth * screenHeight * 4);
    }

public:
    ScreenStreamer(const char* ip, int port) : hdcScreen(NULL), hdcMem(NULL), hbmScreen(NULL) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int sendBufSize = 512 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sendBufSize, sizeof(sendBufSize));
        DWORD timeout = 1;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &dest_addr.sin_addr);
        
        frameBuffer.resize(DISPLAY_WIDTH * DISPLAY_HEIGHT);
        sendBuffer.resize(CHUNK_SIZE + 3);
        
        setupCapture();
    }

    ~ScreenStreamer() {
        DeleteObject(hbmScreen);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        closesocket(sock);
        WSACleanup();
    }

    void captureAndResize() {
        // Check if screen selection changed - reinitialize if needed
        int newScreenIdx = g_selectedScreen.load();
        if (newScreenIdx != currentScreenIdx) {
            setupCapture();
        }
        
        BitBlt(hdcMem, 0, 0, screenWidth, screenHeight, hdcScreen, monitorLeft, monitorTop, SRCCOPY);
        
        if (g_showCursor) {
            CURSORINFO ci = { sizeof(CURSORINFO) };
            if (GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING) {
                POINT pt;
                GetCursorPos(&pt);
                // Adjust cursor position relative to current monitor
                int cursorX = pt.x - monitorLeft;
                int cursorY = pt.y - monitorTop;
                // Only draw cursor if it's on this monitor
                if (cursorX >= 0 && cursorX < screenWidth && cursorY >= 0 && cursorY < screenHeight) {
                    DrawIconEx(hdcMem, cursorX, cursorY, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
                }
            }
        }
        
        GetDIBits(hdcMem, hbmScreen, 0, screenHeight, screenBuffer.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

        // Calculate aspect ratio scaling to fit with black borders
        float screenAspect = (float)screenWidth / screenHeight;
        float displayAspect = (float)DISPLAY_WIDTH / DISPLAY_HEIGHT;
        
        int displayW, displayH, offsetX, offsetY;
        
        if (screenAspect > displayAspect) {
            // Screen is wider - use full width, add top/bottom black bars
            displayW = DISPLAY_WIDTH;
            displayH = (int)(DISPLAY_WIDTH / screenAspect);
            offsetX = 0;
            offsetY = (DISPLAY_HEIGHT - displayH) / 2;
        } else {
            // Screen is taller - use full height, add left/right black bars
            displayH = DISPLAY_HEIGHT;
            displayW = (int)(DISPLAY_HEIGHT * screenAspect);
            offsetX = (DISPLAY_WIDTH - displayW) / 2;
            offsetY = 0;
        }
        
        // Clear to black
        memset(frameBuffer.data(), 0, FRAME_SIZE);
        
        // Scale screen to fit display area
        for (int y = 0; y < displayH; y++) {
            for (int x = 0; x < displayW; x++) {
                int src_x = (x * screenWidth) / displayW;
                int src_y = (y * screenHeight) / displayH;
                if (src_x >= screenWidth) src_x = screenWidth - 1;
                if (src_y >= screenHeight) src_y = screenHeight - 1;
                
                int src_idx = (src_y * screenWidth + src_x) * 4;
                uint8_t b = screenBuffer[src_idx];
                uint8_t g = screenBuffer[src_idx + 1];
                uint8_t r = screenBuffer[src_idx + 2];
                
                int dst_x = offsetX + x;
                int dst_y = offsetY + y;
                frameBuffer[dst_y * DISPLAY_WIDTH + dst_x] = rgb888_to_rgb565(r, g, b);
            }
        }
        
        if (g_previewBuffer.size() > 0) {
            memcpy(g_previewBuffer.data(), frameBuffer.data(), FRAME_SIZE);
            if (g_hwndPreview) InvalidateRect(g_hwndPreview, NULL, FALSE);
        }
    }

    void sendFrame() {
        uint8_t* frameData = (uint8_t*)frameBuffer.data();
        int num_chunks = (FRAME_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE;
        for (int chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
            int offset = chunk_idx * CHUNK_SIZE;
            int chunk_size = (offset + CHUNK_SIZE > FRAME_SIZE) ? (FRAME_SIZE - offset) : CHUNK_SIZE;
            sendBuffer[0] = 0xAA;
            sendBuffer[1] = 0x55;
            sendBuffer[2] = chunk_idx;
            memcpy(sendBuffer.data() + 3, frameData + offset, chunk_size);
            sendto(sock, (char*)sendBuffer.data(), chunk_size + 3, 0, (sockaddr*)&dest_addr, sizeof(dest_addr));
        }
    }
};

void StreamThread(std::string ip) {
    UpdateStatus("[*] CONNECTING...");
    ScreenStreamer streamer(ip.c_str(), UDP_PORT);
    UpdateStatus("[*] STREAMING...");
    g_streaming = true;
    
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    
    while (g_streaming) {
        UpdateStatus("[*] STREAMING...");
        streamer.captureAndResize();
        streamer.sendFrame();
        
        frameCount++;
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastTime).count();
        if (elapsed >= 1.0f) {
            UpdateFPS(frameCount / elapsed);
            frameCount = 0;
            lastTime = now;
        }
        
        int delay = 1000 / g_targetFPS;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

std::string scanForESP() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    BOOL bBroadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&bBroadcast, sizeof(bBroadcast));
    
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    
    const char* subnets[] = {"192.168.1.255", "192.168.0.255", "192.168.10.255", "192.168.100.255"};
    
    for (const char* subnet : subnets) {
        sockaddr_in broadcast_addr;
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(UDP_PORT);
        inet_pton(AF_INET, subnet, &broadcast_addr.sin_addr);
        
        uint8_t ping[2] = {0xAA, 0x55};
        sendto(sock, (char*)ping, 2, 0, (sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    char buffer[1024];
    sockaddr_in from_addr;
    int from_len = sizeof(from_addr);
    int recv_len = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from_addr, &from_len);
    
    closesocket(sock);
    WSACleanup();
    
    if (recv_len >= 2 && buffer[0] == (char)0xAA && buffer[1] == (char)0x55) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));
        return std::string(ip_str);
    }
    
    return "";
}

void AutoDetectAndStart() {
    UpdateStatus("[~] SEARCHING...");
    
    while (true) {
        std::string ip = scanForESP();
        if (!ip.empty()) {
            char msg[128];
            sprintf(msg, "Found: %s", ip.c_str());
            SetWindowTextA(g_hwndIP, msg);
            UpdateStatus("[*] ESP FOUND!");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            if (g_streamThread) delete g_streamThread;
            g_streamThread = new std::thread(StreamThread, ip);
            SetWindowTextA(g_hwndStartStop, "STOP");
            InvalidateRect(g_hwndStartStop, NULL, TRUE);
            return;
        }
        
        UpdateStatus("[~] RETRY IN 3s...");
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorInfo info;
    info.hMonitor = hMonitor;
    info.rect = *lprcMonitor;
    
    MONITORINFOEXA monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFOEXA);
    GetMonitorInfoA(hMonitor, &monitorInfo);
    
    info.isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.name = monitorInfo.szDevice;
    
    // Create display name
    char displayName[128];
    int width = lprcMonitor->right - lprcMonitor->left;
    int height = lprcMonitor->bottom - lprcMonitor->top;
    
    if (info.isPrimary) {
        sprintf(displayName, "Screen %d (Primary) - %dx%d", (int)g_monitors.size() + 1, width, height);
    } else {
        sprintf(displayName, "Screen %d - %dx%d", (int)g_monitors.size() + 1, width, height);
    }
    info.name = displayName;
    
    g_monitors.push_back(info);
    return TRUE;
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            if (g_previewBuffer.size() > 0) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;
                
                std::vector<uint8_t> rgb888(DISPLAY_WIDTH * DISPLAY_HEIGHT * 3);
                for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
                    uint16_t rgb565 = g_previewBuffer[i];
                    // Swap bytes back
                    rgb565 = (rgb565 >> 8) | (rgb565 << 8);
                    
                    // Decode RGB565 back to RGB888 (reverse of standard conversion)
                    uint8_t r = (rgb565 >> 8) & 0xF8;  // Extract R (top 5 bits)
                    uint8_t g = (rgb565 >> 3) & 0xFC;  // Extract G (middle 6 bits)
                    uint8_t b = (rgb565 << 3) & 0xF8;  // Extract B (bottom 5 bits)
                    
                    // Fill in lower bits for smoother preview
                    r |= (r >> 5);
                    g |= (g >> 6);
                    b |= (b >> 5);
                    
                    rgb888[i*3] = b;
                    rgb888[i*3+1] = g;
                    rgb888[i*3+2] = r;
                }
                
                BITMAPINFO bmi = {0};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = DISPLAY_WIDTH;
                bmi.bmiHeader.biHeight = -DISPLAY_HEIGHT;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 24;
                bmi.bmiHeader.biCompression = BI_RGB;
                
                SetStretchBltMode(hdc, HALFTONE);
                StretchDIBits(hdc, 0, 0, w, h, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                    rgb888.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void DrawCustomButton(LPDRAWITEMSTRUCT dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool isPressed = (dis->itemState & ODS_SELECTED);
    
    COLORREF bgColor = COLOR_BG;
    COLORREF borderColor = COLOR_ACCENT;
    COLORREF textColor = COLOR_ACCENT;
    
    char text[32];
    GetWindowTextA(dis->hwndItem, text, sizeof(text));
    
    // Determine colors based on button state
    if (dis->CtlID == ID_START_STOP_BUTTON) {
        if (strcmp(text, "STOP") == 0 || g_streaming) {
            borderColor = COLOR_STOP;
            textColor = COLOR_STOP;
            bgColor = RGB(45, 20, 20);
        } else {
            borderColor = COLOR_ACCENT;
            textColor = COLOR_ACCENT;
            bgColor = RGB(25, 35, 20);
        }
    }
    
    if (isPressed) {
        bgColor = RGB(55, 55, 55);
    }
    
    // Draw background
    HBRUSH hBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);
    
    // Draw border (2px rounded effect)
    HPEN hPen = CreatePen(PS_SOLID, 2, borderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, 8, 8);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
    
    // Draw text
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textColor);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontNormal);
    DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}

HBRUSH g_hBrushInput = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, COLOR_TEXT);
            SetBkColor(hdcStatic, COLOR_BG);
            return (LRESULT)g_hBrushBg;
        }
        
        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            SetTextColor(hdcEdit, COLOR_TEXT_BRIGHT);
            SetBkColor(hdcEdit, COLOR_INPUT_BG);
            if (!g_hBrushInput) g_hBrushInput = CreateSolidBrush(COLOR_INPUT_BG);
            return (LRESULT)g_hBrushInput;
        }
        
        case WM_CTLCOLORLISTBOX: {
            HDC hdcList = (HDC)wParam;
            SetTextColor(hdcList, COLOR_TEXT_BRIGHT);
            SetBkColor(hdcList, COLOR_INPUT_BG);
            if (!g_hBrushInput) g_hBrushInput = CreateSolidBrush(COLOR_INPUT_BG);
            return (LRESULT)g_hBrushInput;
        }
        
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                DrawCustomButton(dis);
                return TRUE;
            }
            return FALSE;
        }
        
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case ID_START_STOP_BUTTON: {
                        if (!g_streaming) {
                            char ip[128];
                            GetWindowTextA(g_hwndIP, ip, sizeof(ip));
                            if (strlen(ip) > 0 && strstr(ip, "Found:") == NULL) {
                                if (g_streamThread) delete g_streamThread;
                                g_streamThread = new std::thread(StreamThread, std::string(ip));
                                SetWindowTextA(g_hwndStartStop, "STOP");
                                InvalidateRect(g_hwndStartStop, NULL, TRUE);
                            } else {
                                std::thread(AutoDetectAndStart).detach();
                            }
                        } else {
                            g_streaming = false;
                            if (g_streamThread && g_streamThread->joinable()) {
                                g_streamThread->join();
                                delete g_streamThread;
                                g_streamThread = nullptr;
                            }
                            UpdateStatus("[ ] STOPPED");
                            SetWindowTextA(g_hwndStartStop, "START");
                        }
                        break;
                    }
                    case ID_CURSOR_CHECK:
                        g_showCursor = (SendMessage(g_hwndCursorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        break;
                }
            } else if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == ID_FPS_COMBO) {
                int sel = SendMessageA(g_hwndFPSCombo, CB_GETCURSEL, 0, 0);
                int fps_values[] = {15, 20, 25, 30, 40, 50, 60};
                if (sel >= 0 && sel < 7) g_targetFPS = fps_values[sel];
            } else if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == ID_SCREEN_COMBO) {
                int sel = SendMessageA(g_hwndScreenCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_monitors.size()) g_selectedScreen = sel;
            }
            return 0;
            
        case WM_DESTROY:
            g_streaming = false;
            if (g_streamThread && g_streamThread->joinable()) {
                g_streamThread->join();
                delete g_streamThread;
            }
            if (g_hBrushInput) DeleteObject(g_hBrushInput);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // MUST be first - before any other Windows calls
    // Use Per-Monitor DPI Awareness V2 for proper multi-monitor support
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    
    WNDCLASSEXA wc = {sizeof(WNDCLASSEXA)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "M5ScreenStreamer";
    wc.hbrBackground = CreateSolidBrush(COLOR_BG);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExA(&wc);
    
    WNDCLASSEXA wcPreview = {sizeof(WNDCLASSEXA)};
    wcPreview.lpfnWndProc = PreviewWndProc;
    wcPreview.hInstance = hInstance;
    wcPreview.lpszClassName = "M5PreviewWindow";
    wcPreview.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcPreview.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExA(&wcPreview);

    g_hwndMain = CreateWindowExA(0, "M5ScreenStreamer", "M5 Screen Streamer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 550, 520, NULL, NULL, hInstance, NULL);

    g_hBrushBg = CreateSolidBrush(COLOR_BG);
    g_hFontLarge = CreateFontA(26, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    g_hFontNormal = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    g_hFontSmall = CreateFontA(10, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    
    // Enumerate monitors
    g_monitors.clear();
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    HWND hwndTitle = CreateWindowExA(0, "STATIC", "M5STICKC PLUS2 STREAMER",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 18, 500, 32, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndTitle, WM_SETFONT, (WPARAM)g_hFontLarge, TRUE);
    
    // Make title brighter
    HDC hdcTitle = GetDC(hwndTitle);
    SetTextColor(hdcTitle, COLOR_TEXT_BRIGHT);
    ReleaseDC(hwndTitle, hdcTitle);

    HWND hwndSubtitle = CreateWindowExA(0, "STATIC", "Real-time Screen Streaming  |  240x135  |  UDP 3333",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 54, 500, 16, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndSubtitle, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    // Connection section
    HWND hwndConnectBox = CreateWindowExA(0, "BUTTON", "Connection",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 20, 85, 510, 75, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndConnectBox, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    HWND hwndIPLabel = CreateWindowExA(0, "STATIC", "ESP32 IP:", WS_CHILD | WS_VISIBLE,
        35, 110, 65, 20, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndIPLabel, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    g_hwndIP = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
        105, 108, 175, 24, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(g_hwndIP, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    g_hwndFPS = CreateWindowExA(0, "STATIC", "0.0 FPS", WS_CHILD | WS_VISIBLE | SS_CENTER,
        290, 108, 90, 24, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(g_hwndFPS, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    g_hwndStartStop = CreateWindowExA(0, "BUTTON", "START", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        390, 103, 130, 34, g_hwndMain, (HMENU)ID_START_STOP_BUTTON, hInstance, NULL);

    g_hwndStatus = CreateWindowExA(0, "STATIC", "[ ] READY", WS_CHILD | WS_VISIBLE | SS_CENTER,
        35, 172, 485, 20, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(g_hwndStatus, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    // Settings section
    HWND hwndSettingsBox = CreateWindowExA(0, "BUTTON", "Settings",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 20, 210, 510, 68, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndSettingsBox, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    HWND hwndScreenLabel = CreateWindowExA(0, "STATIC", "Screen:", WS_CHILD | WS_VISIBLE,
        35, 237, 75, 20, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndScreenLabel, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    g_hwndScreenCombo = CreateWindowExA(0, "COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        115, 235, 150, 150, g_hwndMain, (HMENU)ID_SCREEN_COMBO, hInstance, NULL);
    SendMessage(g_hwndScreenCombo, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    // Populate screen combo
    for (size_t i = 0; i < g_monitors.size(); i++) {
        SendMessageA(g_hwndScreenCombo, CB_ADDSTRING, 0, (LPARAM)g_monitors[i].name.c_str());
    }
    SendMessageA(g_hwndScreenCombo, CB_SETCURSEL, 0, 0);

    HWND hwndFPSLabel = CreateWindowExA(0, "STATIC", "Target FPS:", WS_CHILD | WS_VISIBLE,
        275, 237, 75, 20, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndFPSLabel, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    g_hwndFPSCombo = CreateWindowExA(0, "COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        355, 235, 65, 150, g_hwndMain, (HMENU)ID_FPS_COMBO, hInstance, NULL);
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"15");
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"20");
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"25");
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"30");
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"40");
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"50");
    SendMessageA(g_hwndFPSCombo, CB_ADDSTRING, 0, (LPARAM)"60");
    SendMessageA(g_hwndFPSCombo, CB_SETCURSEL, 3, 0);
    SendMessage(g_hwndFPSCombo, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    g_hwndCursorCheck = CreateWindowExA(0, "BUTTON", "Show Cursor", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        430, 237, 85, 20, g_hwndMain, (HMENU)ID_CURSOR_CHECK, hInstance, NULL);
    SendMessage(g_hwndCursorCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g_hwndCursorCheck, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    // Preview section
    HWND hwndPreviewBox = CreateWindowExA(0, "BUTTON", "Live Preview",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 20, 288, 510, 185, g_hwndMain, NULL, hInstance, NULL);
    SendMessage(hwndPreviewBox, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    g_hwndPreview = CreateWindowExA(WS_EX_CLIENTEDGE, "M5PreviewWindow", NULL, WS_CHILD | WS_VISIBLE,
        155, 310, 240, 135, g_hwndMain, NULL, hInstance, NULL);
    
    g_previewBuffer.resize(DISPLAY_WIDTH * DISPLAY_HEIGHT, 0);

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    std::thread(AutoDetectAndStart).detach();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
