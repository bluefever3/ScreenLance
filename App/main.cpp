#include "../Utils/pch.h"
#include "Application.h"   // main.cpp, App\ klasöründe olduğu için düz isim yeterli

// ─── Entry Point ─────────────────────────────────────────────────────────────
int WINAPI wWinMain(_In_     HINSTANCE hInstance,
                    _In_opt_ HINSTANCE /*hPrevInstance*/,
                    _In_     PWSTR     /*lpCmdLine*/,
                    _In_     int       /*nCmdShow*/)
{
    // KRİTİK: Proje hiçbir manifest ile DPI farkındalığı bildirmiyordu, bu
    // yüzden Windows uygulamayı "DPI-unaware" sayıyor ve overlay penceresinin
    // içeriğini DWM üzerinden ekranın ölçek faktörüne göre (örn. %125, %150)
    // OTOMATİK olarak yeniden ölçekliyordu. Ekran yakalama (Desktop
    // Duplication API) ve DXGI_OUTPUT_DESC.DesktopCoordinates (overlay'in
    // boyut/konumu buradan geliyor) HER ZAMAN gerçek fiziksel piksel
    // değerlerini kullanır ve DPI ölçeklendirmesinden ETKİLENMEZ. Bu ikisi
    // arasındaki uyumsuzluk, OCR/font boyutu matematiği ne kadar doğru olursa
    // olsun overlay'in orijinal metinle aynı boyutta görünmemesine yol
    // açıyordu (ekran ölçeği %100 olmayan her sistemde). Per-Monitor-V2 DPI
    // farkındalığını EN BAŞTA, herhangi bir pencere oluşturulmadan ÖNCE
    // bildirmek bu sorunu kökten çözer: artık GetSystemMetrics(SM_CXVIRTUALSCREEN)
    // gibi çağrılar da (bkz. SelectionWindow.cpp) DXGI ile aynı gerçek piksel
    // uzayını raporlar.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // C++/WinRT apartment – Windows OCR WinRT API'leri için gerekli
    winrt::init_apartment();

    Application app(hInstance);
    return app.Run();
}
