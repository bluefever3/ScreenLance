#include "../Utils/pch.h"
#include "SelectionWindow.h"
#include "../Utils/Logger.h"
#include <cstdio>

// WM_*BUTTONDOWN/UP/MOVE lParam'ından imzalı (negatif olabilen) koordinat
// çıkarmak için: <windowsx.h>'a bağımlı kalmadan kendi yardımcımız.
static inline int XFromLParam(LPARAM lp) { return static_cast<int>(static_cast<short>(LOWORD(lp))); }
static inline int YFromLParam(LPARAM lp) { return static_cast<int>(static_cast<short>(HIWORD(lp))); }

SelectionWindow::SelectionWindow(HINSTANCE hInstance) : m_hInstance(hInstance) {}
SelectionWindow::~SelectionWindow() { if (m_hwnd) ::DestroyWindow(m_hwnd); }

RECT SelectionWindow::Show()
{
    // Sanal masaüstü sınırları – negatif koordinatlı ikincil monitörler dahil.
    m_virtualRect.left   = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualRect.top    = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_virtualRect.right  = m_virtualRect.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_virtualRect.bottom = m_virtualRect.top  + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_CROSS);
    ::RegisterClassExW(&wc); // İkinci çağrıda zaten kayıtlı olur, zararsız.

    int w = m_virtualRect.right  - m_virtualRect.left;
    int h = m_virtualRect.bottom - m_virtualRect.top;

    m_hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"ScreenLance – Alan Seç",
        WS_POPUP,
        m_virtualRect.left, m_virtualRect.top, w, h,
        nullptr, nullptr, m_hInstance, this);

    if (!m_hwnd)
    {
        Logger::Error("SelectionWindow: CreateWindow başarısız.");
        m_cancelled = true;
        return RECT{};
    }

    // Ekranı hafifçe karart – ne seçtiğini görsün ama altındaki içerik
    // hâlâ okunabilir kalsın.
    ::SetLayeredWindowAttributes(m_hwnd, 0, 70, LWA_ALPHA);

    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd);

    // Modal benzeri mesaj döngüsü: WM_DESTROY -> PostQuitMessage çağrılana
    // kadar bloke olur. Bu, Application'ın ana döngüsünü etkilemez çünkü
    // bu döngü tamamen bu fonksiyonun içinde kalır.
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    m_hwnd = nullptr; // OnMouseUp/OnKeyDown içinde zaten DestroyWindow edildi.
    return m_cancelled ? RECT{} : m_result;
}

void SelectionWindow::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC dc = ::BeginPaint(hwnd, &ps);

    RECT client{};
    ::GetClientRect(hwnd, &client);

    HBRUSH bg = ::CreateSolidBrush(RGB(10, 10, 10));
    ::FillRect(dc, &client, bg);
    ::DeleteObject(bg);

    if (m_dragging)
    {
        RECT sel{
            std::min(m_startPt.x, m_curPt.x),
            std::min(m_startPt.y, m_curPt.y),
            std::max(m_startPt.x, m_curPt.x),
            std::max(m_startPt.y, m_curPt.y)
        };

        HPEN   pen      = ::CreatePen(PS_SOLID, 2, RGB(0, 200, 255));
        HPEN   oldPen   = static_cast<HPEN>(::SelectObject(dc, pen));
        HBRUSH oldBrush = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
        ::Rectangle(dc, sel.left, sel.top, sel.right, sel.bottom);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);

        wchar_t dims[64]{};
        swprintf_s(dims, L"%ld x %ld", sel.right - sel.left, sel.bottom - sel.top);
        ::SetTextColor(dc, RGB(0, 200, 255));
        ::SetBkMode(dc, TRANSPARENT);
        ::TextOutW(dc, sel.left + 4, std::max(0L, sel.top - 20), dims, static_cast<int>(wcslen(dims)));
    }
    else
    {
        const wchar_t* hint = L"Çevrilecek alanı sürükleyerek seçin  •  İptal: ESC";
        ::SetTextColor(dc, RGB(230, 230, 230));
        ::SetBkMode(dc, TRANSPARENT);
        ::TextOutW(dc, 24, 24, hint, static_cast<int>(wcslen(hint)));
    }

    ::EndPaint(hwnd, &ps);
}

void SelectionWindow::OnMouseDown(POINT clientPt)
{
    m_dragging = true;
    m_startPt  = clientPt;
    m_curPt    = clientPt;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SelectionWindow::OnMouseMove(POINT clientPt)
{
    if (!m_dragging) return;
    m_curPt = clientPt;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SelectionWindow::OnMouseUp(POINT clientPt)
{
    if (!m_dragging) return;
    m_dragging = false;
    m_curPt = clientPt;

    // İstemci koordinatından sanal masaüstü (ekran) koordinatına çevir.
    RECT sel{
        std::min(m_startPt.x, m_curPt.x) + m_virtualRect.left,
        std::min(m_startPt.y, m_curPt.y) + m_virtualRect.top,
        std::max(m_startPt.x, m_curPt.x) + m_virtualRect.left,
        std::max(m_startPt.y, m_curPt.y) + m_virtualRect.top
    };

    // Yanlışlıkla tek tıklamayı (gerçek sürükleme olmayan) iptal say.
    if ((sel.right - sel.left) < 10 || (sel.bottom - sel.top) < 10)
        m_cancelled = true;
    else
        m_result = sel;

    ::DestroyWindow(m_hwnd); // WM_DESTROY -> PostQuitMessage(0)
}

void SelectionWindow::OnKeyDown(WPARAM vk)
{
    if (vk == VK_ESCAPE)
    {
        m_cancelled = true;
        ::DestroyWindow(m_hwnd);
    }
}

LRESULT CALLBACK SelectionWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    auto* self = reinterpret_cast<SelectionWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
    case WM_PAINT:
        self->OnPaint(hwnd);
        return 0;
    case WM_LBUTTONDOWN:
        self->OnMouseDown({ XFromLParam(lp), YFromLParam(lp) });
        return 0;
    case WM_MOUSEMOVE:
        self->OnMouseMove({ XFromLParam(lp), YFromLParam(lp) });
        return 0;
    case WM_LBUTTONUP:
        self->OnMouseUp({ XFromLParam(lp), YFromLParam(lp) });
        return 0;
    case WM_KEYDOWN:
        self->OnKeyDown(wp);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
