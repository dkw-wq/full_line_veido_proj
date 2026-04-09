#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <libavutil/frame.h>

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    bool initialize(HWND hwnd,
                    ID3D11Device* sharedDevice,
                    ID3D11DeviceContext* sharedContext,
                    int videoWidth,
                    int videoHeight);

    // Render one hardware-decoded frame (D3D11VA). No CPU copy.
    bool presentFrame(const AVFrame* frame, int videoWidth, int videoHeight);

    void shutdown();

private:
    bool createSwapChain(int width, int height);
    bool ensureVideoProcessor(int srcWidth, int srcHeight, int dstWidth, int dstHeight);
    bool resizeSwapChainIfNeeded();
    void updateLetterboxRects(int srcWidth, int srcHeight, int dstWidth, int dstHeight);

private:
    HWND hwnd_ = nullptr;
    UINT backbufferWidth_ = 0;
    UINT backbufferHeight_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc_{};
    RECT srcRect_{};
    RECT dstRect_{};
};
