#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>    // GET_X_LPARAM / GET_Y_LPARAM
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <vector>
#include "Resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------------
// 컴파일-타임 상수
// -----------------------------------------------------------------------------
constexpr int   CLIENT_W_PX = 440;  // physical pixels (no DPI scaling)
constexpr int   CLIENT_H_PX = 200;
constexpr float INTERNAL_DPI = 120.0f;  // assume 150 % when converting Px↔︎DIP
constexpr float SCALE_PX_TO_DIP = 96.0f / INTERNAL_DPI;  // ≃ 0.6666…
constexpr float SCALE_DIP_TO_PX = INTERNAL_DPI / 96.0f;  // = 1.5

constexpr float MARGIN_DIP = 20.0f;
constexpr float LINE_HEIGHT_DIP = 32.0f;
constexpr float LINK_SPACING_DIP = 10.0f;

// -----------------------------------------------------------------------------
// 글로벌
// -----------------------------------------------------------------------------
static HWND                          g_hWnd = nullptr;
static ComPtr<ID2D1Factory>          g_d2Factory;
static ComPtr<ID2D1HwndRenderTarget> g_rt;
static ComPtr<ID2D1SolidColorBrush>  g_brAccent;
static ComPtr<ID2D1SolidColorBrush>  g_brGray;
static ComPtr<ID2D1SolidColorBrush>  g_brText;
static ComPtr<IDWriteFactory>        g_dwFactory;
static ComPtr<IDWriteTextFormat>     g_tfLeft;
static ComPtr<IDWriteTextFormat>     g_tfCenter;

static D2D1_RECT_F                   g_linkRectDip = {};

static int                           g_step = 0;
static constexpr UINT                WM_STEPCHANGE = WM_USER + 1;

// 단계별 텍스트
static const wchar_t* g_statusText[] = {
    L"1) 카카오톡 프로세스 찾는 중...",
    L"2) 광고 제거 중...",
    L"3) 완료! 5초 후 종료됩니다."
};

// -----------------------------------------------------------------------------
// 프로세스 명으로 PID 찾기
// -----------------------------------------------------------------------------
static DWORD FindProcessID(const std::wstring& name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, name.c_str())) {
                CloseHandle(snap);
                return pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
}

// -----------------------------------------------------------------------------
// 광고 제거 로직
// -----------------------------------------------------------------------------
struct AdHiderContext {
    std::vector<std::pair<HWND, HWND>> candidates;
    HWND mainParent = nullptr;
    RECT winRect = {};
};

// 자식 윈도우를 열거하면서 광고 컨트롤 탐색 / 숨김
BOOL CALLBACK EnumChildProc(HWND hChild, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AdHiderContext*>(lParam);
    RECT& rect = ctx->winRect;

    wchar_t cls[128] = {};
    wchar_t txt[256] = {};
    GetClassNameW(hChild, cls, _countof(cls));
    GetWindowTextW(hChild, txt, _countof(txt));
    HWND parent = GetParent(hChild);

    // EVA_ChildWindow 후보 수집
    if (wcscmp(cls, L"EVA_ChildWindow") == 0) {
        if (wcslen(txt) == 0) {
            ctx->candidates.emplace_back(hChild, parent);
        }
        else if (wcsncmp(txt, L"OnlineMainView", 14) == 0) {
            ctx->mainParent = parent;
        }
    }

    // 구버전 배너
    if (wcscmp(cls, L"BannerAdWnd") == 0 ||
        wcscmp(cls, L"BannerAdContainer") == 0)
    {
        HWND tgt = (wcscmp(cls, L"BannerAdContainer") == 0 ? parent : hChild);
        ShowWindow(tgt, SW_HIDE);
        MoveWindow(tgt, 0, 0, 0, 0, TRUE);
    }

    // 락스크린 광고 영역
    if (wcsncmp(txt, L"LockModeView", 12) == 0) {
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        SetWindowPos(hChild, nullptr, 0, 0, w, h, SWP_NOMOVE);
    }

    // 메인뷰 광고 영역
    if (wcsncmp(txt, L"OnlineMainView", 14) == 0) {
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top - 31; // 패딩
        if (h > 0) {
            SetWindowPos(hChild, nullptr, 0, 0, w, h, SWP_NOMOVE);
        }
    }

    // 팝업 광고
    if (wcscmp(cls, L"AdFitWebView") == 0) {
        SendMessageW(parent, WM_CLOSE, 0, 0);
        ShowWindow(parent, SW_HIDE);
        MoveWindow(parent, 0, 0, 0, 0, TRUE);
        SendMessageW(hChild, WM_CLOSE, 0, 0);
        ShowWindow(hChild, SW_HIDE);
        MoveWindow(hChild, 0, 0, 0, 0, TRUE);
    }

    return TRUE;
}

// 광고 숨기기 로직
void HideAdsInWindow(HWND hTopWnd) {
    RECT rect;
    GetWindowRect(hTopWnd, &rect);

    // 컨텍스트 생성
    AdHiderContext* ctx = new AdHiderContext();
    ctx->winRect = rect;

    // 자식 윈도우 전부 열거
    EnumChildWindows(hTopWnd, EnumChildProc, (LPARAM)ctx);

    // EVA_ChildWindow 숨김 처리 (실제 메인뷰 자식만)
    if (ctx->mainParent && !ctx->candidates.empty()) {
        for (auto& pr : ctx->candidates) {
            if (pr.second == ctx->mainParent) {
                ShowWindow(pr.first, SW_HIDE);
                MoveWindow(pr.first, 0, 0, 0, 0, TRUE);
                break;
            }
        }
    }

    delete ctx;
}

// 특정 PID에 대해 최상위 윈도우만 열거
void DoAdRemovalOnce(DWORD pid) {
    EnumWindows([](HWND hWnd, LPARAM lParam)->BOOL {
        DWORD wp;
        GetWindowThreadProcessId(hWnd, &wp);
        if (wp == (DWORD)lParam) {
            HideAdsInWindow(hWnd);
        }
        return TRUE;
        }, (LPARAM)pid);
}

// -----------------------------------------------------------------------------
// Worker thread – 광고 제거 플로우
// -----------------------------------------------------------------------------
static void PostStep(int s) { PostMessageW(g_hWnd, WM_STEPCHANGE, s, 0); }

static DWORD WINAPI Worker(LPVOID)
{
    PostStep(1);
    DWORD pid = FindProcessID(L"KakaoTalk.exe");

    PostStep(2);
    if (pid) DoAdRemovalOnce(pid);

    PostStep(3);
    Sleep(5000);
    PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
    return 0;
}

// -----------------------------------------------------------------------------
// Direct2D 헬퍼
// -----------------------------------------------------------------------------
static void EnsureRenderTarget()
{
    if (g_rt) return;

    // 1) current client pixel size
    RECT rc; GetClientRect(g_hWnd, &rc);
    D2D1_SIZE_U szPx = { UINT(rc.right - rc.left), UINT(rc.bottom - rc.top) };

    // 2) create RT fixed at INTERNAL_DPI
    D2D1_RENDER_TARGET_PROPERTIES rtp =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
            INTERNAL_DPI, INTERNAL_DPI);

    g_d2Factory->CreateHwndRenderTarget(rtp,
        D2D1::HwndRenderTargetProperties(g_hWnd, szPx), &g_rt);

    // brushes
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x0078D4), &g_brAccent);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray, 0.6f), &g_brGray);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &g_brText);
}

static void ClearWithSysControlColor(ID2D1RenderTarget* rt)
{
    // ① 시스템 색 가져오기 = SystemColors.Control
    COLORREF cr = GetSysColor(COLOR_BTNFACE);   // 0x00bbggrr 형식
    const float r = GetRValue(cr) / 255.0f;
    const float g = GetGValue(cr) / 255.0f;
    const float b = GetBValue(cr) / 255.0f;

    // ② Direct2D 색 구조로 변환하여 화면 지우기
    rt->Clear(D2D1::ColorF(r, g, b, 1.0f));
}

static void OnPaint()
{
    PAINTSTRUCT ps; BeginPaint(g_hWnd, &ps);
    EnsureRenderTarget();

    g_rt->BeginDraw();
    //g_rt->Clear(D2D1::ColorF(D2D1::ColorF::White));
    ClearWithSysControlColor(g_rt.Get());

    // 전체 크기 (DIP)
    D2D1_SIZE_F szDip = {
        (float)CLIENT_W_PX * SCALE_PX_TO_DIP,
        (float)CLIENT_H_PX * SCALE_PX_TO_DIP };

    for (int i = 0; i < 3; ++i) {
        // 상태 텍스트
        D2D1_RECT_F rcText = D2D1::RectF(
            MARGIN_DIP,
            MARGIN_DIP + i * LINE_HEIGHT_DIP,
            szDip.width - MARGIN_DIP,
            MARGIN_DIP + (i + 1) * LINE_HEIGHT_DIP);

        g_rt->DrawTextW(g_statusText[i], (UINT32)wcslen(g_statusText[i]),
            g_tfLeft.Get(), rcText, g_brText.Get());

        // 체크박스
        bool done = (g_step > i);
        std::wstring box = L"("; box += (done ? L"✓" : L" "); box += L")";
        D2D1_RECT_F rcIcon = D2D1::RectF(
            szDip.width - MARGIN_DIP - 40.0f,
            rcText.top,
            szDip.width - MARGIN_DIP,
            rcText.bottom);
        g_rt->DrawTextW(box.c_str(), (UINT32)box.size(),
            g_tfLeft.Get(), rcIcon, done ? g_brAccent.Get() : g_brGray.Get());
    }

    // GitHub 링크 (step ≥ 3)
    if (g_step >= 3) {
        float y = MARGIN_DIP + 3 * LINE_HEIGHT_DIP + LINK_SPACING_DIP;
        g_linkRectDip = D2D1::RectF(0, y, szDip.width, y + LINE_HEIGHT_DIP);
        //g_rt->DrawTextW(L"GitHub", 6, g_tfCenter.Get(),
        //    g_linkRectDip, g_brAccent.Get());
        ComPtr<IDWriteTextLayout> textLayout;
        g_dwFactory->CreateTextLayout(L"GitHub", 6, g_tfCenter.Get(),
            g_linkRectDip.right - g_linkRectDip.left,
            g_linkRectDip.bottom - g_linkRectDip.top,
            &textLayout);
        textLayout->SetUnderline(TRUE, { 0, 6 });
        g_rt->DrawTextLayout(D2D1::Point2F(g_linkRectDip.left, g_linkRectDip.top),
            textLayout.Get(), g_brAccent.Get());
    }

    g_rt->EndDraw();
    EndPaint(g_hWnd, &ps);
}

// -----------------------------------------------------------------------------
// 윈도우 프로시저
// -----------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        // Direct2D/Write factories
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2Factory.ReleaseAndGetAddressOf());

        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)&g_dwFactory);

        // Text formats
        g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &g_tfLeft);
        g_tfLeft->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_tfLeft->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &g_tfCenter);
        g_tfCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tfCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        // rounded corners (Win11)
        const DWORD pref = 2; // DWM_WINDOW_CORNER_PREFERENCE_ROUND
        DwmSetWindowAttribute(wnd, 33, &pref, sizeof(pref));

        g_hWnd = wnd;
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        return 0;
    }

    case WM_DPICHANGED:
    {
        // 1) 전체 창 위치/크기 재배치
        RECT* prcWindow = reinterpret_cast<RECT*>(lp);
        SetWindowPos(
            wnd,
            nullptr,
            prcWindow->left, prcWindow->top,
            prcWindow->right - prcWindow->left,
            prcWindow->bottom - prcWindow->top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
        );

        // 2) 새 DPI 값 가져오기
        UINT dpiX = LOWORD(wp);   // X축 DPI
        UINT dpiY = HIWORD(wp);   // Y축 DPI

        // 3) 클라이언트 영역 크기 구하고 렌더 타겟 갱신
        RECT rcClient;
        GetClientRect(wnd, &rcClient);
        UINT  width = rcClient.right - rcClient.left;
        UINT  height = rcClient.bottom - rcClient.top;

        if (g_rt) {
            // DPI 먼저 설정
            g_rt->SetDpi((FLOAT)dpiX, (FLOAT)dpiY);
            // 클라이언트 픽셀 크기로 리사이즈
            g_rt->Resize(D2D1::SizeU(width, height));
        }

        // 4) 강제 리페인트
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    }

    case WM_STEPCHANGE:
        g_step = (int)wp;
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        if (g_step < 3) break;
        int px = GET_X_LPARAM(lp);
        int py = GET_Y_LPARAM(lp);
        float dipX = px * SCALE_PX_TO_DIP;
        float dipY = py * SCALE_PX_TO_DIP;

        bool over = (dipX >= g_linkRectDip.left && dipX <= g_linkRectDip.right &&
            dipY >= g_linkRectDip.top && dipY <= g_linkRectDip.bottom);
        SetCursor(LoadCursor(nullptr, over ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_step < 3) break;
        int px = GET_X_LPARAM(lp);
        int py = GET_Y_LPARAM(lp);
        float dipX = px * SCALE_PX_TO_DIP;
        float dipY = py * SCALE_PX_TO_DIP;
        if (dipX >= g_linkRectDip.left && dipX <= g_linkRectDip.right &&
            dipY >= g_linkRectDip.top && dipY <= g_linkRectDip.bottom)
        {
            ShellExecuteW(nullptr, L"open",
                L"https://github.com/HiSkyZen/KakaoTalkDelAD",
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WM_SIZE:
        if (g_rt) {
            RECT rc; GetClientRect(wnd, &rc);
            g_rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        }
        return 0;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
// 프로그램 시작점
// -----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    // make the process per‑monitor‑aware (optional – won’t react to DPI changes)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_SMALL));
    wc.lpszClassName = L"KakaoTalkDelAD";

    RegisterClassW(&wc);

    // adjust for window chrome once (no DPI math)
    RECT wr = { 0, 0, CLIENT_W_PX, CLIENT_H_PX };
    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    AdjustWindowRect(&wr, style, FALSE);

    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    HWND wnd = CreateWindowW(wc.lpszClassName, L"KakaoTalkDelAD",
        style, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(wnd, nShow);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
