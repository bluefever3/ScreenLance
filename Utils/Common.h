#pragma once

// ─── Veri tipleri ─────────────────────────────────────────────────────────────
struct OcrRegion
{
    uint64_t      id{};
    RECT          bounds{};
    std::wstring  text;
    float         confidence{ 0.f };
    std::vector<uint8_t> pixelHash;

    // Orijinal metnin ekrandan otomatik algılanan görünümü (yazı boyutu/rengi
    // ve çevresindeki arka plan rengi). Overlay, çeviri metnini orijinaline
    // benzer ve görüntüyle uyumlu göstermek için bunları kullanır; algılama
    // başarısız olursa (düşük kontrast vb.) Ayarlar'daki sabit değerlere
    // geri düşülür.
    bool     hasDetectedColor{ false };
    COLORREF detectedColor{};
    bool     hasDetectedBgColor{ false };
    COLORREF detectedBgColor{};
    // ÖNEMLİ: Bu alan DirectWrite font boyutu (em) DEĞİL, OCR'ın ekranda
    // tespit ettiği metnin HAM piksel yüksekliğidir (ascender-descender
    // aralığı, bkz. OCREngine::RunOcr). Em boyutuna dönüşüm, kullanılan
    // fontun gerçek ascent/descent metriklerinden OverlayEngine::DrawText
    // içinde yapılır. <= 0 ise algılanamadı.
    float    detectedFontSize{ 0.f };
};

struct TranslatedRegion
{
    uint64_t      id{};
    RECT          bounds{};
    std::wstring  originalText;
    std::wstring  translatedText;
    bool          dirty{ true };

    bool     hasDetectedColor{ false };
    COLORREF detectedColor{};
    bool     hasDetectedBgColor{ false };
    COLORREF detectedBgColor{};
    // ÖNEMLİ: Bkz. OcrRegion::detectedFontSize – bu da em boyutu değil,
    // OCR'da tespit edilen HAM piksel yüksekliğidir.
    float    detectedFontSize{ 0.f };
};

struct CaptureFrame
{
    std::vector<uint8_t> bgra;      // BGRA pixel data
    int                  width{};
    int                  height{};
    RECT                 virtualRect{}; // Sanal masaüstü koordinatı
    int                  monitorIndex{};
    bool                 hasDesktopUpdate{};
};

// ─── IoU – bölge örtüşme oranı ───────────────────────────────────────────────
inline float CalculateIoU(const RECT& a, const RECT& b) noexcept
{
    long iL = std::max(a.left,   b.left);
    long iT = std::max(a.top,    b.top);
    long iR = std::min(a.right,  b.right);
    long iB = std::min(a.bottom, b.bottom);
    if (iR <= iL || iB <= iT) return 0.f;
    float inter = static_cast<float>((iR - iL) * (iB - iT));
    float aA    = static_cast<float>((a.right - a.left) * (a.bottom - a.top));
    float bA    = static_cast<float>((b.right - b.left) * (b.bottom - b.top));
    return inter / (aA + bA - inter);
}

// ─── Benzersiz ID üreteci ─────────────────────────────────────────────────────
inline uint64_t NewID() noexcept
{
    static std::atomic<uint64_t> s_counter{ 1 };
    return s_counter.fetch_add(1, std::memory_order_relaxed);
}

// ─── COM akıllı işaretçi ─────────────────────────────────────────────────────
// winrt::com_ptr<T> kullanımı:
//   ptr.put()       → yeni nesne almak için (Release çağırır)
//   ptr = nullptr   → serbest bırakmak için  (.reset() YOKTUR)
//   ptr.get()       → ham işaretçi almak için
template<typename T>
using ComPtr = winrt::com_ptr<T>;

// ─── Makrolar ─────────────────────────────────────────────────────────────────
#define SAFE_RELEASE(p)        do { if (p) { (p)->Release(); (p) = nullptr; } } while(0)
#define THROW_IF_FAILED(hr)    winrt::check_hresult(hr)
#define RETURN_IF_FAILED(hr)   do { if (FAILED(hr)) return hr; } while(0)
