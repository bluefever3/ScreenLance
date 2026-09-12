#pragma once
#include "../Utils/Common.h"
#include "../Settings/Config.h"

// Transparent, click-through, always-on-top overlay window.
// Renders translated text on top of blurred original text regions.
//
// OCR-capture and overlay are fully separated:
//   – Capture reads from the real desktop frame (IDXGIOutputDuplication)
//   – Overlay window is a separate layered HWND never included in the capture
//
// This prevents the Translate→OCR→Translate infinite loop.
class OverlayEngine
{
public:
    explicit OverlayEngine(const OverlayConfig& cfg, HINSTANCE hInstance);
    ~OverlayEngine();

    OverlayEngine(const OverlayEngine&)            = delete;
    OverlayEngine& operator=(const OverlayEngine&) = delete;

    HRESULT Init(const RECT& virtualDesktopRect);
    void    Shutdown();

    // Yatay taşma sınırlaması için her fiziksel monitörün sanal masaüstü
    // koordinatındaki sınırlarını bildirir (bkz. DrawRegion / maxRight).
    // CaptureEngine::GetMonitorRects() ile aynı listeden, Init'ten sonra
    // bir kez çağrılması yeterlidir.
    void SetMonitorRects(std::vector<RECT> rects);

    // Update overlay with new translated regions.
    // Thread-safe – called from Application pipeline thread.
    void UpdateRegions(std::vector<TranslatedRegion> regions);

    // Ayarlar penceresinden gelen yeni yapılandırmayı CANLI olarak uygular
    // (yeniden başlatma gerekmez). D3D cihazı/takas zinciri/DirectComposition
    // dokunulmaz; yalnızca metin formatı ve fırçalar yeniden oluşturulur.
    // Thread-safe – render thread ile aynı mutex üzerinden senkronize olur.
    void UpdateConfig(const OverlayConfig& newCfg);

    // Remove all overlays (e.g., continuous mode stopped)
    void ClearAll();

    // Called when GPU device is removed (driver restart, sleep etc.)
    HRESULT RecreateDevice();

    HWND GetHwnd() const { return m_hwnd; }

    // Windows message pump helper – call in main thread
    static bool ProcessMessages();

private:
    HRESULT CreateDeviceResources();
    HRESULT CreateDCompVisual();
    void    RenderFrame();
    void    DrawRegion(const TranslatedRegion& r);
    void    DrawBlur         (const TranslatedRegion& r, const D2D1_RECT_F& rect); // BlurMode::Blur          – şimdilik SemiTransparent ile aynı (bkz. .cpp notu)
    void    DrawAverageColor (const TranslatedRegion& r, const D2D1_RECT_F& rect); // BlurMode::AverageColor  – şimdilik SemiTransparent ile aynı (bkz. .cpp notu)
    void    DrawText         (const TranslatedRegion& r, const D2D1_RECT_F& rect, float fontSize, bool singleLine, float topInsetRatio);

    // Bölgenin ekran koordinatlarını overlay penceresi yerel koordinatına çevirir.
    D2D1_RECT_F ComputeScreenRect(const TranslatedRegion& r) const;

    // Şu an yapılandırılmış fontun (m_cfg.fontFamily/fontBold) GERÇEK
    // ascent/descent oranlarını (IDWriteFontFace::GetMetrics'ten, em başına)
    // günceller. CreateDeviceResources() ve UpdateConfig() içinde, font
    // ailesi/kalınlığı değişebileceği her yerde çağrılır. ARTIK yalnızca
    // MeasureInkMetrics ölçümü BAŞARISIZ OLURSA (örn. seçili font bulunamazsa)
    // yedek olarak kullanılır — hem BOYUT hem HİZALAMA hesaplaması normalde
    // satır bazlı içerik-duyarlı ölçüme (MeasureInkMetrics) dayanır, çünkü
    // fontun genel oranı satırdaki gerçek karakterlere göre değişen mürekkep
    // yüksekliğini/konumunu yakalayamıyordu.
    void RefreshFontMetricRatios();

    // VERİLEN metni (originalText – OCR'ın çeviriden ÖNCE gördüğü orijinal
    // metin), referans bir font boyutunda GERÇEKTEN DirectWrite ile dizip
    // hem gerçek mürekkep (ink) yüksekliğinin hem de "üst boşluk"
    // (mürekkebin doğal layout kutusunun tepesine göre ne kadar aşağıda
    // başladığı) referans boyuta oranını döndürür (false = ölçülemedi).
    // Sabit bir ascent/descent oranı YERİNE bunu kullanmamızın sebebi: hem
    // BOYUT hem de HİZALAMA, OCR'ın tespit ettiği kutuda FİİLEN GEÇEN
    // karakterlere göre değişir (örn. çıkıntılı/sarkan harf içermeyen
    // "over" gibi bir kelimede kutu çok kısa, "Kg" gibi hem büyük hem
    // sarkan harf içeren bir kelimede çok uzun çıkar; tamamen büyük harfli
    // bir kelimede mürekkep yalnızca cap-height kadardır, fontun genel
    // ascent oranı bundan belirgin büyüktür). Fontun genel oranları bu
    // satır bazlı farkı hiçbir zaman doğru yakalayamaz. Bu satır-bazlı
    // gerçek ölçüm, hangi karakterler geçerse geçsin doğru sonuç verir.
    bool MeasureInkMetrics(const std::wstring& text, float& outHeightRatio, float& outTopInsetRatio) const;

    // r için GERÇEK DirectWrite font boyutunu hesaplar; çeviri metni
    // orijinalinden UZUN çıkarsa (Türkçe çeviriler İngilizce'den genelde
    // daha uzun olur) ÖNCE kutuyu YATAYDA büyütmeyi dener (boxLeft'ten,
    // maxRight'a kadar – örn. ekranın/monitörün sağ kenarına kadar hâlâ boş
    // yer varsa) – çünkü OCR'ın tespit ettiği dar kutu genişliği, o an
    // orijinal metnin NE KADAR yer kapladığını gösterir, ekranda GERÇEKTEN
    // NE KADAR yer OLDUĞUNU değil ("Seçtiğim alanda orijinal yazının
    // algılandığı alan ile yazılabilecek alan farklı şeyler" – bkz. proje
    // notu). Çeviri, o genişletilmiş sınıra kadar bile tek satıra
    // SIĞMIYORSA (gerçekten ekran kenarına dayanıyorsa) ancak O ZAMAN
    // satırlara sarılır ve kutu YÜKSEKLİKTE büyütülür – aksi halde sarılan
    // satırlar zorla tek-satırlık aralığa sıkıştırılıp üst üste biner (bu,
    // "yazı küçük/karışık görünüyor" şikayetinin asıl nedeniydi: font boyutu
    // aslında doğruydu, satırlar birbirinin üstüne biniyordu).
    // outTopInsetRatio: bkz. MeasureInkMetrics – DrawText içinde dikey
    // hizalama için kullanılır (fontun genel ascent oranı YERİNE).
    void ComputeTextLayoutInfo(const TranslatedRegion& r, float boxLeft, float boxWidth, float maxRight,
                                float& outFontSize, float& outWidth, float& outHeight, bool& outSingleLine,
                                float& outTopInsetRatio) const;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void HandleDeviceLost();

    // r'nin bulunduğu FİZİKSEL MONİTÖRÜN sağ kenarını (yerel/overlay
    // koordinatında) döndürür – ComputeTextLayoutInfo'ya geçilecek doğru
    // "maxRight" budur. m_monitorRects boşsa veya r.bounds hiçbirine denk
    // gelmiyorsa (örn. el ile seçim/one-shot ya da monitör listesi henüz
    // ayarlanmadıysa) tüm sanal masaüstünün sağ kenarına düşer (eski
    // davranış – yalnızca güvenli bir yedek).
    float ComputeMaxRightForRegion(const TranslatedRegion& r) const;

    OverlayConfig m_cfg;
    HINSTANCE     m_hInstance;
    HWND          m_hwnd{ nullptr };
    RECT          m_virtualRect{};
    std::vector<RECT> m_monitorRects; // Fiziksel monitör sınırları (sanal masaüstü koordinatında)

    // D3D / D2D / DComp objects
    ComPtr<ID3D11Device>          m_d3dDevice;
    ComPtr<ID3D11DeviceContext>   m_d3dCtx;
    ComPtr<IDXGISwapChain1>       m_swapChain;
    ComPtr<ID2D1Factory1>         m_d2dFactory;
    ComPtr<ID2D1Device>           m_d2dDevice;
    ComPtr<ID2D1DeviceContext>    m_d2dCtx;
    ComPtr<IDWriteFactory>        m_dwFactory;
    ComPtr<IDWriteTextFormat>     m_textFormat;
    ComPtr<ID2D1SolidColorBrush>  m_textBrush;
    ComPtr<ID2D1SolidColorBrush>  m_bgBrush;        // Tüm blurMode'lar için: yapılandırılmış düz renk
    ComPtr<ID2D1SolidColorBrush>  m_outlineBrush;   // Yazı anahat rengi (metin rengine göre otomatik kontrast)

    // m_cfg.fontFamily/fontBold için em başına ascent/descent oranı
    // (RefreshFontMetricRatios ile doldurulur). Bulunamazsa (font eksik vb.)
    // Arial'e yakın makul bir varsayılana düşer, 0'a düşmez.
    float m_fontAscentRatio{ 0.905f };
    float m_fontDescentRatio{ 0.212f };
    ComPtr<IDCompositionDevice>   m_dcompDevice;
    ComPtr<IDCompositionTarget>   m_dcompTarget;
    ComPtr<IDCompositionVisual>   m_dcompVisual;

    // Region state (protected by mutex)
    std::mutex m_regionMutex;
    std::vector<TranslatedRegion> m_regions;
    bool m_dirty{ false };

    // Render thread
    std::atomic<bool> m_running{ false };
    std::thread       m_renderThread;

    static constexpr wchar_t kClassName[] = L"ScreenLanceOverlay";
};
