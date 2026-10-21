#include "RenderPipelineYUV.h"

using namespace std::string_literals;

namespace d3d_render_utils {

RenderPipelineYUV::RenderPipelineYUV(ID3D11Device* device) {
    mDevice = device;
    mDevice->GetImmediateContext(&mImmediateContext);
}

void RenderPipelineYUV::Initialize(
    std::vector<ID3D11Texture2D*> inputTextures,
    ID3D11VertexShader* quadVertexShader,
    ID3D11PixelShader* pixelShader,
    ID3D11Texture2D* renderTarget,
    ID3D11Buffer* shaderBuffer
) {
    mInputTextureViews.clear();
    mInputTextureViewPtrs.clear();
    for (auto tex : inputTextures) {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resourceView;
        OK_OR_THROW(
            mDevice->CreateShaderResourceView(tex, nullptr, resourceView.GetAddressOf()),
            "Failed to create input texture resosurce view."
        );
        mInputTextureViews.push_back(resourceView);
        mInputTextureViewPtrs.push_back(resourceView.Get());
    }

    D3D11_TEXTURE2D_DESC renderTargetDesc;
    renderTarget->GetDesc(&renderTargetDesc);

    DXGI_FORMAT uvFormat = renderTargetDesc.Format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8G8_UNORM
                                                                       : DXGI_FORMAT_R16G16_UNORM;
    DXGI_FORMAT yFormat = renderTargetDesc.Format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8_UNORM
                                                                      : DXGI_FORMAT_R16_UNORM;

    // Create SRV for luminance (Y) plane
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = yFormat;
    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescY.Texture2D.MostDetailedMip = 0;
    srvDescY.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* pSRVY;
    OK_OR_THROW(
        mDevice->CreateShaderResourceView(renderTarget, &srvDescY, &mRenderTargetResourceViewY),
        "Failed to create render target resosurce view."
    );

    // Create SRV for chrominance (UV) planes
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = uvFormat;
    srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescUV.Texture2D.MostDetailedMip = 0;
    srvDescUV.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* pSRVUV;
    OK_OR_THROW(
        mDevice->CreateShaderResourceView(renderTarget, &srvDescUV, &mRenderTargetResourceViewUV),
        "Failed to create render target resosurce view."
    );

    // Create luminance (Y) render target view
    D3D11_RENDER_TARGET_VIEW_DESC rtvDescLuminance = {};
    rtvDescLuminance.Format = yFormat;
    rtvDescLuminance.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDescLuminance.Texture2D.MipSlice = 0;
    ID3D11RenderTargetView* pRTVLuminance;
    OK_OR_THROW(
        mDevice->CreateRenderTargetView(renderTarget, &rtvDescLuminance, &mRenderTargetViewY),
        "Failed to create ID3D11RenderTargetView."
    );

    // Create chrominance (UV) render target view
    D3D11_RENDER_TARGET_VIEW_DESC rtvDescChrominance = {};
    rtvDescChrominance.Format = uvFormat;
    rtvDescChrominance.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDescChrominance.Texture2D.MipSlice = 0;
    ID3D11RenderTargetView* pRTVChrominance;
    OK_OR_THROW(
        mDevice->CreateRenderTargetView(renderTarget, &rtvDescChrominance, &mRenderTargetViewUV),
        "Failed to create ID3D11RenderTargetView."
    );

    mViewportY.Width = (FLOAT)renderTargetDesc.Width;
    mViewportY.Height = (FLOAT)renderTargetDesc.Height;
    mViewportY.MinDepth = 0.0f;
    mViewportY.MaxDepth = 1.0f;
    mViewportY.TopLeftX = 0.0f;
    mViewportY.TopLeftY = 0.0f;

    mViewportUV.Width = (FLOAT)renderTargetDesc.Width;
    mViewportUV.Height = (FLOAT)renderTargetDesc.Height;
    mViewportUV.MinDepth = 0.0f;
    mViewportUV.MaxDepth = 1.0f;
    mViewportUV.TopLeftX = 0.0f;
    mViewportUV.TopLeftY = 0.0f;

    mGenerateMipmaps = renderTargetDesc.MipLevels != 1;

    mShaderBuffer = shaderBuffer;
    mVertexShader = quadVertexShader;
    mPixelShader = pixelShader;
}

void RenderPipelineYUV::Initialize(
    std::vector<ID3D11Texture2D*> inputTextures,
    ID3D11VertexShader* quadVertexShader,
    std::vector<uint8_t>& pixelShaderCSO,
    ID3D11Texture2D* renderTarget,
    ID3D11Buffer* shaderBuffer
) {
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    pixelShader.Attach(CreatePixelShader(mDevice.Get(), pixelShaderCSO));
    Initialize(inputTextures, quadVertexShader, pixelShader.Get(), renderTarget, shaderBuffer);
}

void RenderPipelineYUV::Render(ID3D11DeviceContext* otherContext) {
    ID3D11DeviceContext* context = otherContext != nullptr ? otherContext : mImmediateContext.Get();

    ID3D11RenderTargetView* aRenderTargets[]
        = { mRenderTargetViewY.Get(), mRenderTargetViewUV.Get() };
    D3D11_VIEWPORT aViewports[] = { mViewportY, mViewportUV };

    ID3D11RenderTargetView* aRenderTargetsHack[] = { mRenderTargetViewY.Get() };
    D3D11_VIEWPORT aViewportsHack[] = { mViewportY };

    if (mSeparatePlanePasses) {
        // Replace the preceding pass's RTV before binding its texture as an SRV.
        // Otherwise D3D11 substitutes NULL for the conflicting input binding.
        context->OMSetRenderTargets(1, aRenderTargetsHack, nullptr);
        context->RSSetViewports(1, &mViewportY);
    } else {
        context->OMSetRenderTargets(2, aRenderTargets, nullptr);
        context->RSSetViewports(2, aViewports);
    }

    if (mShaderBuffer != nullptr) {
        context->PSSetConstantBuffers(0, 1, mShaderBuffer.GetAddressOf());
    }

    context->PSSetShaderResources(0, (UINT)mInputTextureViewPtrs.size(), mInputTextureViewPtrs.data());

    context->VSSetShader(mVertexShader.Get(), nullptr, 0);
    context->PSSetShader(mPixelShader.Get(), nullptr, 0);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (mSeparatePlanePasses) {
        context->Draw(4, 0);

        ID3D11RenderTargetView* uvTargets[] = { nullptr, mRenderTargetViewUV.Get() };
        context->OMSetRenderTargets(2, uvTargets, nullptr);
        context->RSSetViewports(1, &mViewportUV);
        context->Draw(4, 0);

        // Release input/output bindings before the next frame and encoder copy.
        static ID3D11ShaderResourceView* const
            nullViews[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        context->PSSetShaderResources(0, (UINT)mInputTextureViewPtrs.size(), nullViews);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        return;
    }
    context->Draw(4, 0);

    // HACK: Whyyyyyy does the Y channel only render the top-left corner
    // with both render targets enabled????
    context->OMSetRenderTargets(1, aRenderTargetsHack, nullptr);
    context->RSSetViewports(1, aViewportsHack);

    context->Draw(4, 0);

    if (mGenerateMipmaps) {
        context->GenerateMips(mRenderTargetResourceViewY.Get());
        context->GenerateMips(mRenderTargetResourceViewUV.Get());
    }
}

}
