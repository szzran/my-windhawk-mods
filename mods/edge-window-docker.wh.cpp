// ==WindhawkMod==
// @id              edge-window-docker
// @name            Edge Window Docker
// @description     Docks active window to nearest screen edges, auto-hide with slide animation.
// @version         0.1
// @author          szzran
// @github          https://github.com/szzran
// @include         *
// @compilerOptions -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Edge Window Docker
**Edge Window Docker** allows you to "park" active windows against any screen edge to declutter your workspace. Once docked, windows automatically slide out of view leaving only a thin visible sliver and reappear instantly when you hover over them.

![Demonstration](https://i.imgur.com/HCb8PEn.gif)

### 🚀 Key Features
- **Edge Docking:** Quickly dock the active window to the nearest screen edge using a customizable hotkey (Default: `Ctrl+Alt+Space`).
- **Smooth Animations:** Includes support for slide and fade transitions with "Linear" or "Ease-Out" (Windows-style) animation curves.
- **Intelligent Taskbar Awareness:** Actively avoids docking windows onto the edge where your native Windows Taskbar is located.
- **Visual Feedback:** Features a "bounce" indicator that triggers if you attempt to minimize a window while it is currently docked.
- **Always on Top:** Includes an option to force docked windows to stay above regular windows, ensuring they are always accessible.

### ⚙️ Configuration Highlights
- **Customizable "Sliver":** Adjust the **Visible Edge** (in pixels) to determine how much of the window remains visible while hidden.
- **Hover Delay:** Prevent accidental triggers by setting a delay (in ms) before the window slides out.
- **Transparency Control:** Set a custom opacity level for windows when they are in their hidden/docked state.

> ⚠️ **Experimental: Restore Original Size**
> 
> You can enable an experimental setting to restore windows to their pre-docked dimensions upon undocking. **Note:** This may cause visual stuttering in "heavy" applications; for the smoothest performance, it is recommended to keep this disabled.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- Hotkey: Ctrl+Alt+Space
  $name: Dock Toggle Hotkey
  $description: >-
    Format: Modifier+Key (e.g., Ctrl+Alt+Space).
    Modifiers: Alt, Ctrl, Shift, Win. Keys: A-Z, 0-9, F1-F24, Enter, Space, Home, End, Insert, Delete, etc.
- Gap: 10
  $name: Gap Size (px)
  $description: Distance in pixels between the docked window and the monitor edge.
- VisibleEdge: 15
  $name: Visible Edge (px)
  $description: The amount of the window (in pixels) that remains visible when it slides off-screen.
- DisableAnimations: false
  $name: Disable All Animations
  $description: >-
    Instantly snap windows into position. 
    This disables all sliding, fading, and the bounce effect.
- AnimStyle: ease
  $name: Animation Style
  $description: Choose how the window slides.
  $options:
    - linear: Linear (Constant speed)
    - ease: Ease-Out (Smooth deceleration, similar to Windows flyouts)
- Speed: 30
  $name: Animation Intensity
  $description: Higher values make the window move faster.
- HiddenAlpha: 200
  $name: Hidden Transparency (0-255)
  $description: The opacity of the window when it is docked and hidden. 0 is completely invisible, 255 is solid.
- HoverDelay: 300
  $name: Hover Delay (ms)
  $description: The time in milliseconds your mouse must hover over the visible edge before the window slides out.
- AlwaysOnTop: true
  $name: Always on Top
  $description: Forces the docked window to stay above other regular windows.
- TaskbarAware: true
  $name: Taskbar Aware
  $description: Prevent windows from docking to the edge where the native Windows Taskbar is located.
- RestoreSize: false
  $name: Restore Original Size (Experimental)
  $description: >-
    Restores the window to its pre-docked dimensions when undocking.
    Warning: Animating size changes can cause severe visual lag or stuttering in heavier apps.
    Disabling this keeps animations perfectly smooth.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h> 
#include <algorithm>
#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>
#include <mutex>

// ============================================================================
// Constants & Enums
// ============================================================================
const UINT_PTR DOCK_TIMER_ID = 13371337;
const UINT TIMER_INTERVAL_FAST = 10;   // ~100fps polling for smooth 60hz+ animations
const UINT TIMER_INTERVAL_SLOW = 100;  // Idle polling
const int BOUNCE_DURATION_MS = 400;
const int DRAG_PULL_BUFFER = 50;       // Pixels to pull before tearing off

enum Side { 
    LEFT = 0, 
    TOP = 1, 
    RIGHT = 2, 
    BOTTOM = 3 
};

#define WM_UPDATE_SETTINGS (WM_APP + 1)

// ============================================================================
// Global State & Structs
// ============================================================================
UINT WM_TOGGLE_DOCK = 0;
DWORD g_dwThreadId = 0;
std::recursive_mutex g_dockMutex;
HHOOK g_hKeyboardHook = NULL;

UINT g_hotkeyMod = 0;
UINT g_hotkeyVk = 0;

struct DockedWindow {
    HWND hwnd;
    Side side;
    RECT monitor;
    
    // Original state for restoration
    int originalX, originalY;
    int originalWidth, originalHeight;
    LONG_PTR originalExStyle;
    
    // Current dimensions
    int width, height;
    
    // Animation targets and current state
    int curX, curY, tarX, tarY;
    int curAlpha, tarAlpha;
    int lastSetX, lastSetY;
    
    // Status flags
    bool isHovered;
    bool initialDocking;
    bool pendingRemoval;
    bool isMoving;
    bool isTearingOff;
    bool isBouncing;
    bool isFastTimer;
    
    // Timers
    DWORD hoverStartTick;
    DWORD bounceStartTick;
};

std::unordered_map<HWND, DockedWindow> g_dockedWindows;

// Forward Declarations
VOID CALLBACK DockTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
LRESULT CALLBACK DockedWindowSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

// ============================================================================
// System Queries (Taskbar & Monitors)
// ============================================================================

HWND FindTaskbarForMonitor(HMONITOR targetMon) {
    HWND hwndMain = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hwndMain && MonitorFromWindow(hwndMain, MONITOR_DEFAULTTONEAREST) == targetMon) {
        return hwndMain;
    }
    
    HWND hwndSec = NULL;
    while ((hwndSec = FindWindowExW(NULL, hwndSec, L"Shell_SecondaryTrayWnd", NULL)) != NULL) {
        if (MonitorFromWindow(hwndSec, MONITOR_DEFAULTTONEAREST) == targetMon) {
            return hwndSec;
        }
    }
    return NULL;
}

int GetTaskbarEdge(HWND hwndTaskbar, RECT rcMonitor) {
    RECT rc;
    if (hwndTaskbar && GetWindowRect(hwndTaskbar, &rc)) {
        int cx = rc.left + (rc.right - rc.left) / 2;
        int cy = rc.top + (rc.bottom - rc.top) / 2;
        int mcx = rcMonitor.left + (rcMonitor.right - rcMonitor.left) / 2;
        int mcy = rcMonitor.top + (rcMonitor.bottom - rcMonitor.top) / 2;
        
        bool isHorizontal = (rc.right - rc.left) >= (rc.bottom - rc.top);
        if (isHorizontal) {
            return (cy > mcy) ? BOTTOM : TOP;
        } else {
            return (cx > mcx) ? RIGHT : LEFT;
        }
    }
    return -1;
}

struct MaxWinCheckData {
    HMONITOR hMon;
    HWND excludeHwnd;
    bool found;
};

BOOL CALLBACK EnumMaxWindowsProc(HWND hwnd, LPARAM lParam) {
    MaxWinCheckData* data = (MaxWinCheckData*)lParam;
    
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == data->excludeHwnd) {
        return TRUE;
    }

    WCHAR className[256];
    GetClassName(hwnd, className, 256);
    if (wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Progman") == 0) {
        return TRUE;
    }

    if (IsZoomed(hwnd)) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        if (mon == data->hMon) {
            data->found = true;
            return FALSE; 
        }
    }
    return TRUE;
}

bool HasMaximizedWindow(HMONITOR hMon, HWND excludeHwnd) {
    MaxWinCheckData data = { hMon, excludeHwnd, false };
    EnumWindows(EnumMaxWindowsProc, (LPARAM)&data);
    return data.found;
}

// ============================================================================
// Math & Positioning Logic
// ============================================================================

Side GetNearestSide(RECT wr, RECT mr, int forbiddenSide) {
    int dists[4];
    dists[LEFT] = std::abs(wr.left - mr.left);
    dists[TOP] = std::abs(wr.top - mr.top);
    dists[RIGHT] = std::abs(mr.right - wr.right);
    dists[BOTTOM] = std::abs(mr.bottom - wr.bottom);

    if (forbiddenSide >= LEFT && forbiddenSide <= BOTTOM) {
        dists[forbiddenSide] = 9999999; // Heavily penalize the taskbar side
    }

    Side bestSide = LEFT;
    int minDist = dists[LEFT];
    for (int i = 1; i <= BOTTOM; i++) {
        if (dists[i] < minDist) {
            minDist = dists[i];
            bestSide = (Side)i;
        }
    }
    return bestSide;
}

void GetPositions(DockedWindow& dw, int edge, int& hX, int& hY, int& vX, int& vY) {
    int gap = Wh_GetIntSetting(L"Gap");
    switch (dw.side) {
        case LEFT:
            hX = dw.monitor.left - dw.width + edge;
            vX = dw.monitor.left + gap; 
            hY = vY = dw.curY; 
            break;
        case RIGHT:
            hX = dw.monitor.right - edge;
            vX = dw.monitor.right - dw.width - gap; 
            hY = vY = dw.curY; 
            break;
        case TOP:
            hY = dw.monitor.top - dw.height + edge;
            vY = dw.monitor.top + gap; 
            hX = vX = dw.curX; 
            break;
        case BOTTOM:
            hY = dw.monitor.bottom - edge;
            vY = dw.monitor.bottom - dw.height - gap; 
            hX = vX = dw.curX; 
            break;
    }
}

// ============================================================================
// Core Docking & Animation Logic
// ============================================================================

void ToggleDockWindow(HWND hwnd) {
    if (!hwnd || hwnd == GetDesktopWindow() || hwnd == FindWindow(L"Shell_TrayWnd", NULL)) return;

    std::lock_guard<std::recursive_mutex> lock(g_dockMutex);

    // Undock if already docked
    if (g_dockedWindows.count(hwnd)) {
        DockedWindow& dw = g_dockedWindows[hwnd];
        if (dw.isMoving || dw.isTearingOff) return;
        
        dw.pendingRemoval = true;
        dw.tarX = dw.originalX; 
        dw.tarY = dw.originalY; 
        dw.tarAlpha = 255;
        
        if (!dw.isFastTimer) {
            SetTimer(hwnd, DOCK_TIMER_ID, TIMER_INTERVAL_FAST, DockTimerProc);
            dw.isFastTimer = true;
        }
        
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    // Docking Initialization
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};

    if (GetMonitorInfo(hMon, &mi)) {
        if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        RECT wr; GetWindowRect(hwnd, &wr);

        int forbiddenSide = -1;
        if (Wh_GetIntSetting(L"TaskbarAware")) {
            HWND hwndTaskbar = FindTaskbarForMonitor(hMon);
            if (hwndTaskbar) {
                forbiddenSide = GetTaskbarEdge(hwndTaskbar, mi.rcMonitor);
            }
        }

        DockedWindow dw = {};
        dw.hwnd = hwnd;
        dw.side = GetNearestSide(wr, mi.rcWork, forbiddenSide);
        dw.width = dw.originalWidth = wr.right - wr.left; 
        dw.height = dw.originalHeight = wr.bottom - wr.top; 
        dw.monitor = mi.rcWork;
        dw.originalX = wr.left; 
        dw.originalY = wr.top;
        dw.originalExStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        
        dw.curX = dw.lastSetX = wr.left; 
        dw.curY = dw.lastSetY = wr.top;
        dw.curAlpha = 255;
        dw.initialDocking = true;
        dw.isFastTimer = true;

        bool alwaysOnTop = Wh_GetIntSetting(L"AlwaysOnTop") != 0;
        int edgeSetting = Wh_GetIntSetting(L"VisibleEdge");
        int currentEdge = (!alwaysOnTop && HasMaximizedWindow(hMon, hwnd)) ? 0 : edgeSetting;

        int hX, hY, vX, vY;
        GetPositions(dw, currentEdge, hX, hY, vX, vY);
        
        dw.tarX = hX; 
        dw.tarY = hY;
        dw.tarAlpha = Wh_GetIntSetting(L"HiddenAlpha");
        
        HWND zOrder = alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, dw.originalExStyle | WS_EX_LAYERED);
        SetWindowPos(hwnd, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        SetWindowSubclass(hwnd, DockedWindowSubclassProc, 1, 0);
        
        g_dockedWindows[hwnd] = dw;
        SetTimer(hwnd, DOCK_TIMER_ID, TIMER_INTERVAL_FAST, DockTimerProc); 
    }
}

void UpdateDockedWindow(HWND hwnd) {
    std::lock_guard<std::recursive_mutex> lock(g_dockMutex); 
    if (!g_dockedWindows.count(hwnd)) { 
        KillTimer(hwnd, DOCK_TIMER_ID); 
        return; 
    } 

    DockedWindow& dw = g_dockedWindows[hwnd]; 
    POINT pt; GetCursorPos(&pt); 
    
    // Settings Fetch
    int animSpeed = Wh_GetIntSetting(L"Speed");
    bool disableAllAnim = Wh_GetIntSetting(L"DisableAnimations") != 0;
    int hAlpha = Wh_GetIntSetting(L"HiddenAlpha");
    int delay = Wh_GetIntSetting(L"HoverDelay"); 
    int gap = Wh_GetIntSetting(L"Gap");
    int pullBuffer = gap + DRAG_PULL_BUFFER; 
    
    PCWSTR animStyleRaw = Wh_GetStringSetting(L"AnimStyle"); 
    bool useEasing = (animStyleRaw && wcscmp(animStyleRaw, L"ease") == 0); 
    if (animStyleRaw) Wh_FreeStringSetting(animStyleRaw); 

    // Helper lambda for smooth transitions
    auto Animate = [&](int& cur, int tar, int step) -> bool { 
        if (cur == tar) return false;
        if (disableAllAnim) {
            cur = tar;
            return true;
        }
        if (useEasing) { 
            int diff = tar - cur; 
            float factor = (float)step / 200.0f; 
            factor = std::clamp(factor, 0.05f, 0.5f);
            int move = (int)(diff * factor); 
            if (move == 0) move = (diff > 0) ? 1 : -1; 
            cur += move; 
        } else { 
            if (cur < tar) cur = std::min(cur + step, tar); 
            else cur = std::max(cur - step, tar); 
        } 
        return true; 
    }; 

    // Handle unexpected minimization
    if (IsIconic(dw.hwnd)) { 
        ShowWindow(dw.hwnd, SW_RESTORE); 
        dw.isBouncing = true; 
        dw.bounceStartTick = GetTickCount(); 
    } 

    RECT act; GetWindowRect(dw.hwnd, &act); 
    bool externalDrag = (abs(act.left - dw.lastSetX) > 2 || abs(act.top - dw.lastSetY) > 2); 
    bool userIsDragging = dw.isMoving || externalDrag; 
    
    // ---------------------------------------------------------
    // State 1: Removal (Undocking)
    // ---------------------------------------------------------
    if (dw.pendingRemoval) { 
        bool restoreSize = Wh_GetIntSetting(L"RestoreSize") != 0;
        bool xMoved = Animate(dw.curX, dw.originalX, animSpeed);
        bool yMoved = Animate(dw.curY, dw.originalY, animSpeed); 
        bool alphaChanged = Animate(dw.curAlpha, 255, 20);

        bool widthChanged = false, heightChanged = false;
        if (restoreSize) {
            widthChanged = Animate(dw.width, dw.originalWidth, animSpeed);
            heightChanged = Animate(dw.height, dw.originalHeight, animSpeed);
        }

        if (xMoved || yMoved || widthChanged || heightChanged) { 
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            if (!restoreSize) flags |= SWP_NOSIZE;

            SetWindowPos(dw.hwnd, NULL, dw.curX, dw.curY, dw.width, dw.height, flags);
            dw.lastSetX = dw.curX; 
            dw.lastSetY = dw.curY; 
        } 
        
        if (alphaChanged) {
            SetLayeredWindowAttributes(dw.hwnd, 0, (BYTE)dw.curAlpha, LWA_ALPHA);
        }
        
        // Finalize removal once animation completes
        if (!xMoved && !yMoved && !alphaChanged && !widthChanged && !heightChanged) { 
            SetLayeredWindowAttributes(dw.hwnd, 0, 255, LWA_ALPHA);
            SetWindowLongPtr(dw.hwnd, GWL_EXSTYLE, dw.originalExStyle); 
            
            UINT finalFlags = SWP_FRAMECHANGED | SWP_NOACTIVATE;
            if (!restoreSize) finalFlags |= SWP_NOSIZE;

            SetWindowPos(dw.hwnd, HWND_NOTOPMOST, dw.originalX, dw.originalY, dw.originalWidth, dw.originalHeight, finalFlags); 
            
            KillTimer(hwnd, DOCK_TIMER_ID); 
            RemoveWindowSubclass(hwnd, DockedWindowSubclassProc, 1); 
            g_dockedWindows.erase(hwnd);
        } 
        return;
    }

    // ---------------------------------------------------------
    // State 2: Active Dragging (Tear-off Logic)
    // ---------------------------------------------------------
    if (userIsDragging) { 
        int dragDist = 0; 
        bool outOfRange = false; 
        
        if (dw.side == LEFT) { 
            dragDist = act.left - dw.monitor.left; 
        } else if (dw.side == RIGHT) { 
            dragDist = dw.monitor.right - act.right; 
        } else if (dw.side == TOP) { 
            dragDist = act.top - dw.monitor.top; 
        } else if (dw.side == BOTTOM) { 
            dragDist = dw.monitor.bottom - act.bottom; 
        } 
        
        if (dragDist > pullBuffer) outOfRange = true; 
        
        if (outOfRange) { 
            if (!dw.isTearingOff) {
                SetLayeredWindowAttributes(dw.hwnd, 0, 255, LWA_ALPHA);
                SetWindowLongPtr(dw.hwnd, GWL_EXSTYLE, dw.originalExStyle); 
                
                UINT finalFlags = SWP_NOMOVE | SWP_NOACTIVATE | SWP_FRAMECHANGED;
                if (Wh_GetIntSetting(L"RestoreSize") == 0) {
                    finalFlags |= SWP_NOSIZE;
                }
                SetWindowPos(dw.hwnd, HWND_NOTOPMOST, 0, 0, dw.originalWidth, dw.originalHeight, finalFlags);
                
                if (dw.isMoving) {
                    dw.isTearingOff = true;
                } else {
                    KillTimer(hwnd, DOCK_TIMER_ID); 
                    RemoveWindowSubclass(hwnd, DockedWindowSubclassProc, 1);
                    g_dockedWindows.erase(hwnd);
                }
            }
            return; 
        } else {
            // Apply visual tension based on drag distance
            float stretchFactor = std::clamp((float)dragDist / (float)pullBuffer, 0.0f, 1.0f); 
            int dynamicAlpha = hAlpha + (int)((255 - hAlpha) * stretchFactor); 
            SetLayeredWindowAttributes(dw.hwnd, 0, (BYTE)dynamicAlpha, LWA_ALPHA); 
            
            dw.curAlpha = dw.tarAlpha = dynamicAlpha; 
            dw.curX = dw.lastSetX = dw.tarX = act.left; 
            dw.curY = dw.lastSetY = dw.tarY = act.top; 
            
            dw.width = act.right - act.left;
            dw.height = act.bottom - act.top;

            dw.isHovered = true; 
            dw.hoverStartTick = 0; 
            dw.isBouncing = false; 
        } 
    } 
    // ---------------------------------------------------------
    // State 3: Standard Docked & Hover Polling
    // ---------------------------------------------------------
    else { 
        int edge = Wh_GetIntSetting(L"VisibleEdge"); 
        bool alwaysOnTop = Wh_GetIntSetting(L"AlwaysOnTop") != 0; 
        
        if (!alwaysOnTop) { 
            HMONITOR hMon = MonitorFromWindow(dw.hwnd, MONITOR_DEFAULTTONEAREST); 
            if (HasMaximizedWindow(hMon, dw.hwnd)) { 
                edge = 0; 
            } 
        } 

        int hX, hY, vX, vY; 
        GetPositions(dw, edge, hX, hY, vX, vY); 

        int hitEdge = (edge > 0) ? edge : 1; 
        RECT hb; 
        
        if (!dw.isHovered) { 
            if (dw.side == LEFT)       hb = { dw.monitor.left, dw.curY, dw.monitor.left + hitEdge, dw.curY + dw.height }; 
            else if (dw.side == RIGHT) hb = { dw.monitor.right - hitEdge, dw.curY, dw.monitor.right, dw.curY + dw.height }; 
            else if (dw.side == TOP)   hb = { dw.curX, dw.monitor.top, dw.curX + dw.width, dw.monitor.top + hitEdge }; 
            else                       hb = { dw.curX, dw.monitor.bottom - hitEdge, dw.curX + dw.width, dw.monitor.bottom }; 
        } else { 
            hb = { dw.curX - 20, dw.curY - 20, dw.curX + dw.width + 20, dw.curY + dw.height + 20 }; 
        } 

        if (!dw.initialDocking) { 
            if (PtInRect(&hb, pt)) { 
                if (!dw.isHovered) { 
                    if (dw.hoverStartTick == 0) dw.hoverStartTick = GetTickCount(); 
                    if ((GetTickCount() - dw.hoverStartTick) >= (DWORD)delay) { 
                        dw.isHovered = true; 
                        dw.isBouncing = false; 
                        
                        if (!alwaysOnTop) {
                            SetWindowPos(dw.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                        }
                    } 
                } 
            } else { 
                if (dw.isHovered && !alwaysOnTop) {
                    SetWindowPos(dw.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                dw.isHovered = false; 
                dw.hoverStartTick = 0; 
            } 
            
            dw.tarX = dw.isHovered ? vX : hX; 
            dw.tarY = dw.isHovered ? vY : hY; 
            dw.tarAlpha = dw.isHovered ? 255 : hAlpha; 

            if (dw.isBouncing) {
                if (disableAllAnim) {
                    dw.isBouncing = false;
                } else {
                    DWORD elapsed = GetTickCount() - dw.bounceStartTick;
                    if (elapsed > BOUNCE_DURATION_MS) {
                        dw.isBouncing = false; 
                    } else if (!dw.isHovered) { 
                        int peekAmount = 50;
                        int currentPeek = (int)(peekAmount * std::sin(((float)elapsed / BOUNCE_DURATION_MS) * 3.14159265f)); 
                        
                        if (dw.side == LEFT) dw.tarX = hX + currentPeek; 
                        else if (dw.side == RIGHT) dw.tarX = hX - currentPeek; 
                        else if (dw.side == TOP) dw.tarY = hY + currentPeek; 
                        else if (dw.side == BOTTOM) dw.tarY = hY - currentPeek; 
                        
                        dw.tarAlpha = 255; 
                    }
                }
            } 
        } 

        // Apply visual updates
        bool xMoved = Animate(dw.curX, dw.tarX, animSpeed); 
        bool yMoved = Animate(dw.curY, dw.tarY, animSpeed); 
        bool alphaChanged = Animate(dw.curAlpha, dw.tarAlpha, 20); 
        
        if (dw.initialDocking && !xMoved && !yMoved) {
            dw.initialDocking = false; 
        }

        if (xMoved || yMoved) { 
            SetWindowPos(dw.hwnd, NULL, dw.curX, dw.curY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); 
            dw.lastSetX = dw.curX; 
            dw.lastSetY = dw.curY; 
        } 
        if (alphaChanged) {
            SetLayeredWindowAttributes(dw.hwnd, 0, (BYTE)dw.curAlpha, LWA_ALPHA); 
        }

        // Throttle timer depending on activity
        bool isAnimating = (dw.curX != dw.tarX || dw.curY != dw.tarY || dw.curAlpha != dw.tarAlpha); 
        bool needsFastTimer = (isAnimating || dw.isHovered || dw.initialDocking || dw.isBouncing || PtInRect(&hb, pt)); 
        
        if (needsFastTimer && !dw.isFastTimer) { 
            SetTimer(dw.hwnd, DOCK_TIMER_ID, TIMER_INTERVAL_FAST, DockTimerProc); 
            dw.isFastTimer = true; 
        } else if (!needsFastTimer && dw.isFastTimer) { 
            SetTimer(dw.hwnd, DOCK_TIMER_ID, TIMER_INTERVAL_SLOW, DockTimerProc); 
            dw.isFastTimer = false; 
        } 
    } 
}

VOID CALLBACK DockTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    if (idEvent == DOCK_TIMER_ID) {
        UpdateDockedWindow(hwnd);
    }
}

// ============================================================================
// Window Procedure Interception
// ============================================================================

LRESULT CALLBACK DockedWindowSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_MINIMIZE) {
        std::lock_guard<std::recursive_mutex> lock(g_dockMutex);
        if (g_dockedWindows.count(hWnd)) {
            g_dockedWindows[hWnd].isBouncing = true; 
            g_dockedWindows[hWnd].bounceStartTick = GetTickCount();
            return 0; // Prevent actual minimization
        }
    }
    
    if (uMsg == WM_MOVING) {
        std::lock_guard<std::recursive_mutex> lock(g_dockMutex);
        if (g_dockedWindows.count(hWnd)) {
            DockedWindow& dw = g_dockedWindows[hWnd];
            if (dw.isTearingOff && Wh_GetIntSetting(L"RestoreSize") != 0) {
                LPRECT prc = (LPRECT)lParam;
                prc->right = prc->left + dw.originalWidth;
                prc->bottom = prc->top + dw.originalHeight;
                return TRUE;
            }
        }
    }
    
    if (uMsg == WM_ENTERSIZEMOVE) {
        std::lock_guard<std::recursive_mutex> lock(g_dockMutex);
        if (g_dockedWindows.count(hWnd)) g_dockedWindows[hWnd].isMoving = true;
    }
    
    if (uMsg == WM_EXITSIZEMOVE) {
        std::lock_guard<std::recursive_mutex> lock(g_dockMutex);
        if (g_dockedWindows.count(hWnd)) {
            DockedWindow& dw = g_dockedWindows[hWnd];

            if (dw.isTearingOff) {
                KillTimer(hWnd, DOCK_TIMER_ID); 
                RemoveWindowSubclass(hWnd, DockedWindowSubclassProc, 1);
                g_dockedWindows.erase(hWnd);
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            }

            dw.isMoving = false; 
            RECT r; GetWindowRect(dw.hwnd, &r);
            
            dw.curX = dw.lastSetX = r.left; 
            dw.curY = dw.lastSetY = r.top; 
            dw.width = r.right - r.left;
            dw.height = r.bottom - r.top;
        }
    }
    
    if (uMsg == WM_DESTROY) {
        std::lock_guard<std::recursive_mutex> lock(g_dockMutex);
        g_dockedWindows.erase(hWnd); 
        RemoveWindowSubclass(hWnd, DockedWindowSubclassProc, 1);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
    
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// Hotkey Parsing & Keyboard Hooks
// ============================================================================

void TriggerDockActiveWindow() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || hwnd == GetDesktopWindow() || hwnd == FindWindow(L"Shell_TrayWnd", NULL)) return;
    PostMessage(hwnd, WM_TOGGLE_DOCK, 0, 0);
}

void ParseHotkey(PCWSTR hotkeyStr) {
    g_hotkeyMod = 0; g_hotkeyVk = 0;
    if (!hotkeyStr || wcslen(hotkeyStr) == 0) return;
    
    std::wstring s(hotkeyStr);
    size_t start = 0, end = s.find(L'+');
    std::vector<std::wstring> parts;
    
    while (end != std::wstring::npos) {
        parts.push_back(s.substr(start, end - start));
        start = end + 1; 
        end = s.find(L'+', start);
    }
    parts.push_back(s.substr(start));
    
    for (size_t i = 0; i < parts.size(); ++i) {
        std::wstring part = parts[i];
        part.erase(0, part.find_first_not_of(L" \t"));
        part.erase(part.find_last_not_of(L" \t") + 1);
        std::transform(part.begin(), part.end(), part.begin(), towupper);
        
        if (part == L"CTRL" || part == L"CONTROL") g_hotkeyMod |= MOD_CONTROL;
        else if (part == L"ALT") g_hotkeyMod |= MOD_ALT;
        else if (part == L"SHIFT") g_hotkeyMod |= MOD_SHIFT;
        else if (part == L"WIN" || part == L"WINDOWS") g_hotkeyMod |= MOD_WIN;
        else {
            if (part == L"SPACE") g_hotkeyVk = VK_SPACE;
            else if (part == L"ENTER" || part == L"RETURN") g_hotkeyVk = VK_RETURN;
            else if (part == L"HOME") g_hotkeyVk = VK_HOME;
            else if (part == L"END") g_hotkeyVk = VK_END;
            else if (part == L"INSERT") g_hotkeyVk = VK_INSERT;
            else if (part == L"DELETE") g_hotkeyVk = VK_DELETE;
            else if (part == L"TAB") g_hotkeyVk = VK_TAB;
            else if (part == L"ESCAPE" || part == L"ESC") g_hotkeyVk = VK_ESCAPE;
            else if (part.length() == 1) g_hotkeyVk = part[0];
            else if (part[0] == L'F' && part.length() > 1) {
                int fKey = _wtoi(part.c_str() + 1);
                if (fKey >= 1 && fKey <= 24) g_hotkeyVk = VK_F1 + fKey - 1;
            }
        }
    }
}

LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* pKbd = (KBDLLHOOKSTRUCT*)lParam;
        
        if (g_hotkeyVk != 0 && pKbd->vkCode == g_hotkeyVk) {
            bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
            bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
            bool shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
            bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
            
            bool matchCtrl = (g_hotkeyMod & MOD_CONTROL) != 0;
            bool matchAlt = (g_hotkeyMod & MOD_ALT) != 0;
            bool matchShift = (g_hotkeyMod & MOD_SHIFT) != 0;
            bool matchWin = (g_hotkeyMod & MOD_WIN) != 0;
            
            if (ctrl == matchCtrl && alt == matchAlt && shift == matchShift && win == matchWin) {
                TriggerDockActiveWindow();
                return 1; // Block default execution
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

DWORD WINAPI ThreadProc(LPVOID) {
    auto applySettings = [&]() {
        PCWSTR s = Wh_GetStringSetting(L"Hotkey");
        if (s) { 
            ParseHotkey(s); 
            Wh_FreeStringSetting(s); 
        }
    };

    applySettings();
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookProc, NULL, 0);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_UPDATE_SETTINGS) applySettings();
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    if (g_hKeyboardHook) UnhookWindowsHookEx(g_hKeyboardHook);
    return 0;
}

// ============================================================================
// Windhawk Message Routing & Initialization
// ============================================================================

using DispatchMessageW_t = LRESULT(WINAPI*)(const MSG*);
DispatchMessageW_t DispatchMessageW_Orig;

LRESULT WINAPI DispatchMessageW_Hook(const MSG* lpMsg) {
    if (lpMsg && lpMsg->message == WM_TOGGLE_DOCK && WM_TOGGLE_DOCK != 0) {
        ToggleDockWindow(lpMsg->hwnd);
        return 0;
    }
    return DispatchMessageW_Orig(lpMsg);
}

using DispatchMessageA_t = LRESULT(WINAPI*)(const MSG*);
DispatchMessageA_t DispatchMessageA_Orig;

LRESULT WINAPI DispatchMessageA_Hook(const MSG* lpMsg) {
    if (lpMsg && lpMsg->message == WM_TOGGLE_DOCK && WM_TOGGLE_DOCK != 0) {
        ToggleDockWindow(lpMsg->hwnd);
        return 0;
    }
    return DispatchMessageA_Orig(lpMsg);
}

void AllowMessageBypass(HWND hwnd) {
    if (WM_TOGGLE_DOCK != 0) {
        ChangeWindowMessageFilterEx(hwnd, WM_TOGGLE_DOCK, MSGFLT_ALLOW, NULL);
    }
}

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExW_t CreateWindowExW_Orig;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {
    HWND hwnd = CreateWindowExW_Orig(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    if (hwnd) AllowMessageBypass(hwnd);
    return hwnd;
}

void Wh_ModSettingsChanged() {
    if (g_dwThreadId != 0) PostThreadMessage(g_dwThreadId, WM_UPDATE_SETTINGS, 0, 0);
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) AllowMessageBypass(hwnd);
    return TRUE;
}

BOOL Wh_ModInit() {
    WM_TOGGLE_DOCK = RegisterWindowMessage(L"Windhawk_Edge_Docker_Toggle");
    
    Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CreateWindowExW_Hook, (void**)&CreateWindowExW_Orig);
    Wh_SetFunctionHook((void*)DispatchMessageW, (void*)DispatchMessageW_Hook, (void**)&DispatchMessageW_Orig);
    Wh_SetFunctionHook((void*)DispatchMessageA, (void*)DispatchMessageA_Hook, (void**)&DispatchMessageA_Orig);
    
    EnumWindows(EnumWindowsProc, 0);

    WCHAR exeName[MAX_PATH];
    if (GetModuleFileName(NULL, exeName, MAX_PATH)) {
        for (int i = 0; exeName[i]; i++) {
            exeName[i] = towlower(exeName[i]);
        }
        if (wcsstr(exeName, L"explorer.exe")) {
            g_dwThreadId = 0;
            HANDLE hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, &g_dwThreadId);
            if (hThread) {
                CloseHandle(hThread);
            }
        }
    }

    return TRUE;
}

void Wh_ModUninit() {
    if (g_dwThreadId != 0) PostThreadMessage(g_dwThreadId, WM_QUIT, 0, 0);
    std::lock_guard<std::recursive_mutex> lock(g_dockMutex);
    
    for (auto& pair : g_dockedWindows) {
        DockedWindow& dw = pair.second;
        SetLayeredWindowAttributes(dw.hwnd, 0, 255, LWA_ALPHA);
        SetWindowLongPtr(dw.hwnd, GWL_EXSTYLE, dw.originalExStyle);
        
        UINT finalFlags = SWP_FRAMECHANGED;
        if (Wh_GetIntSetting(L"RestoreSize") == 0) finalFlags |= SWP_NOSIZE;

        SetWindowPos(dw.hwnd, HWND_NOTOPMOST, dw.originalX, dw.originalY, dw.originalWidth, dw.originalHeight, finalFlags);
        
        KillTimer(dw.hwnd, DOCK_TIMER_ID);
        RemoveWindowSubclass(dw.hwnd, DockedWindowSubclassProc, 1);
    }
    g_dockedWindows.clear();
}