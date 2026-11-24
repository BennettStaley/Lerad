// SPDX-License-Identifier: MIT
// Moves the picture on each frame that reaches the screen by what the input
// program asks for (src/common/view_shift.h, shift.hlsl).
#pragma once
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "compositor.h"
#include "view_shift.h"

namespace app::overlay {

class ViewShifter {
public:
    // name: the file mapping's ("" = none). It is opened when it is there.
    void set_name(const std::wstring &name) { name_ = name; }
    bool named() const { return !name_.empty(); }
    // Moves the picture's part of back_buffer, and puts the device's state
    // back as it was.
    void apply(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, const D3D11_BOX &picture,
               Encoding encoding, float paper_nits);
    void release();

private:
    bool read(ViewShift &out);
    bool ready(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &bb);

    std::wstring name_;
    HANDLE mapping_ = nullptr;
    const ViewShift *shared_ = nullptr;
    int64_t tried_ = 0;
    bool failed_ = false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copy_srv_;
    D3D11_TEXTURE2D_DESC copy_desc_{};
};

}  // namespace app::overlay
