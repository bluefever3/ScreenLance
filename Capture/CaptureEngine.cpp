#include "../Utils/pch.h"
#include "CaptureEngine.h"
#include "../Utils/Logger.h"
#include <chrono>

CaptureEngine::CaptureEngine(const CaptureConfig& cfg)
    : m_cfg(cfg)
{}

CaptureEngine::~CaptureEngine() { Stop(); }

HRESULT CaptureEngine::Init()
{
    HRESULT hr = InitD3D();
    if (FAILED(hr)) {
        Logger::ErrorF("CaptureEngine: D3D init failed 0x{:08X}", static_cast<unsigned>(hr));
        return hr;
    }
    hr = InitDuplication();
    if (FAILED(hr)) {
        Logger::ErrorF("CaptureEngine: Duplication init failed 0x{:08X}", static_cast<unsigned>(hr));
        return hr;
    }
    Logger::InfoF("CaptureEngine: initialised with {} monitor(s).", m_monitors.size());
    return S_OK;
}

HRESULT CaptureEngine::InitD3D()
{
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        m_d3dDevice.put(), nullptr, m_d3dCtx.put());
    return hr;
}

HRESULT CaptureEngine::InitDuplication()
{
    m_monitors.clear();
    m_monitorRects.clear();

    ComPtr<IDXGIDevice>  dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());
    RETURN_IF_FAILED(hr);
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), adapter.put_void());
    RETURN_IF_FAILED(hr);

    int monIdx = 0;
    ComPtr<IDXGIOutput> output;
    while (adapter->EnumOutputs(monIdx, output.put()) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);

        // Filter by user config
        bool include = m_cfg.monitorIndices.empty() ||
            std::ranges::contains(m_cfg.monitorIndices, monIdx);

        if (include)
        {
            ComPtr<IDXGIOutput1> output1;
            hr = output->QueryInterface(__uuidof(IDXGIOutput1), output1.put_void());
            if (SUCCEEDED(hr))
            {
                MonitorContext ctx;
                ctx.index  = monIdx;
                ctx.bounds = desc.DesktopCoordinates;
                ctx.width  = ctx.bounds.right  - ctx.bounds.left;
                ctx.height = ctx.bounds.bottom - ctx.bounds.top;

                hr = output1->DuplicateOutput(m_d3dDevice.get(), ctx.duplication.put());
                if (SUCCEEDED(hr))
                {
                    // Create staging texture for CPU readback
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width            = ctx.width;
                    td.Height           = ctx.height;
                    td.MipLevels        = 1;
                    td.ArraySize        = 1;
                    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage            = D3D11_USAGE_STAGING;
                    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

                    m_d3dDevice->CreateTexture2D(&td, nullptr, ctx.stagingTex.put());
                    m_monitorRects.push_back(ctx.bounds);
                    m_monitors.push_back(std::move(ctx));
                    Logger::InfoF("  Monitor {}: {}x{} @ ({},{})",
                        monIdx, ctx.width, ctx.height,
                        desc.DesktopCoordinates.left, desc.DesktopCoordinates.top);
                }
            }
        }
        output = nullptr;
        ++monIdx;
    }
    return m_monitors.empty() ? E_FAIL : S_OK;
}

void CaptureEngine::Start(FrameCallback cb)
{
    if (m_running.exchange(true)) return;
    m_callback = std::move(cb);
    m_thread   = std::thread(&CaptureEngine::CaptureLoop, this);
    Logger::Info("CaptureEngine: started.");
}

void CaptureEngine::Stop()
{
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    Logger::Info("CaptureEngine: stopped.");
}

void CaptureEngine::Restart()
{
    Stop();
    if (SUCCEEDED(InitDuplication()))
        Start(m_callback);
    else
        Logger::Error("CaptureEngine: restart failed.");
}

void CaptureEngine::CaptureLoop()
{
    using clock = std::chrono::steady_clock;
    auto frameInterval = std::chrono::milliseconds(1000 / std::max(1, m_cfg.captureFps));

    while (m_running.load(std::memory_order_relaxed))
    {
        auto frameStart = clock::now();

        for (auto& ctx : m_monitors)
        {
            HRESULT hr = CaptureMonitor(ctx, m_callback);
            if (hr == DXGI_ERROR_ACCESS_LOST ||
                hr == DXGI_ERROR_DEVICE_REMOVED ||
                hr == DXGI_ERROR_DEVICE_RESET)
            {
                Logger::Warning("CaptureEngine: device lost – reinitialising.");
                if (FAILED(RecreateDevice()))
                {
                    m_running.store(false);
                    return;
                }
                break;
            }
        }

        auto elapsed = clock::now() - frameStart;
        if (elapsed < frameInterval)
            std::this_thread::sleep_for(frameInterval - elapsed);
    }
}

HRESULT CaptureEngine::CaptureMonitor(MonitorContext& ctx, FrameCallback& cb)
{
    std::lock_guard lock(m_duplicationMutex); // CaptureRegionOnce ile çakışmayı önle

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource>   resource;
    HRESULT hr = ctx.duplication->AcquireNextFrame(0, &frameInfo, resource.put());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_OK; // No new frame yet
    if (FAILED(hr)) return hr;

    // Only read back if there was a desktop update
    if (frameInfo.LastPresentTime.QuadPart != 0)
    {
        ComPtr<ID3D11Texture2D> desktopTex;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), desktopTex.put_void());
        if (SUCCEEDED(hr))
        {
            m_d3dCtx->CopyResource(ctx.stagingTex.get(), desktopTex.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = m_d3dCtx->Map(ctx.stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr))
            {
                CaptureFrame frame;
                frame.width        = ctx.width;
                frame.height       = ctx.height;
                frame.virtualRect  = ctx.bounds;
                frame.monitorIndex = ctx.index;
                frame.hasDesktopUpdate = true;
                frame.bgra.resize(ctx.width * ctx.height * 4);

                const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
                uint8_t*       dst = frame.bgra.data();
                for (int row = 0; row < ctx.height; ++row)
                    std::memcpy(dst + row * ctx.width * 4,
                                src + row * mapped.RowPitch,
                                ctx.width * 4);

                m_d3dCtx->Unmap(ctx.stagingTex.get(), 0);
                cb(std::move(frame));
            }
        }
    }
    ctx.duplication->ReleaseFrame();
    return S_OK;
}

HRESULT CaptureEngine::RecreateDevice()
{
    m_monitors.clear();
    m_d3dCtx   = nullptr;
    m_d3dDevice= nullptr;

    HRESULT hr = InitD3D();
    if (FAILED(hr)) return hr;
    return InitDuplication();
}

HRESULT CaptureEngine::CaptureRegionOnce(const RECT& region, CaptureFrame& outFrame)
{
    MonitorContext* target = nullptr;
    for (auto& ctx : m_monitors)
    {
        RECT overlap{};
        if (::IntersectRect(&overlap, &ctx.bounds, &region)) { target = &ctx; break; }
    }
    if (!target || !target->duplication)
        return E_FAIL;

    std::lock_guard lock(m_duplicationMutex); // CaptureLoop thread'iyle (sürekli mod) çakışmayı önle

    // Oyun/uygulama aktif render ettiği için yeni bir kare kısa sürede
    // gelmeli; yine de birkaç deneme ile güvenli tarafta kalıyoruz.
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource>   resource;
    HRESULT acqHr = E_FAIL;
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        acqHr = target->duplication->AcquireNextFrame(50, &frameInfo, resource.put());
        if (acqHr != DXGI_ERROR_WAIT_TIMEOUT) break;
    }
    if (FAILED(acqHr))
        return acqHr;

    ComPtr<ID3D11Texture2D> desktopTex;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), desktopTex.put_void());
    if (FAILED(hr)) { target->duplication->ReleaseFrame(); return hr; }

    m_d3dCtx->CopyResource(target->stagingTex.get(), desktopTex.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_d3dCtx->Map(target->stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { target->duplication->ReleaseFrame(); return hr; }

    int rx0 = std::clamp(static_cast<int>(region.left   - target->bounds.left), 0, target->width);
    int ry0 = std::clamp(static_cast<int>(region.top    - target->bounds.top),  0, target->height);
    int rx1 = std::clamp(static_cast<int>(region.right  - target->bounds.left), 0, target->width);
    int ry1 = std::clamp(static_cast<int>(region.bottom - target->bounds.top),  0, target->height);
    int w = rx1 - rx0, h = ry1 - ry0;

    if (w > 0 && h > 0)
    {
        outFrame.bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int row = 0; row < h; ++row)
        {
            const uint8_t* srcRow = src + static_cast<size_t>(ry0 + row) * mapped.RowPitch
                                         + static_cast<size_t>(rx0) * 4;
            std::memcpy(outFrame.bgra.data() + static_cast<size_t>(row) * w * 4, srcRow,
                        static_cast<size_t>(w) * 4);
        }
        outFrame.width            = w;
        outFrame.height           = h;
        outFrame.virtualRect      = region;
        outFrame.monitorIndex     = target->index;
        outFrame.hasDesktopUpdate = true;
    }

    m_d3dCtx->Unmap(target->stagingTex.get(), 0);
    target->duplication->ReleaseFrame();

    return outFrame.bgra.empty() ? E_FAIL : S_OK;
}
