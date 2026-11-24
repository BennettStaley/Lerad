// SPDX-License-Identifier: MIT
// The add-on's half of the stream window (src/common/stream_link.h): each
// game frame, scaled and tone mapped to SDR, into the texture the app's
// window shows. Nothing is made, and nothing costs, while it is not wanted.
#pragma once
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "compositor.h"

namespace app::overlay {

class StreamOut {
public:
    // Whether the app that started mpv has a stream window to offer.
    bool offered();
    // Whether the window is wanted now, and saying so (the menu's switch).
    bool wanted();
    void set_wanted(bool on);
    // How bright an HDR picture is in the window, percent (the settings
    // file's stream.brightness): 100 puts HDR's reference white at SDR white.
    int brightness() const { return brightness_; }
    void set_brightness(int percent) { brightness_ = percent < 40 ? 40 : percent > 160 ? 160 : percent; }

    // The picture (box of back_buffer, in its encoding) into the shared
    // texture, if the window is wanted. Leaves the context's state as it was.
    void frame(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, Encoding encoding,
               const D3D11_BOX &box);

    void release();

private:
    bool open_events();
    bool make(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &bb);
    void drop(const char *why);

    HANDLE on_ = nullptr, frame_ = nullptr;
    bool looked_ = false;
    bool failed_ = false;   // could not be made on this device: not tried again until asked again
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_;  // the back buffer, to read from
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copy_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> shared_rtv_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex_;
    HANDLE shared_handle_ = nullptr;  // keeps the texture's name alive
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    Encoding encoding_ = Encoding::Sdr;
    int brightness_ = 80;
    unsigned long long frames_ = 0, busy_ = 0;
};

}  // namespace app::overlay
