#pragma once
#include "../Utils/Common.h"

// Tam ekran, yarı şeffaf alan seçim aracı (klasik "screenshot tool" tarzı).
// Sanal masaüstünün tamamını kaplar (negatif koordinatlı ikincil monitörler
// dahil). Sol fare tuşuyla sürükleyip bırakma ile dikdörtgen seçilir.
// ESC ile iptal edilir.
//
// Show() çağrısı, kullanıcı seçimi tamamlayana kadar BLOKE OLUR – kendi
// mesaj döngüsünü çalıştırır (klasik Win32 modal pencere deseni gibi).
// Bu, Application'ın ana mesaj döngüsünü etkilemez; Show() döndüğünde
// kontrol normal şekilde çağırana geri döner.
class SelectionWindow
{
public:
    explicit SelectionWindow(HINSTANCE hInstance);
    ~SelectionWindow();

    SelectionWindow(const SelectionWindow&)            = delete;
    SelectionWindow& operator=(const SelectionWindow&) = delete;

    // Seçilen dikdörtgeni SANAL MASAÜSTÜ koordinatlarında döndürür.
    // Kullanıcı ESC'ye basarsa veya çok küçük bir seçim yaparsa
    // (yanlışlıkla tıklama), boş bir RECT (sıfırlanmış) döner.
    RECT Show();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint(HWND hwnd);
    void OnMouseDown(POINT clientPt);
    void OnMouseMove(POINT clientPt);
    void OnMouseUp(POINT clientPt);
    void OnKeyDown(WPARAM vk);

    HINSTANCE m_hInstance;
    HWND      m_hwnd{ nullptr };
    RECT      m_virtualRect{};   // Sanal masaüstü sınırları (ekran koordinatı)

    bool      m_dragging{ false };
    bool      m_cancelled{ false };
    POINT     m_startPt{};       // İstemci (client) koordinatı
    POINT     m_curPt{};         // İstemci (client) koordinatı
    RECT      m_result{};        // Sanal masaüstü koordinatında sonuç

    static constexpr wchar_t kClassName[] = L"ScreenLanceSelectionWnd";
};
