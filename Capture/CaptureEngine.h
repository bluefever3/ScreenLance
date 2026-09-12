#pragma once
#include "../Utils/Common.h"
#include "../Settings/Config.h"

// CaptureFrame is defined in Common.h

// Per-monitor duplication context
struct MonitorContext
{
    int                              index{ 0 };
    RECT                             bounds{};
    ComPtr<IDXGIOutputDuplication>   duplication;
    ComPtr<ID3D11Texture2D>          stagingTex;
    int                              width{ 0 };
    int                              height{ 0 };
};

using FrameCallback = std::function<void(CaptureFrame)>;

class CaptureEngine
{
public:
    explicit CaptureEngine(const CaptureConfig& cfg);
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&)            = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    HRESULT Init();
    void    Start(FrameCallback cb);
    void    Stop();
    void    Restart();   // Called by Watchdog

    // Belirli bir ekran bölgesini senkron olarak TEK SEFERLİK yakalar
    // (One-shot mod için). Sürekli-modun ZATEN AÇIK olan duplication
    // handle'ını yeniden kullanır – aynı monitör çıktısı için ikinci bir
    // bağımsız IDXGIOutputDuplication oluşturmaya çalışmak (farklı bir D3D
    // cihazından bile olsa) Windows tarafından reddedilir; bir çıktı için
    // aynı anda yalnızca TEK bir aktif duplication'a izin verilir.
    // Thread-safe: sürekli-modun arka plan thread'iyle m_duplicationMutex
    // üzerinden senkronize olur.
    HRESULT CaptureRegionOnce(const RECT& region, CaptureFrame& outFrame);

    bool IsRunning() const noexcept { return m_running.load(); }
    const std::vector<RECT>& GetMonitorRects() const { return m_monitorRects; }

private:
    void   CaptureLoop();
    HRESULT InitD3D();
    HRESULT InitDuplication();
    HRESULT CaptureMonitor(MonitorContext& ctx, FrameCallback& cb);
    HRESULT RecreateDevice();

    CaptureConfig              m_cfg;
    FrameCallback              m_callback;
    std::atomic<bool>          m_running{ false };
    std::thread                m_thread;
    std::mutex                 m_duplicationMutex; // ctx.duplication/stagingTex erişimini korur

    // D3D objects
    ComPtr<ID3D11Device>        m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dCtx;
    ComPtr<IDXGIFactory1>       m_dxgiFactory;

    std::vector<MonitorContext> m_monitors;
    std::vector<RECT>           m_monitorRects;  // Shared read – set during Init
};
