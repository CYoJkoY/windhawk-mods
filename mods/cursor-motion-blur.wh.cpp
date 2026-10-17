// ==WindhawkMod==
// @id              cursor-motion-blur
// @name            Cursor Motion Blur
// @description     Adds high-speed cartoon motion blur to your mouse pointer.
// @version         2.0
// @author          TheatriChris
// @github          https://github.com/chrisc44890
// @license         MIT
// @include         windhawk.exe
// @compilerOptions -ld2d1 -lole32 -lgdi32 -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Cursor Smear Frames
Replaces your standard Windows cursor with a smooth, tapered motion blur trail when moving at high speeds. Hardware accelerated via Direct2D.

![Demonstration](https://i.imgur.com/XJWuSS3.gif)

### Features
* **Dynamic 2D Mesh:** Procedurally generates a continuous, overlapping-free polygon ribbon.
* **Direct2D Rendering:** Buttery smooth sub-pixel anti-aliasing calculated directly on your GPU.
* **Zero Input Lag:** Draws directly off the hardware cursor coordinates.
* **Game Detection:** Automatically disables itself when playing fullscreen or borderless DirectX games.
* **Taskbar Friendly:** Retains native edge-detection for auto-hiding taskbars.
* **Bounded Rendering:** Keeps the overlay hidden when idle and redraws only the local trail region when motion blur is active.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- trigger_velocity: 25
  $name: Trigger Velocity
  $description: How fast the mouse needs to trigger the blur (pixels per frame).
- stop_velocity: 10
  $name: Stop Velocity
  $description: Velocity threshold to stop the blur. Must be lower than Trigger Velocity.
- tail_offset_x: 6
  $name: Tail Offset X
  $description: X-axis offset for where the tail connects to the cursor (-128 to 128).
- tail_offset_y: 10
  $name: Tail Offset Y
  $description: Y-axis offset for where the tail connects to the cursor (-128 to 128).
- tail_length: 10
  $name: Tail Length
  $description: How many frames the blur trails behind you (2 to 64).
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <d2d1.h>
#include <math.h>
#include <algorithm>
#include <shellapi.h>
#include <deque>
#include <vector>

// Bound the geometry workload even if settings are edited outside the normal UI.
constexpr int kMaxTailLength = 64;
constexpr int kMaxTailOffset = 128;
constexpr int kTrailBoundsPadding = 12;
constexpr UINT_PTR kSmearTimerId = 1;

// Global state variables
HWND g_overlayHwnd = NULL;
bool g_timerActive = false;
volatile LONG g_shutdownRequested = 0;
HANDLE g_threadHandle = NULL;
std::deque<POINT> g_history;
POINT g_lastPos = { 0, 0 }; 

// Direct2D globals
ID2D1Factory* g_pD2DFactory = nullptr;
ID2D1DCRenderTarget* g_pDCRenderTarget = nullptr;
ID2D1SolidColorBrush* g_pCapBlackBrush = nullptr;
ID2D1SolidColorBrush* g_pCapWhiteBrush = nullptr;

// Cached backbuffer so we stop nuking the RAM every frame
HDC g_hdcMem = NULL;
HBITMAP g_hBitmap = NULL;
int g_cachedBufferW = 0;
int g_cachedBufferH = 0;

// Settings cache
float g_triggerVelocity = 25.0f;
float g_stopVelocity = 10.0f;
int g_tailOffsetX = 6;
int g_tailOffsetY = 10;
int g_tailLength = 10;

void LoadSettings() {
    g_triggerVelocity = static_cast<float>(Wh_GetIntSetting(L"trigger_velocity"));
    g_stopVelocity = static_cast<float>(Wh_GetIntSetting(L"stop_velocity"));
    g_tailOffsetX = Wh_GetIntSetting(L"tail_offset_x");
    g_tailOffsetY = Wh_GetIntSetting(L"tail_offset_y");
    g_tailLength = Wh_GetIntSetting(L"tail_length");

    if (g_triggerVelocity <= 0.0f) g_triggerVelocity = 25.0f;
    if (g_stopVelocity <= 0.0f) g_stopVelocity = 10.0f;
    if (g_stopVelocity >= g_triggerVelocity) {
        g_stopVelocity = g_triggerVelocity * 0.4f;
    }

    g_tailOffsetX = std::max(-kMaxTailOffset, std::min(g_tailOffsetX, kMaxTailOffset));
    g_tailOffsetY = std::max(-kMaxTailOffset, std::min(g_tailOffsetY, kMaxTailOffset));
    g_tailLength = std::max(2, std::min(g_tailLength, kMaxTailLength));
}

bool CreateRibbonGeometry(
    ID2D1Factory* factory,
    const std::vector<D2D1_POINT_2F>& left,
    const std::vector<D2D1_POINT_2F>& right,
    ID2D1PathGeometry** geometryOut) {
    if (!factory || !geometryOut || left.empty() || left.size() != right.size()) {
        return false;
    }

    *geometryOut = nullptr;
    ID2D1PathGeometry* path = nullptr;
    HRESULT hr = factory->CreatePathGeometry(&path);
    if (FAILED(hr) || !path) {
        return false;
    }

    ID2D1GeometrySink* sink = nullptr;
    hr = path->Open(&sink);
    if (FAILED(hr) || !sink) {
        path->Release();
        return false;
    }

    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(left.front(), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < left.size(); ++i) {
        sink->AddLine(left[i]);
    }
    for (size_t i = right.size(); i > 0; --i) {
        sink->AddLine(right[i - 1]);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    hr = sink->Close();
    sink->Release();
    if (FAILED(hr)) {
        path->Release();
        return false;
    }

    *geometryOut = path;
    return true;
}

bool IsGameRunning() {
    HWND hwnd = GetForegroundWindow();
    
    if (!hwnd || hwnd == GetDesktopWindow()) {
        return false;
    }

    // Cache the desktop worker handles so we don't spam the Windows string table search literally 60 times a second
    static HWND s_hwndProgman = FindWindowW(L"Progman", NULL);
    static HWND s_hwndWorkerW = FindWindowW(L"WorkerW", NULL);
    
    if (hwnd == s_hwndProgman || hwnd == s_hwndWorkerW) {
        return false;
    }

    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN) {
            return true;
        }
    }

    RECT rcApp;
    GetWindowRect(hwnd, &rcApp);
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMonitor, &mi)) {
        bool isFullscreen = (rcApp.left <= mi.rcMonitor.left &&
                             rcApp.top <= mi.rcMonitor.top &&
                             rcApp.right >= mi.rcMonitor.right &&
                             rcApp.bottom >= mi.rcMonitor.bottom);
        if (isFullscreen) {
            RECT rcClip;
            if (GetClipCursor(&rcClip)) {
                int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if ((rcClip.right - rcClip.left) < vW || (rcClip.bottom - rcClip.top) < vH) {
                    return true;
                }
            }
            
            CURSORINFO ci = { sizeof(CURSORINFO) };
            if (GetCursorInfo(&ci) && ci.flags == 0) {
                return true;
            }
        }
    }
    return false;
}

VOID CALLBACK SmearTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    POINT pt = {};
    if (!GetCursorPos(&pt)) {
        return;
    }

    const float dx = static_cast<float>(pt.x) - static_cast<float>(g_lastPos.x);
    const float dy = static_cast<float>(pt.y) - static_cast<float>(g_lastPos.y);
    const float velocity = sqrt(dx * dx + dy * dy);
    g_lastPos = pt; 

    static DWORD lastFullscreenCheck = 0;
    static bool isGameCached = false;
    static bool isSmearing = false;
    static int lowVelocityFrames = 0;

    if (dwTime - lastFullscreenCheck > 500) {
        isGameCached = IsGameRunning();
        lastFullscreenCheck = dwTime;
    }

    const int vX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vH = GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;

    if (vW <= 0 || vH <= 0) {
        return;
    }

    if (isGameCached) {
        isSmearing = false;
        g_history.clear();
    } else {
        if (velocity > g_triggerVelocity && !isSmearing) {
            isSmearing = true;
            lowVelocityFrames = 0;
        } 
        else if (velocity < g_stopVelocity && isSmearing) {
            lowVelocityFrames++;
            if (lowVelocityFrames > 2) { 
                isSmearing = false;
            }
        } 
        else if (velocity >= g_stopVelocity && isSmearing) {
            lowVelocityFrames = 0;
        }

        if (isSmearing) {
            POINT newPt = { pt.x - vX, pt.y - vY };
            g_history.push_front(newPt);
            while (g_history.size() > (size_t)g_tailLength) { 
                g_history.pop_back();
            }
        } else {
            if (!g_history.empty()) {
                g_history.pop_back();
                if (!g_history.empty()) {
                    g_history.pop_back();
                }
            }
        }
    }

    const bool isDrawing = isSmearing || !g_history.empty();
    static bool isWindowVisible = false;

    if (!isDrawing) {
        if (isWindowVisible) {
            ShowWindow(hwnd, SW_HIDE);
            isWindowVisible = false;
        }

        return;
    }

    // Samples are relative to the virtual-screen origin. Clip the backing surface
    // to the recent trail bounds so rendering cost follows trail area, not desktop area.
    int bufferX = 0;
    int bufferY = 0;
    int bufferW = 1;
    int bufferH = 1;
    if (!g_history.empty()) {
        const POINT& firstPoint = g_history.front();
        int minX = firstPoint.x + g_tailOffsetX;
        int minY = firstPoint.y + g_tailOffsetY;
        int maxX = minX;
        int maxY = minY;

        for (const POINT& historyPoint : g_history) {
            const int pointX = historyPoint.x + g_tailOffsetX;
            const int pointY = historyPoint.y + g_tailOffsetY;
            minX = std::min(minX, pointX);
            minY = std::min(minY, pointY);
            maxX = std::max(maxX, pointX);
            maxY = std::max(maxY, pointY);
        }

        bufferX = std::max(0, std::min(vW - 1, minX - kTrailBoundsPadding));
        bufferY = std::max(0, std::min(vH - 1, minY - kTrailBoundsPadding));
        const int bufferRight = std::max(
            bufferX + 1, std::min(vW, maxX + kTrailBoundsPadding + 1));
        const int bufferBottom = std::max(
            bufferY + 1, std::min(vH, maxY + kTrailBoundsPadding + 1));
        bufferW = bufferRight - bufferX;
        bufferH = bufferBottom - bufferY;
    }

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        if (isWindowVisible) {
            ShowWindow(hwnd, SW_HIDE);
            isWindowVisible = false;
        }
        return;
    }

    // Keep spare capacity so one-pixel movements don't resize the bitmap on every
    // frame. Grow geometrically; shrink only after the trail stays much smaller.
    const bool needsGrowth =
        bufferW > g_cachedBufferW || bufferH > g_cachedBufferH;
    const bool needsShrink =
        g_hBitmap && bufferW * 3 < g_cachedBufferW &&
        bufferH * 3 < g_cachedBufferH;

    if (!g_hBitmap || needsGrowth || needsShrink) {
        int surfaceW = bufferW;
        int surfaceH = bufferH;
        if (g_hBitmap && !needsShrink) {
            surfaceW = bufferW > g_cachedBufferW
                ? std::max(bufferW, std::min(vW, g_cachedBufferW * 2))
                : g_cachedBufferW;
            surfaceH = bufferH > g_cachedBufferH
                ? std::max(bufferH, std::min(vH, g_cachedBufferH * 2))
                : g_cachedBufferH;
        }

        if (g_hdcMem) {
            DeleteDC(g_hdcMem);
            g_hdcMem = NULL;
        }
        if (g_hBitmap) {
            DeleteObject(g_hBitmap);
            g_hBitmap = NULL;
        }

        g_hdcMem = CreateCompatibleDC(hdcScreen);
        if (g_hdcMem) {
            g_hBitmap = CreateCompatibleBitmap(hdcScreen, surfaceW, surfaceH);
        }

        if (!g_hdcMem || !g_hBitmap) {
            if (g_hdcMem) {
                DeleteDC(g_hdcMem);
                g_hdcMem = NULL;
            }
            if (g_hBitmap) {
                DeleteObject(g_hBitmap);
                g_hBitmap = NULL;
            }
            ReleaseDC(NULL, hdcScreen);
            if (isWindowVisible) {
                ShowWindow(hwnd, SW_HIDE);
                isWindowVisible = false;
            }
            return;
        }

        HGDIOBJ previousBitmap = SelectObject(g_hdcMem, g_hBitmap);
        if (!previousBitmap || previousBitmap == HGDI_ERROR) {
            DeleteDC(g_hdcMem);
            g_hdcMem = NULL;
            DeleteObject(g_hBitmap);
            g_hBitmap = NULL;
            ReleaseDC(NULL, hdcScreen);
            if (isWindowVisible) {
                ShowWindow(hwnd, SW_HIDE);
                isWindowVisible = false;
            }
            return;
        }

        g_cachedBufferW = surfaceW;
        g_cachedBufferH = surfaceH;

        if (g_pDCRenderTarget) {
            g_pDCRenderTarget->Release();
            g_pDCRenderTarget = nullptr;
        }
    }

        // Initialize Direct2D Render Target if we don't have one
        if (!g_pDCRenderTarget && g_pD2DFactory) {
            if (g_pCapBlackBrush) { g_pCapBlackBrush->Release(); g_pCapBlackBrush = nullptr; }
            if (g_pCapWhiteBrush) { g_pCapWhiteBrush->Release(); g_pCapWhiteBrush = nullptr; }

            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT
            );
            
            g_pD2DFactory->CreateDCRenderTarget(&props, &g_pDCRenderTarget);

            if (g_pDCRenderTarget) {
                // Solid colors for the perfectly rounded head caps and the entire body
                g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.86f), &g_pCapBlackBrush);
                g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.86f), &g_pCapWhiteBrush);
            }
        }

        if (!g_pDCRenderTarget || !g_pCapBlackBrush || !g_pCapWhiteBrush) {
            if (g_pCapBlackBrush) { g_pCapBlackBrush->Release(); g_pCapBlackBrush = nullptr; }
            if (g_pCapWhiteBrush) { g_pCapWhiteBrush->Release(); g_pCapWhiteBrush = nullptr; }
            if (g_pDCRenderTarget) { g_pDCRenderTarget->Release(); g_pDCRenderTarget = nullptr; }
            ReleaseDC(NULL, hdcScreen);
            if (isWindowVisible) {
                ShowWindow(hwnd, SW_HIDE);
                isWindowVisible = false;
            }
            return;
        }

        if (g_pDCRenderTarget) {
            RECT rc = { 0, 0, bufferW, bufferH };
            const HRESULT bindResult = g_pDCRenderTarget->BindDC(g_hdcMem, &rc);
            if (FAILED(bindResult)) {
                if (g_pCapBlackBrush) {
                    g_pCapBlackBrush->Release();
                    g_pCapBlackBrush = nullptr;
                }
                if (g_pCapWhiteBrush) {
                    g_pCapWhiteBrush->Release();
                    g_pCapWhiteBrush = nullptr;
                }
                g_pDCRenderTarget->Release();
                g_pDCRenderTarget = nullptr;
                ReleaseDC(NULL, hdcScreen);
                if (isWindowVisible) {
                    ShowWindow(hwnd, SW_HIDE);
                    isWindowVisible = false;
                }
                return;
            }

            g_pDCRenderTarget->BeginDraw();
            g_pDCRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)); 

            if (isDrawing && g_history.size() >= 2) {
                // CHAIKIN SUBDIVISION: Mathematically curve the rigid mouse coordinates into a dense bezier curve
                // Retain bounded scratch capacity across frames. Two subdivision passes
                // expand at most 64 samples to 256 points.
                static std::vector<D2D1_POINT_2F> smoothed;
                static std::vector<D2D1_POINT_2F> subdivision;
                static std::vector<D2D1_POINT_2F> leftOutline;
                static std::vector<D2D1_POINT_2F> rightOutline;
                static std::vector<D2D1_POINT_2F> leftCore;
                static std::vector<D2D1_POINT_2F> rightCore;

                const size_t scratchCapacity = static_cast<size_t>(g_tailLength) * 4;
                if (smoothed.capacity() < scratchCapacity) smoothed.reserve(scratchCapacity);
                if (subdivision.capacity() < scratchCapacity) subdivision.reserve(scratchCapacity);
                if (leftOutline.capacity() < scratchCapacity) leftOutline.reserve(scratchCapacity);
                if (rightOutline.capacity() < scratchCapacity) rightOutline.reserve(scratchCapacity);
                if (leftCore.capacity() < scratchCapacity) leftCore.reserve(scratchCapacity);
                if (rightCore.capacity() < scratchCapacity) rightCore.reserve(scratchCapacity);

                smoothed.clear();
                subdivision.clear();
                leftOutline.clear();
                rightOutline.clear();
                leftCore.clear();
                rightCore.clear();

                for (const POINT& point : g_history) {
                    smoothed.push_back(D2D1::Point2F(
                        static_cast<float>(point.x + g_tailOffsetX - bufferX),
                        static_cast<float>(point.y + g_tailOffsetY - bufferY)));
                }

                for (int iter = 0; iter < 2; ++iter) {
                    if (smoothed.size() < 3) break;
                    subdivision.clear();
                    subdivision.push_back(smoothed.front());
                    for (size_t i = 0; i < smoothed.size() - 1; ++i) {
                        const D2D1_POINT_2F p0 = smoothed[i];
                        const D2D1_POINT_2F p1 = smoothed[i + 1];
                        subdivision.push_back(D2D1::Point2F(
                            0.75f * p0.x + 0.25f * p1.x,
                            0.75f * p0.y + 0.25f * p1.y));
                        subdivision.push_back(D2D1::Point2F(
                            0.25f * p0.x + 0.75f * p1.x,
                            0.25f * p0.y + 0.75f * p1.y));
                    }
                    subdivision.push_back(smoothed.back());
                    smoothed.swap(subdivision);
                }

                // PROCEDURAL MESH GENERATION: Calculate perpendicular normals to build the left and right walls of the streak
                const size_t s_len = smoothed.size();

                for (size_t i = 0; i < s_len; ++i) {
                    float dx, dy;
                    if (i == 0) {
                        dx = smoothed[0].x - smoothed[1].x;
                        dy = smoothed[0].y - smoothed[1].y;
                    } else if (i == s_len - 1) {
                        dx = smoothed[i-1].x - smoothed[i].x;
                        dy = smoothed[i-1].y - smoothed[i].y;
                    } else {
                        dx = smoothed[i-1].x - smoothed[i+1].x;
                        dy = smoothed[i-1].y - smoothed[i+1].y;
                    }
                    float len = sqrt(dx*dx + dy*dy);
                    if (len > 0) { dx /= len; dy /= len; } else { dx = 1; dy = 0; }

                    float nx = -dy;
                    float ny = dx;

                    float ratio = (float)i / (s_len - 1);
                    float outW = 10.0f - (10.0f * ratio); // Outer shadow layer tapers to 0
                    float coreW = 6.0f - (6.0f * ratio);  // Inner light layer tapers to 0
                    if (i == s_len - 1) { outW = 0.0f; coreW = 0.0f; }

                    leftOutline.push_back(D2D1::Point2F(smoothed[i].x + nx * outW, smoothed[i].y + ny * outW));
                    rightOutline.push_back(D2D1::Point2F(smoothed[i].x - nx * outW, smoothed[i].y - ny * outW));
                    leftCore.push_back(D2D1::Point2F(smoothed[i].x + nx * coreW, smoothed[i].y + ny * coreW));
                    rightCore.push_back(D2D1::Point2F(smoothed[i].x - nx * coreW, smoothed[i].y - ny * coreW));
                }

                // Skip a frame if any Direct2D geometry allocation fails.
                ID2D1PathGeometry* pOutlineGeom = nullptr;
                ID2D1PathGeometry* pCoreGeom = nullptr;
                ID2D1EllipseGeometry* pBlackEllipse = nullptr;
                ID2D1EllipseGeometry* pWhiteEllipse = nullptr;
                ID2D1GeometryGroup* pBlackGroup = nullptr;
                ID2D1GeometryGroup* pWhiteGroup = nullptr;

                bool geometryReady =
                    CreateRibbonGeometry(g_pD2DFactory, leftOutline, rightOutline, &pOutlineGeom) &&
                    CreateRibbonGeometry(g_pD2DFactory, leftCore, rightCore, &pCoreGeom);

                if (geometryReady) {
                    geometryReady =
                        SUCCEEDED(g_pD2DFactory->CreateEllipseGeometry(
                            D2D1::Ellipse(smoothed[0], 10.0f, 10.0f), &pBlackEllipse)) &&
                        pBlackEllipse &&
                        SUCCEEDED(g_pD2DFactory->CreateEllipseGeometry(
                            D2D1::Ellipse(smoothed[0], 6.0f, 6.0f), &pWhiteEllipse)) &&
                        pWhiteEllipse;
                }

                if (geometryReady) {
                    ID2D1Geometry* blackGeometries[2] = { pOutlineGeom, pBlackEllipse };
                    geometryReady =
                        SUCCEEDED(g_pD2DFactory->CreateGeometryGroup(
                            D2D1_FILL_MODE_WINDING, blackGeometries, 2, &pBlackGroup)) &&
                        pBlackGroup;
                }

                if (geometryReady) {
                    ID2D1Geometry* whiteGeometries[2] = { pCoreGeom, pWhiteEllipse };
                    geometryReady =
                        SUCCEEDED(g_pD2DFactory->CreateGeometryGroup(
                            D2D1_FILL_MODE_WINDING, whiteGeometries, 2, &pWhiteGroup)) &&
                        pWhiteGroup;
                }

                if (geometryReady) {
                    g_pDCRenderTarget->FillGeometry(pBlackGroup, g_pCapBlackBrush);
                    g_pDCRenderTarget->FillGeometry(pWhiteGroup, g_pCapWhiteBrush);
                }

                if (pBlackGroup) pBlackGroup->Release();
                if (pWhiteGroup) pWhiteGroup->Release();
                if (pBlackEllipse) pBlackEllipse->Release();
                if (pWhiteEllipse) pWhiteEllipse->Release();
                if (pOutlineGeom) pOutlineGeom->Release();
                if (pCoreGeom) pCoreGeom->Release();
            }

            const HRESULT drawResult = g_pDCRenderTarget->EndDraw();
            if (FAILED(drawResult)) {
                if (g_pCapBlackBrush) {
                    g_pCapBlackBrush->Release();
                    g_pCapBlackBrush = nullptr;
                }
                if (g_pCapWhiteBrush) {
                    g_pCapWhiteBrush->Release();
                    g_pCapWhiteBrush = nullptr;
                }
                g_pDCRenderTarget->Release();
                g_pDCRenderTarget = nullptr;
                ReleaseDC(NULL, hdcScreen);
                if (isWindowVisible) {
                    ShowWindow(hwnd, SW_HIDE);
                    isWindowVisible = false;
                }
                return;
            }
        }

        BLENDFUNCTION blend = { 0 };
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255; 
        blend.AlphaFormat = AC_SRC_ALPHA;

        POINT ptPos = { vX + bufferX, vY + bufferY };
        SIZE sizeWnd = { bufferW, bufferH };
        POINT ptSrc = { 0, 0 };

        if (UpdateLayeredWindow(hwnd, hdcScreen, &ptPos, &sizeWnd, g_hdcMem,
                                &ptSrc, 0, &blend, ULW_ALPHA)) {
            if (!isWindowVisible) {
                ShowWindow(hwnd, SW_SHOWNA);
                isWindowVisible = true;
            }
        } else if (isWindowVisible) {
            ShowWindow(hwnd, SW_HIDE);
            isWindowVisible = false;
        }

        ReleaseDC(NULL, hdcScreen);
}

LRESULT CALLBACK OverlayWndProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        if (g_timerActive) {
            KillTimer(hwnd, kSmearTimerId);
            g_timerActive = false;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

DWORD WINAPI OverlayThreadProc(LPVOID lpParam) {
    // Direct2D demands COM to be initialized on this thread before it will talk to us
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Tell Windows we aren't a blurry legacy piece of shit so mixed-DPI monitors don't fuck up the math
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory);

    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t CLASS_NAME[] = L"SmearFrameOverlayClass";

    WNDCLASS wc = { };
    wc.lpfnWndProc = OverlayWndProc; 
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClass(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Wh_Log(L"RegisterClass failed: %lu", GetLastError());
        return 0;
    }

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;

    g_overlayHwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS_NAME,
        L"SmearOverlay",
        WS_POPUP,
        screenX, screenY, screenW, screenH,
        NULL, NULL, hInstance, NULL
    );

    if (!g_overlayHwnd) {
        UnregisterClass(CLASS_NAME, hInstance);
        return 0;
    }

    GetCursorPos(&g_lastPos);

    g_timerActive = SetTimer(
        g_overlayHwnd, kSmearTimerId, USER_TIMER_MINIMUM, SmearTimerProc) != 0;
    if (!g_timerActive) {
        Wh_Log(L"Unable to start cursor sampling timer");
        PostMessageW(g_overlayHwnd, WM_CLOSE, 0, 0);
    }

    if (InterlockedCompareExchange(&g_shutdownRequested, 0, 0) != 0) {
        PostMessageW(g_overlayHwnd, WM_CLOSE, 0, 0);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_timerActive && g_overlayHwnd) {
        KillTimer(g_overlayHwnd, kSmearTimerId);
        g_timerActive = false;
    }

    if (g_pCapWhiteBrush) { g_pCapWhiteBrush->Release(); g_pCapWhiteBrush = nullptr; }
    if (g_pCapBlackBrush) { g_pCapBlackBrush->Release(); g_pCapBlackBrush = nullptr; }
    if (g_pDCRenderTarget) { g_pDCRenderTarget->Release(); g_pDCRenderTarget = nullptr; }
    if (g_pD2DFactory) { g_pD2DFactory->Release(); g_pD2DFactory = nullptr; }

    // The bitmap is selected into the memory DC, so release the DC first.
    if (g_hdcMem) {
        DeleteDC(g_hdcMem);
        g_hdcMem = NULL;
    }
    if (g_hBitmap) {
        DeleteObject(g_hBitmap);
        g_hBitmap = NULL;
    }
    if (g_overlayHwnd) {
        DestroyWindow(g_overlayHwnd);
        g_overlayHwnd = nullptr;
    }
    UnregisterClass(CLASS_NAME, hInstance);

    CoUninitialize();
    return 0;
}

BOOL WhTool_ModInit() {
    LoadSettings();
    g_threadHandle = CreateThread(NULL, 0, OverlayThreadProc, NULL, 0, NULL);
    return g_threadHandle != nullptr;
}

void WhTool_ModUninit() {
    InterlockedExchange(&g_shutdownRequested, 1);
    if (g_overlayHwnd) {
        PostMessageW(g_overlayHwnd, WM_CLOSE, 0, 0);
    }
    if (g_threadHandle) {
        WaitForSingleObject(g_threadHandle, INFINITE);
        CloseHandle(g_threadHandle);
    }
}

void WhTool_ModSettingsChanged() {
    LoadSettings();
}

////////////////////////////////////////////////////////////////////////////////
// Windhawk tool mod implementation for mods which don't need to inject to other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process
//
// The mod will load and run in a dedicated windhawk.exe process.
//
// Paste the code below as part of the mod code, and use these callbacks:
// * WhTool_ModInit
// * WhTool_ModSettingsChanged
// * WhTool_ModUninit
//
// Currently, other callbacks are not supported.

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    bool isExcluded = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0 ||
            wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isExcluded = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isExcluded) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
