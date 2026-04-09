#include "D3D11Renderer.h"

#include <libavutil/hwcontext_d3d11va.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

D3D11Renderer::~D3D11Renderer() {
    shutdown();
}

bool D3D11Renderer::initialize(HWND hwnd,
                               ID3D11Device* sharedDevice,
                               ID3D11DeviceContext* sharedContext,
                               int videoWidth,
                               int videoHeight) {
    hwnd_ = hwnd;
    if (!hwnd_) {
        return false;
    }

    if (!sharedDevice || !sharedContext) {
        return false;
    }

    // Hold refs so the device stays alive while we render.
    sharedDevice->AddRef();
    sharedContext->AddRef();
    device_.Attach(sharedDevice);
    context_.Attach(sharedContext);

    // Enable multi-thread protection for the immediate context when both decode and render share it.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
    }

    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        return false;
    }

    if (!createSwapChain(videoWidth, videoHeight)) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT dstW = std::max<UINT>(1, rc.right - rc.left);
    UINT dstH = std::max<UINT>(1, rc.bottom - rc.top);

    if (!ensureVideoProcessor(videoWidth, videoHeight, dstW, dstH)) {
        return false;
    }

    return true;
}

void D3D11Renderer::shutdown() {
    videoProcessor_.Reset();
    vpEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

bool D3D11Renderer::createSwapChain(int width, int height) {
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) {
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    backbufferWidth_ = std::max<UINT>(1, rc.right - rc.left);
    backbufferHeight_ = std::max<UINT>(1, rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = backbufferWidth_;
    desc.Height = backbufferHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        device_.Get(),
        hwnd_,
        &desc,
        nullptr,
        nullptr,
        &swapChain
    );
    if (FAILED(hr)) {
        return false;
    }

    swapChain_ = swapChain;
    return true;
}

bool D3D11Renderer::resizeSwapChainIfNeeded() {
    if (!swapChain_) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = std::max<UINT>(1, rc.right - rc.left);
    UINT h = std::max<UINT>(1, rc.bottom - rc.top);

    if (w == backbufferWidth_ && h == backbufferHeight_) {
        return true;
    }

    backbufferWidth_ = w;
    backbufferHeight_ = h;

    HRESULT hr = swapChain_->ResizeBuffers(
        0,
        backbufferWidth_,
        backbufferHeight_,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        0
    );

    return SUCCEEDED(hr);
}

bool D3D11Renderer::ensureVideoProcessor(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
    if (vpEnum_ && contentDesc_.InputWidth == static_cast<UINT>(srcWidth) &&
        contentDesc_.InputHeight == static_cast<UINT>(srcHeight) &&
        contentDesc_.OutputWidth == static_cast<UINT>(dstWidth) &&
        contentDesc_.OutputHeight == static_cast<UINT>(dstHeight)) {
        updateLetterboxRects(srcWidth, srcHeight, dstWidth, dstHeight);
        return true;
    }

    vpEnum_.Reset();
    videoProcessor_.Reset();

    contentDesc_ = {};
    contentDesc_.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc_.InputWidth = srcWidth;
    contentDesc_.InputHeight = srcHeight;
    contentDesc_.OutputWidth = dstWidth;
    contentDesc_.OutputHeight = dstHeight;
    contentDesc_.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&contentDesc_, &vpEnum_))) {
        return false;
    }

    UINT flags = 0;
    if (FAILED(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags))) {
        return false;
    }
    if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        return false;
    }

    if (FAILED(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &videoProcessor_))) {
        return false;
    }

    updateLetterboxRects(srcWidth, srcHeight, dstWidth, dstHeight);
    return true;
}

void D3D11Renderer::updateLetterboxRects(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
    srcRect_ = {0, 0, srcWidth, srcHeight};

    // Keep aspect ratio while filling destination as much as possible.
    double srcAspect = static_cast<double>(srcWidth) / static_cast<double>(srcHeight);
    double dstAspect = static_cast<double>(dstWidth) / static_cast<double>(dstHeight);

    if (dstAspect > srcAspect) {
        int w = static_cast<int>(dstHeight * srcAspect);
        int x = (dstWidth - w) / 2;
        dstRect_ = {x, 0, x + w, dstHeight};
    } else {
        int h = static_cast<int>(dstWidth / srcAspect);
        int y = (dstHeight - h) / 2;
        dstRect_ = {0, y, dstWidth, y + h};
    }
}

bool D3D11Renderer::presentFrame(const AVFrame* frame, int videoWidth, int videoHeight) {
    if (!frame || !device_ || !context_ || !swapChain_) {
        return false;
    }

    if (!resizeSwapChainIfNeeded()) {
        return false;
    }

    UINT dstW = backbufferWidth_;
    UINT dstH = backbufferHeight_;
    if (!ensureVideoProcessor(videoWidth, videoHeight, dstW, dstH)) {
        return false;
    }

    auto tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    if (!tex) {
        return false;
    }
    UINT arrayIdx = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
    inDesc.FourCC = 0;
    inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inDesc.Texture2D.MipSlice = 0;
    inDesc.Texture2D.ArraySlice = arrayIdx;

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(
            tex, vpEnum_.Get(), &inDesc, &inputView))) {
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
    outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outDesc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(
            backBuffer.Get(), vpEnum_.Get(), &outDesc, &outputView))) {
        return false;
    }

    videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &srcRect_);
    videoContext_->VideoProcessorSetOutputTargetRect(videoProcessor_.Get(), TRUE, &dstRect_);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    HRESULT hr = videoContext_->VideoProcessorBlt(
        videoProcessor_.Get(),
        outputView.Get(),
        0,
        1,
        &stream
    );
    if (FAILED(hr)) {
        return false;
    }

    hr = swapChain_->Present(1, 0);
    return SUCCEEDED(hr);
}
