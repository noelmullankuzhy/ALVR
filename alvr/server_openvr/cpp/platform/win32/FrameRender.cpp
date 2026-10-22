#include "FrameRender.h"
#include "angle_chart_shader.h"
#include "alvr_server/shader/AngleChartColor.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/Utils.h"
#include "alvr_server/bindings.h"
#include "sdr_p010_shader.h"
#include "pq_p010_shader.h"
#include "pq_frame_shader.h"
#include "pq_correction_shader.h"
#include "alvr_server/Hdr10.h"

extern uint64_t g_DriverTestMode;

using namespace d3d_render_utils;

static const DirectX::XMFLOAT4X4 _identityMat = DirectX::XMFLOAT4X4(
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f
);

static DirectX::XMMATRIX HmdMatrix_AsDxMat(const vr::HmdMatrix34_t& m) {
    // I think the negative Y basis is a handedness thing?
    DirectX::XMFLOAT4X4 f = DirectX::XMFLOAT4X4(
        m.m[0][0],
        m.m[1][0],
        m.m[2][0],
        0.0f,
        -m.m[0][1],
        -m.m[1][1],
        -m.m[2][1],
        0.0f,
        m.m[0][2],
        m.m[1][2],
        m.m[2][2],
        0.0f,
        m.m[0][3],
        m.m[1][3],
        m.m[2][3],
        1.0f
    );
    return DirectX::XMLoadFloat4x4(&f);
}

static DirectX::XMMATRIX HmdMatrix_AsDxMatOrientOnly(vr::HmdMatrix34_t& m) {
    // I think the negative Y basis is a handedness thing?
    DirectX::XMFLOAT4X4 f = DirectX::XMFLOAT4X4(
        m.m[0][0],
        m.m[1][0],
        m.m[2][0],
        0.0f,
        -m.m[0][1],
        -m.m[1][1],
        -m.m[2][1],
        0.0f,
        m.m[0][2],
        m.m[1][2],
        m.m[2][2],
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f
    );
    return DirectX::XMLoadFloat4x4(&f);
}

static DirectX::XMMATRIX HmdMatrix_AsDxMatPosOnly(const vr::HmdMatrix34_t& m) {
    DirectX::XMFLOAT4X4 f = DirectX::XMFLOAT4X4(
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        m.m[0][3],
        m.m[1][3],
        m.m[2][3],
        1.0f
    );
    return DirectX::XMLoadFloat4x4(&f);
}

FrameRender::FrameRender(std::shared_ptr<CD3DRender> pD3DRender)
    : m_pD3DRender(pD3DRender) {
    // Set safe defaults for tangents and eye-to-HMD
    HmdMatrix_SetIdentity(&m_eyeToHead[0]);
    HmdMatrix_SetIdentity(&m_eyeToHead[1]);
    m_viewProj[0] = { -1.0f, 1.0f, 1.0f, -1.0f };
    m_viewProj[1] = { -1.0f, 1.0f, 1.0f, -1.0f };

    FrameRender::SetGpuPriority(m_pD3DRender->GetDevice());
}

FrameRender::~FrameRender() { }

bool FrameRender::Startup(const hdr10::InputPlan& plan) {
    if (m_pStagingTexture) {
        return true;
    }

    // The initialized encoder determines the input contract, never the adapter.
    m_sdrP010 = plan.p010;
    m_floatSdr = plan.floatComposition;

    //
    // Create staging texture
    // This is input texture of Video Encoder and is render target of both eyes.
    //

    D3D11_TEXTURE2D_DESC compositionTextureDesc;
    ZeroMemory(&compositionTextureDesc, sizeof(compositionTextureDesc));
    compositionTextureDesc.Width = Settings::Instance().m_renderWidth;
    compositionTextureDesc.Height = Settings::Instance().m_renderHeight;
    compositionTextureDesc.Format = (Settings::Instance().m_enableHdr || m_floatSdr)
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    compositionTextureDesc.MipLevels = 1;
    compositionTextureDesc.ArraySize = 1;
    compositionTextureDesc.SampleDesc.Count = 1;
    compositionTextureDesc.Usage = D3D11_USAGE_DEFAULT;
    compositionTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> compositionTexture;

    if (FAILED(m_pD3DRender->GetDevice()->CreateTexture2D(
            &compositionTextureDesc, NULL, &compositionTexture
        ))) {
        Error("Failed to create staging texture!\n");
        return false;
    }

    HRESULT hr = m_pD3DRender->GetDevice()->CreateRenderTargetView(
        compositionTexture.Get(), NULL, &m_pRenderTargetView
    );
    if (FAILED(hr)) {
        Error("CreateRenderTargetView %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
    depthStencilDesc.DepthEnable = FALSE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depthStencilDesc.StencilEnable = FALSE;
    depthStencilDesc.StencilReadMask = 0xFF;
    depthStencilDesc.StencilWriteMask = 0xFF;

    // Stencil operations if pixel is front-facing
    depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
    depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    // Stencil operations if pixel is back-facing
    depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
    depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    m_pD3DRender->GetDevice()->CreateDepthStencilState(&depthStencilDesc, &m_depthStencilState);

    // Left eye viewport

    m_viewportL.Width = (float)Settings::Instance().m_renderWidth / 2.0;
    m_viewportL.Height = (float)Settings::Instance().m_renderHeight;
    m_viewportL.MinDepth = 0.0f;
    m_viewportL.MaxDepth = 1.0f;
    m_viewportL.TopLeftX = 0;
    m_viewportL.TopLeftY = 0;

    // Right eye viewport
    m_viewportR.Width = (float)Settings::Instance().m_renderWidth / 2.0;
    m_viewportR.Height = (float)Settings::Instance().m_renderHeight;
    m_viewportR.MinDepth = 0.0f;
    m_viewportR.MaxDepth = 1.0f;
    m_viewportR.TopLeftX = (float)Settings::Instance().m_renderWidth / 2.0;
    m_viewportR.TopLeftY = 0;

    // Final composition viewport
    m_viewport.Width = (float)Settings::Instance().m_renderWidth;
    m_viewport.Height = (float)Settings::Instance().m_renderHeight;
    m_viewport.MinDepth = 0.0f;
    m_viewport.MaxDepth = 1.0f;
    m_viewport.TopLeftX = 0;
    m_viewport.TopLeftY = 0;

    // Left eye scissor
    m_scissorL.bottom = 0.0f;
    m_scissorL.left = 0.0f;
    m_scissorL.right = (float)Settings::Instance().m_renderWidth / 2.0f;
    m_scissorL.top = (float)Settings::Instance().m_renderHeight;

    // Right eye scissor
    m_scissorR.bottom = 0.0f;
    m_scissorR.left = (float)Settings::Instance().m_renderWidth / 2.0f;
    m_scissorR.right = (float)Settings::Instance().m_renderWidth;
    m_scissorR.top = (float)Settings::Instance().m_renderHeight;

    // Final composition scissor
    m_scissor.bottom = 0.0f;
    m_scissor.left = 0.0f;
    m_scissor.right = (float)Settings::Instance().m_renderWidth;
    m_scissor.top = (float)Settings::Instance().m_renderHeight;

    //
    // Compile shaders
    //

    std::vector<uint8_t> vshader(
        FRAME_RENDER_VS_CSO_PTR, FRAME_RENDER_VS_CSO_PTR + FRAME_RENDER_VS_CSO_LEN
    );
    hr = m_pD3DRender->GetDevice()->CreateVertexShader(
        (const DWORD*)&vshader[0], vshader.size(), NULL, &m_pVertexShader
    );
    if (FAILED(hr)) {
        Error("CreateVertexShader %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    std::vector<uint8_t> pshader(
        FRAME_RENDER_PS_CSO_PTR, FRAME_RENDER_PS_CSO_PTR + FRAME_RENDER_PS_CSO_LEN
    );
    if (Settings::Instance().m_hdrPq)
        pshader.assign(PQ_FRAME_SHADER, PQ_FRAME_SHADER + sizeof(PQ_FRAME_SHADER));
    hr = m_pD3DRender->GetDevice()->CreatePixelShader(
        (const DWORD*)&pshader[0], pshader.size(), NULL, &m_pPixelShader
    );
    if (FAILED(hr)) {
        Error("CreatePixelShader %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    //
    // Create input layout
    //

    // Define the input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "VIEW", 0, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    UINT numElements = ARRAYSIZE(layout);

    // Create the input layout
    hr = m_pD3DRender->GetDevice()->CreateInputLayout(
        layout, numElements, &vshader[0], vshader.size(), &m_pVertexLayout
    );
    if (FAILED(hr)) {
        Error("CreateInputLayout %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // Set the input layout
    m_pD3DRender->GetContext()->IASetInputLayout(m_pVertexLayout.Get());

    //
    // Create frame render CBuffer
    //
    struct FrameRenderBuffer {
        float encodingGamma;
        float pqEnabled;
        float _align1;
        float _align2;
    };
    FrameRenderBuffer frameRenderStruct
        = { (float)(1.0 / Settings::Instance().m_encodingGamma),
            Settings::Instance().m_hdrPq ? 1.0f : 0.0f, 0.0f, 0.0f };
    m_pFrameRenderCBuffer = CreateBuffer(m_pD3DRender->GetDevice(), frameRenderStruct);

    //
    // Create vertex buffer
    //

    // Src texture has various geometry and we should use the part of the textures.
    // That part are defined by uv-coordinates of "bounds" passed to
    // IVRDriverDirectModeComponent::SubmitLayer. So we should update uv-coordinates for every
    // frames and layers.
    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(SimpleVertex) * 8;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_pD3DRender->GetDevice()->CreateBuffer(&bd, NULL, &m_pVertexBuffer);
    if (FAILED(hr)) {
        Error("CreateBuffer 1 %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // Set vertex buffer
    UINT stride = sizeof(SimpleVertex);
    UINT offset = 0;
    m_pD3DRender->GetContext()->IASetVertexBuffers(
        0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset
    );

    //
    // Create index buffer
    //

    WORD indices[] = { 0, 1, 2, 0, 3, 1,

                       4, 5, 6, 4, 7, 5 };

    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA InitData;
    ZeroMemory(&InitData, sizeof(InitData));
    InitData.pSysMem = indices;

    hr = m_pD3DRender->GetDevice()->CreateBuffer(&bd, &InitData, &m_pIndexBuffer);
    if (FAILED(hr)) {
        Error("CreateBuffer 2 %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // Set index buffer
    m_pD3DRender->GetContext()->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

    // Set primitive topology
    m_pD3DRender->GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Create the sample state
    D3D11_SAMPLER_DESC sampDesc;
    ZeroMemory(&sampDesc, sizeof(sampDesc));
    sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.MaxAnisotropy = D3D11_REQ_MAXANISOTROPY;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_pD3DRender->GetDevice()->CreateSamplerState(&sampDesc, &m_pSamplerLinear);
    if (FAILED(hr)) {
        Error("CreateSamplerState %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    //
    // Create alpha blend state
    // We need alpha blending to support layer.
    //

    // BlendState for first layer.
    // Some VR apps (like SteamVR Home beta) submit the texture that alpha is zero on all pixels.
    // So we need to ignore alpha of first layer.
    D3D11_BLEND_DESC BlendDesc;
    ZeroMemory(&BlendDesc, sizeof(BlendDesc));
    BlendDesc.AlphaToCoverageEnable = FALSE;
    BlendDesc.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
        BlendDesc.RenderTarget[i].BlendEnable = TRUE;
        BlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
        BlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
        BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
        BlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
            | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    }

    hr = m_pD3DRender->GetDevice()->CreateBlendState(&BlendDesc, &m_pBlendStateFirst);
    if (FAILED(hr)) {
        Error("CreateBlendState %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // BleandState for other layers than first.
    BlendDesc.AlphaToCoverageEnable = FALSE;
    BlendDesc.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
        BlendDesc.RenderTarget[i].BlendEnable = TRUE;
        BlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        BlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
        BlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }

    hr = m_pD3DRender->GetDevice()->CreateBlendState(&BlendDesc, &m_pBlendState);
    if (FAILED(hr)) {
        Error("CreateBlendState %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    m_pStagingTexture = compositionTexture;

    std::vector<uint8_t> quadShaderCSO(
        QUAD_SHADER_CSO_PTR, QUAD_SHADER_CSO_PTR + QUAD_SHADER_CSO_LEN
    );
    ComPtr<ID3D11VertexShader> quadVertexShader
        = CreateVertexShader(m_pD3DRender->GetDevice(), quadShaderCSO);

    enableColorCorrection = Settings::Instance().m_enableColorCorrection;
    if (enableColorCorrection) {
        std::vector<uint8_t> colorCorrectionShaderCSO(
            COLOR_CORRECTION_CSO_PTR, COLOR_CORRECTION_CSO_PTR + COLOR_CORRECTION_CSO_LEN
        );
        if (Settings::Instance().m_hdrPq)
            colorCorrectionShaderCSO.assign(
                PQ_CORRECTION_SHADER, PQ_CORRECTION_SHADER + sizeof(PQ_CORRECTION_SHADER));

        ComPtr<ID3D11Texture2D> colorCorrectedTexture = CreateTexture(
            m_pD3DRender->GetDevice(),
            Settings::Instance().m_renderWidth,
            Settings::Instance().m_renderHeight,
            (Settings::Instance().m_enableHdr || m_floatSdr) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                             : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        );

        struct ColorCorrection {
            float renderWidth;
            float renderHeight;
            float brightness;
            float contrast;
            float saturation;
            float gamma;
            float sharpening;
            float pqEnabled;
        };
        ColorCorrection colorCorrectionStruct = {
            (float)Settings::Instance().m_renderWidth, (float)Settings::Instance().m_renderHeight,
            Settings::Instance().m_brightness,         Settings::Instance().m_contrast + 1.f,
            Settings::Instance().m_saturation + 1.f,   Settings::Instance().m_gamma,
            Settings::Instance().m_sharpening, Settings::Instance().m_hdrPq ? 1.0f : 0.0f
        };
        ComPtr<ID3D11Buffer> colorCorrectionBuffer
            = CreateBuffer(m_pD3DRender->GetDevice(), colorCorrectionStruct);

        m_colorCorrectionPipeline = std::make_unique<RenderPipeline>(m_pD3DRender->GetDevice());
        m_colorCorrectionPipeline->Initialize(
            { m_pStagingTexture.Get() },
            quadVertexShader.Get(),
            colorCorrectionShaderCSO,
            colorCorrectedTexture.Get(),
            colorCorrectionBuffer.Get()
        );

        m_pStagingTexture = colorCorrectedTexture;
    }

    enableFFE = Settings::Instance().m_enableFoveatedEncoding;
    if (enableFFE) {
        m_ffr = std::make_unique<FFR>(m_pD3DRender->GetDevice());
        m_ffr->Initialize(m_pStagingTexture.Get());

        m_pStagingTexture = m_ffr->GetOutputTexture();
    }

    m_chartInput = m_pStagingTexture; // RGB encoder input, before any YUV conversion.

    if (Settings::Instance().m_enableHdr && !Settings::Instance().m_hdrPq) {
        std::vector<uint8_t> yuv420ShaderCSO(
            RGBTOYUV420_CSO_PTR, RGBTOYUV420_CSO_PTR + RGBTOYUV420_CSO_LEN
        );
        uint32_t texWidth, texHeight;
        GetEncodingResolution(&texWidth, &texHeight);

        ComPtr<ID3D11Texture2D> yuvTexture = CreateTexture(
            m_pD3DRender->GetDevice(),
            texWidth,
            texHeight,
            Settings::Instance().m_use10bitEncoder ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12
        );

        struct YUVParams {
            float offset[4];
            float yCoeff[4];
            float uCoeff[4];
            float vCoeff[4];

            float renderWidth;
            float renderHeight;
            float _padding0;
            float _padding1;
        };

        // Bless this page for ending my stint of plugging in random values
        // from other projects:
        // https://kdashg.github.io/misc/colors/from-coeffs.html
        YUVParams paramStruct_bt2020_8bit_full
            = { { 0.0000000f, 0.5019608f, 0.5019608f, 0.0f }, // offset
                { 0.2627000f, 0.6780000f, 0.0593000f, 0.0f }, // yCoeff
                { -0.1390825f, -0.3589567f, 0.4980392f, 0.0f }, // uCoeff
                { 0.4980392f, -0.4579826f, -0.0400566f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams paramStruct_bt2020_10bit_full
            = { { 0.0000000f, 0.5004888f, 0.5004888f, 0.0f }, // offset
                { 0.2627000f, 0.6780000f, 0.0593000f, 0.0f }, // yCoeff
                { -0.1394936f, -0.3600177f, 0.4995112f, 0.0f }, // uCoeff
                { 0.4995112f, -0.4593363f, -0.0401750f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams paramStruct_bt2020_8bit_limited
            = { { 0.0627451f, 0.5019608f, 0.5019608f, 0.0f }, // offset
                { 0.2256129f, 0.5822824f, 0.0509282f, 0.0f }, // yCoeff
                { -0.1226554f, -0.3165603f, 0.4392157f, 0.0f }, // uCoeff
                { 0.4392157f, -0.4038902f, -0.0353255f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams paramStruct_bt2020_10bit_limited
            = { { 0.0625611f, 0.5004888f, 0.5004888f, 0.0f }, // offset
                { 0.2249513f, 0.5805748f, 0.0507789f, 0.0f }, // yCoeff
                { -0.1222957f, -0.3156319f, 0.4379277f, 0.0f }, // uCoeff
                { 0.4379277f, -0.4027058f, -0.0352219f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams& paramStruct = paramStruct_bt2020_8bit_full;
        if (Settings::Instance().m_use10bitEncoder) {
            if (Settings::Instance().m_useFullRangeEncoding) {
                paramStruct = paramStruct_bt2020_10bit_full;
            } else {
                paramStruct = paramStruct_bt2020_10bit_limited;
            }
        } else {
            if (Settings::Instance().m_useFullRangeEncoding) {
                paramStruct = paramStruct_bt2020_8bit_full;
            } else {
                paramStruct = paramStruct_bt2020_8bit_limited;
            }
        }

        ComPtr<ID3D11Buffer> paramBuffer = CreateBuffer(m_pD3DRender->GetDevice(), paramStruct);

        m_yuvPipeline = std::make_unique<RenderPipelineYUV>(m_pD3DRender->GetDevice());
        m_yuvPipeline->Initialize(
            { m_pStagingTexture.Get() },
            quadVertexShader.Get(),
            yuv420ShaderCSO,
            yuvTexture.Get(),
            paramBuffer.Get()
        );

        m_pStagingTexture = yuvTexture;
    }

    if (m_sdrP010) {
        InitializeSdrP010(quadVertexShader.Get());
    }

    Debug("Staging Texture created\n");

    return true;
}

void FrameRender::InitializeSdrP010(ID3D11VertexShader* quadVertexShader) {
    uint32_t width, height;
    GetEncodingResolution(&width, &height);
    ComPtr<ID3D11Texture2D> texture;
    texture.Attach(CreateTexture(
        m_pD3DRender->GetDevice(), width, height, DXGI_FORMAT_P010
    ));
    const bool full = Settings::Instance().m_useFullRangeEncoding;
    ComPtr<ID3D11Buffer> buffer;
    std::vector<uint8_t> shader(SDR_P010_SHADER, SDR_P010_SHADER + sizeof(SDR_P010_SHADER));
    if (Settings::Instance().m_hdrPq) {
        const auto params = hdr10::MakeShaderParams(Settings::Instance().m_hdrReferenceWhiteNits);
        buffer.Attach(CreateBuffer(m_pD3DRender->GetDevice(), params));
        shader.assign(PQ_P010_SHADER, PQ_P010_SHADER + sizeof(PQ_P010_SHADER));
    } else {
        struct SdrP010Params { float yOffset, yScale, chromaScale, padding; };
        const SdrP010Params params = {
            full ? 0.0f : 64.0f, full ? 1023.0f : 876.0f, full ? 1023.0f : 896.0f, 0.0f
        };
        buffer.Attach(CreateBuffer(m_pD3DRender->GetDevice(), params));
    }
    m_yuvPipeline = std::make_unique<RenderPipelineYUV>(m_pD3DRender->GetDevice());
    m_yuvPipeline->Initialize(
        { m_pStagingTexture.Get() }, quadVertexShader, shader, texture.Get(), buffer.Get()
    );
    m_yuvPipeline->UseSeparatePlanePasses();
    m_pStagingTexture = texture;
    if (Settings::Instance().m_hdrPq)
        Info("HDR10 pipeline: float16 composition/correction/FFR -> BT.2020/PQ limited P010, white=%.0f nits",
             Settings::Instance().m_hdrReferenceWhiteNits);
    else Info("SDR pipeline: composition/correction/FFR=%s -> P010 (YUV420_10BIT), BT.709/sRGB, %s range",
         m_floatSdr ? "R16G16B16A16_FLOAT" : "R8G8B8A8_UNORM_SRGB", full ? "full" : "limited");
}

void FrameRender::SetViewsConfig(
    vr::HmdRect2_t projLeft,
    vr::HmdMatrix34_t eyeToHeadLeft,
    vr::HmdRect2_t projRight,
    vr::HmdMatrix34_t eyeToHeadRight
) {
    std::lock_guard<std::mutex> lock(m_viewsMutex);
    m_chartDirty = m_chartDirty || memcmp(m_viewProj, &projLeft, sizeof(projLeft)) != 0
        || memcmp(&m_viewProj[1], &projRight, sizeof(projRight)) != 0;
    m_viewProj[0] = projLeft;
    m_eyeToHead[0] = eyeToHeadLeft;
    m_viewProj[1] = projRight;
    m_eyeToHead[1] = eyeToHeadRight;
}

bool FrameRender::RenderFrame(
    ID3D11Texture2D* pTexture[][2],
    vr::VRTextureBounds_t bounds[][2],
    vr::HmdMatrix34_t poses[],
    int layerCount,
    bool recentering,
    const std::string& message,
    const std::string& debugText
) {
    std::lock_guard<std::mutex> lock(m_viewsMutex); // One projection snapshot for this frame.
    // Set render target
    m_pD3DRender->GetContext()->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), NULL);

    m_pD3DRender->GetContext()->OMSetDepthStencilState(m_depthStencilState.Get(), 0);

    // Clear the back buffer
    m_pD3DRender->GetContext()->ClearRenderTargetView(
        m_pRenderTargetView.Get(), DirectX::Colors::MidnightBlue
    );

    // Overlay recentering texture on top of all layers.
    int recenterLayer = -1;
    if (recentering) {
        recenterLayer = layerCount;
        layerCount++;
    }

    // Set up our projection, HMD, and HMD-to-eye transforms once
    const auto nearZ = 0.001f;
    const auto farZ = 1.0f;
    DirectX::XMMATRIX projectionMatL = DirectX::XMMatrixPerspectiveOffCenterRH(
        m_viewProj[0].vTopLeft.v[0] * nearZ,
        m_viewProj[0].vBottomRight.v[0] * nearZ,
        -m_viewProj[0].vTopLeft.v[1] * nearZ,
        -m_viewProj[0].vBottomRight.v[1] * nearZ,
        nearZ,
        farZ
    );
    DirectX::XMMATRIX projectionMatR = DirectX::XMMatrixPerspectiveOffCenterRH(
        m_viewProj[1].vTopLeft.v[0] * nearZ,
        m_viewProj[1].vBottomRight.v[0] * nearZ,
        -m_viewProj[1].vTopLeft.v[1] * nearZ,
        -m_viewProj[1].vBottomRight.v[1] * nearZ,
        nearZ,
        farZ
    );
    DirectX::XMMATRIX hmdToEyeMatL
        = DirectX::XMMatrixInverse(nullptr, HmdMatrix_AsDxMatPosOnly(m_eyeToHead[0]));
    DirectX::XMMATRIX hmdToEyeMatR
        = DirectX::XMMatrixInverse(nullptr, HmdMatrix_AsDxMatPosOnly(m_eyeToHead[1]));
    DirectX::XMMATRIX hmdPoseForTargetTs
        = HmdMatrix_AsDxMatOrientOnly(poses[0]); // Set to HmdMatrix_AsDxMat to debug the rendering

    // I think the negative Y basis is a handedness thing?
    DirectX::XMMATRIX identityMat = DirectX::XMLoadFloat4x4(&_identityMat);

    for (int i = 0; i < layerCount; i++) {
        ID3D11Texture2D* textures[2];
        vr::VRTextureBounds_t bound[2];

        if (i == recenterLayer) {
            textures[0] = (ID3D11Texture2D*)m_recenterTexture.Get();
            textures[1] = (ID3D11Texture2D*)m_recenterTexture.Get();
            bound[0].uMin = bound[0].vMin = bound[1].uMin = bound[1].vMin = 0.0f;
            bound[0].uMax = bound[0].vMax = bound[1].uMax = bound[1].vMax = 1.0f;
        } else {
            textures[0] = pTexture[i][0];
            textures[1] = pTexture[i][1];
            bound[0] = bounds[i][0];
            bound[1] = bounds[i][1];
        }
        if (textures[0] == NULL || textures[1] == NULL) {
            Debug(
                "Ignore NULL layer. layer=%d/%d%s%s\n",
                i,
                layerCount,
                recentering ? L" (recentering)" : L"",
                !message.empty() ? L" (message)" : L""
            );
            continue;
        }

        D3D11_TEXTURE2D_DESC srcDesc;
        textures[0]->GetDesc(&srcDesc);

        D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
        SRVDesc.Format = srcDesc.Format;
        SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        SRVDesc.Texture2D.MipLevels = 1;

        ComPtr<ID3D11ShaderResourceView> pShaderResourceView[2];

        HRESULT hr = m_pD3DRender->GetDevice()->CreateShaderResourceView(
            textures[0], &SRVDesc, pShaderResourceView[0].ReleaseAndGetAddressOf()
        );
        if (FAILED(hr)) {
            Error("CreateShaderResourceView %p %ls\n", hr, GetErrorStr(hr).c_str());
            return false;
        }
        hr = m_pD3DRender->GetDevice()->CreateShaderResourceView(
            textures[1], &SRVDesc, pShaderResourceView[1].ReleaseAndGetAddressOf()
        );
        if (FAILED(hr)) {
            Error("CreateShaderResourceView %p %ls\n", hr, GetErrorStr(hr).c_str());
            return false;
        }

        if (i == 0) {
            m_pD3DRender->GetContext()->OMSetBlendState(m_pBlendStateFirst.Get(), NULL, 0xffffffff);
        } else {
            m_pD3DRender->GetContext()->OMSetBlendState(m_pBlendState.Get(), NULL, 0xffffffff);
        }

        int inputColorAdjust = 0;
        if (Settings::Instance().m_enableHdr) {
            if (SRVDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                || SRVDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                || SRVDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
                inputColorAdjust = 1; // do sRGB manually
            }
            if (Settings::Instance().m_forceHdrSrgbCorrection) {
                inputColorAdjust = 1;
            }
            if (Settings::Instance().m_clampHdrExtendedRange) {
                inputColorAdjust |= 0x10; // Clamp values to 0.0 to 1.0
            }
        } else {
            if (SRVDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                && SRVDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                && SRVDesc.Format != DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
                inputColorAdjust = 2; // undo sRGB?

                if (Settings::Instance().m_forceHdrSrgbCorrection) {
                    inputColorAdjust = 0;
                }
            }

            if (Settings::Instance().m_clampHdrExtendedRange) {
                inputColorAdjust |= 0x10; // Clamp values to 0.0 to 1.0
            }
        }

        //
        // Update uv-coordinates in vertex buffer according to bounds.
        //

        DirectX::XMMATRIX framePose
            = (i == recenterLayer) ? identityMat : HmdMatrix_AsDxMatOrientOnly(poses[i]);
        DirectX::XMMATRIX framePoseInv = DirectX::XMMatrixInverse(nullptr, framePose);

        // framePose is the position of the layer in space, ie an identity matrix
        // would place the quad perpendicular in the floor at 0,0,0
        DirectX::XMMATRIX viewMatDiff
            = DirectX::XMMatrixInverse(nullptr, hmdPoseForTargetTs * framePoseInv);

        DirectX::XMMATRIX transformMatL = viewMatDiff * hmdToEyeMatL * projectionMatL;
        DirectX::XMMATRIX transformMatR = viewMatDiff * hmdToEyeMatR * projectionMatR;

        if (i == recenterLayer) {
            transformMatL = identityMat;
            transformMatR = identityMat;
        }

        const auto depth = 700.0f;
        const auto m = 1.0f;
        DirectX::XMFLOAT4 vertsL[4];
        DirectX::XMFLOAT4 vertsR[4];

        DirectX::XMVECTORF32 vertsL_VF32[4]
            = { { { { -1.0f * -m_viewProj[0].vTopLeft.v[0] * depth * m,
                      1.0f * -m_viewProj[0].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[0].vBottomRight.v[0] * depth * m,
                      -1.0f * m_viewProj[0].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[0].vBottomRight.v[0] * depth * m,
                      1.0f * -m_viewProj[0].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { -1.0f * -m_viewProj[0].vTopLeft.v[0] * depth * m,
                      -1.0f * m_viewProj[0].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } } };

        for (int i = 0; i < 4; i++) {
            DirectX::XMStoreFloat4(
                &vertsL[i], DirectX::XMVector3Transform(vertsL_VF32[i], transformMatL)
            );
        }

        DirectX::XMVECTORF32 vertsR_VF32[4]
            = { { { { -1.0f * -m_viewProj[1].vTopLeft.v[0] * depth * m,
                      1.0f * -m_viewProj[1].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[1].vBottomRight.v[0] * depth * m,
                      -1.0f * m_viewProj[1].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[1].vBottomRight.v[0] * depth * m,
                      1.0f * -m_viewProj[1].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { -1.0f * -m_viewProj[1].vTopLeft.v[0] * depth * m,
                      -1.0f * m_viewProj[1].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } } };

        for (int i = 0; i < 4; i++) {
            DirectX::XMStoreFloat4(
                &vertsR[i], DirectX::XMVector3Transform(vertsR_VF32[i], transformMatR)
            );
        }

        // We discard the z value because we never want any clipping,
        // but we do want the w value for perspective correction.
        SimpleVertex vertices[] = {
            // Left View
            { DirectX::XMFLOAT4(vertsL[0].x, vertsL[0].y, 0.5, vertsL[0].w),
              DirectX::XMFLOAT2(bound[0].uMin, bound[0].vMax),
              0 + (inputColorAdjust * 2) },
            { DirectX::XMFLOAT4(vertsL[1].x, vertsL[1].y, 0.5, vertsL[1].w),
              DirectX::XMFLOAT2(bound[0].uMax, bound[0].vMin),
              0 + (inputColorAdjust * 2) },
            { DirectX::XMFLOAT4(vertsL[2].x, vertsL[2].y, 0.5, vertsL[2].w),
              DirectX::XMFLOAT2(bound[0].uMax, bound[0].vMax),
              0 + (inputColorAdjust * 2) },
            { DirectX::XMFLOAT4(vertsL[3].x, vertsL[3].y, 0.5, vertsL[3].w),
              DirectX::XMFLOAT2(bound[0].uMin, bound[0].vMin),
              0 + (inputColorAdjust * 2) },
            // Right View
            { DirectX::XMFLOAT4(vertsR[0].x, vertsR[0].y, 0.5, vertsR[0].w),
              DirectX::XMFLOAT2(bound[1].uMin, bound[1].vMax),
              1 + (inputColorAdjust * 2) },
            { DirectX::XMFLOAT4(vertsR[1].x, vertsR[1].y, 0.5, vertsR[1].w),
              DirectX::XMFLOAT2(bound[1].uMax, bound[1].vMin),
              1 + (inputColorAdjust * 2) },
            { DirectX::XMFLOAT4(vertsR[2].x, vertsR[2].y, 0.5, vertsR[2].w),
              DirectX::XMFLOAT2(bound[1].uMax, bound[1].vMax),
              1 + (inputColorAdjust * 2) },
            { DirectX::XMFLOAT4(vertsR[3].x, vertsR[3].y, 0.5, vertsR[3].w),
              DirectX::XMFLOAT2(bound[1].uMin, bound[1].vMin),
              1 + (inputColorAdjust * 2) },
        };

        D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
        hr = m_pD3DRender->GetContext()->Map(
            m_pVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped
        );
        if (FAILED(hr)) {
            Error("Map %p %ls\n", hr, GetErrorStr(hr).c_str());
            return false;
        }
        memcpy(mapped.pData, vertices, sizeof(vertices));

        m_pD3DRender->GetContext()->Unmap(m_pVertexBuffer.Get(), 0);

        // Set the input layout
        m_pD3DRender->GetContext()->IASetInputLayout(m_pVertexLayout.Get());

        //
        // Set buffers
        //

        UINT stride = sizeof(SimpleVertex);
        UINT offset = 0;
        m_pD3DRender->GetContext()->IASetVertexBuffers(
            0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset
        );

        m_pD3DRender->GetContext()->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        m_pD3DRender->GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pD3DRender->GetContext()->PSSetConstantBuffers(
            0, 1, m_pFrameRenderCBuffer.GetAddressOf()
        );

        //
        // Set shaders
        //

        m_pD3DRender->GetContext()->VSSetShader(m_pVertexShader.Get(), nullptr, 0);
        m_pD3DRender->GetContext()->PSSetShader(m_pPixelShader.Get(), nullptr, 0);

        ID3D11ShaderResourceView* shaderResourceView[2]
            = { pShaderResourceView[0].Get(), pShaderResourceView[1].Get() };
        m_pD3DRender->GetContext()->PSSetShaderResources(0, 2, shaderResourceView);

        m_pD3DRender->GetContext()->PSSetSamplers(0, 1, m_pSamplerLinear.GetAddressOf());

        //
        // Draw
        //

        // Left eye
        m_pD3DRender->GetContext()->RSSetViewports(1, &m_viewportL);
        m_pD3DRender->GetContext()->RSSetScissorRects(1, &m_scissorL);
        m_pD3DRender->GetContext()->DrawIndexed(VERTEX_INDEX_COUNT / 2, 0, 0);

        // Right eye
        m_pD3DRender->GetContext()->RSSetViewports(1, &m_viewportR);
        m_pD3DRender->GetContext()->RSSetScissorRects(1, &m_scissorR);
        m_pD3DRender->GetContext()->DrawIndexed(
            VERTEX_INDEX_COUNT / 2, (VERTEX_INDEX_COUNT / 2), 0
        );
    }

    // Restore full viewport/scissor rect for the rest
    m_pD3DRender->GetContext()->RSSetViewports(1, &m_viewport);
    m_pD3DRender->GetContext()->RSSetScissorRects(1, &m_scissor);

    m_chartDump.Poll(m_pD3DRender->GetContext());
    RenderAngleChart();

    if (enableColorCorrection) {
        m_colorCorrectionPipeline->Render();
    }

    if (enableFFE) {
        m_ffr->Render();
    }

    if (m_chartDump.Started()) {
        m_chartDump.Capture(m_pD3DRender->GetDevice(), m_pD3DRender->GetContext(),
                           m_chartInput.Get(), 1);
        m_chartDump.armed = false;
    }

    if (Settings::Instance().m_enableHdr || m_sdrP010) {
        m_yuvPipeline->Render();
    }

    m_pD3DRender->GetContext()->Flush();

    return true;
}

ComPtr<ID3D11Texture2D> FrameRender::GetTexture() { return m_pStagingTexture; }

void FrameRender::GetEncodingResolution(uint32_t* width, uint32_t* height) {
    if (Settings::Instance().m_enableFoveatedEncoding) {
        FFR::GetOptimizedResolution(width, height);
    } else {
        *width = Settings::Instance().m_renderWidth;
        *height = Settings::Instance().m_renderHeight;
    }
}

void FrameRender::RenderAngleChart() {
    const auto now = std::chrono::steady_clock::now();
    if (now >= m_chartPoll) {
        m_chartPoll = now + std::chrono::seconds(1);
        std::ifstream flag(std::filesystem::path(g_sessionPath).parent_path() / "angle_chart.enable");
        const bool enabled = flag.good();
        if (enabled != m_chartEnabled) {
            std::string command, word;
            flag >> command;
            bool dump = false, stress = false;
            for (std::istringstream words(command); std::getline(words, word, ',');) {
                dump = dump || word == "dump";
                stress = stress || word == "stress";
            }
            m_chartDump.armed = enabled && dump;
            m_chartStress = enabled && stress;
            m_chartEnabled = enabled;
            m_chartDirty = true;
            Info("angle_chart enabled=%d stress=%d", enabled, m_chartStress);
        }
    }
    if (!m_chartEnabled) return;
    auto device = m_pD3DRender->GetDevice();
    auto context = m_pD3DRender->GetContext();
    if (!m_chartPS) {
        OK_OR_THROW(device->CreatePixelShader(ANGLE_CHART_SHADER, sizeof(ANGLE_CHART_SHADER),
                    nullptr, &m_chartPS), "Angle chart pixel shader");
        OK_OR_THROW(device->CreateVertexShader(QUAD_SHADER_CSO_PTR, QUAD_SHADER_CSO_LEN,
                    nullptr, &m_chartVS), "Angle chart vertex shader");
    }
    if (m_chartDirty) {
        auto& chart = m_chart;
        chart.size[0] = m_viewportL.Width;
        chart.size[1] = m_viewportL.Height;
        uint32_t codedW, codedH;
        GetEncodingResolution(&codedW, &codedH);
        const auto& s = Settings::Instance();
        // Colours are display-referred SDR; encode them like FrameRender.fx leaves normal frames.
        chart.encode[0] = float(s.m_hdrPq ? ANGLE_CHART_PQ : s.m_enableHdr ? ANGLE_CHART_HDR : ANGLE_CHART_SDR);
        chart.encode[1] = float(1.0 / s.m_encodingGamma);
        chart.encode[2] = m_chartStress ? 1.f : 0.f;
        for (int eye = 0; eye < 2; ++eye) {
            const auto& p = m_viewProj[eye];
            auto& t = chart.tangents[eye];
            t[0] = p.vTopLeft.v[0]; t[1] = p.vBottomRight.v[0];
            t[2] = p.vTopLeft.v[1]; t[3] = p.vBottomRight.v[1];
            Info("angle_chart eye=%s w=%.0f h=%.0f tanL=%.9g tanR=%.9g tanD=%.9g tanU=%.9g "
                 "foveation=%d centerSize=%.9g,%.9g centerShift=%.9g,%.9g edgeRatio=%.9g,%.9g coded=%ux%u",
                 eye ? "R" : "L", chart.size[0], chart.size[1], t[0], t[1], t[2], t[3],
                 enableFFE, s.m_foveationCenterSizeX, s.m_foveationCenterSizeY,
                 s.m_foveationCenterShiftX, s.m_foveationCenterShiftY,
                 s.m_foveationEdgeRatioX, s.m_foveationEdgeRatioY, codedW, codedH);
        }
        Info("angle_chart encode mode=%.0f invGamma=%.9g stress=%d", chart.encode[0], chart.encode[1],
             m_chartStress);
        m_chartBuffer.Attach(CreateBuffer(device, chart, D3D11_USAGE_DEFAULT));
        m_chartDirty = false;
    } else if (m_chartStress) {
        m_chart.encode[3] = float((unsigned(m_chart.encode[3]) + 1) & 0xffffff); // exact in float
        UpdateBuffer(context, m_chartBuffer.Get(), &m_chart);
    }
    context->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->PSSetConstantBuffers(0, 1, m_chartBuffer.GetAddressOf());
    context->VSSetShader(m_chartVS.Get(), nullptr, 0);
    context->PSSetShader(m_chartPS.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->Draw(4, 0);
    if (m_chartDump.armed && !m_chartDump.Busy()) {
        ComPtr<ID3D11Resource> resource;
        m_pRenderTargetView->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);
        const auto& s = Settings::Instance();
        uint32_t w, h; GetEncodingResolution(&w, &h);
        char meta[2048];
        const auto& l = m_viewProj[0]; const auto& r = m_viewProj[1];
        snprintf(meta, sizeof(meta), "{\"tangents\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]],"
            "\"renderEyeSize\":[%.0f,%.0f],\"foveation\":%s,\"centerSize\":[%.9g,%.9g],"
            "\"centerShift\":[%.9g,%.9g],\"edgeRatio\":[%.9g,%.9g],\"codedSize\":[%u,%u]}",
            l.vTopLeft.v[0], l.vBottomRight.v[0], l.vTopLeft.v[1], l.vBottomRight.v[1],
            r.vTopLeft.v[0], r.vBottomRight.v[0], r.vTopLeft.v[1], r.vBottomRight.v[1],
            m_viewportL.Width, m_viewportL.Height, enableFFE ? "true" : "false",
            s.m_foveationCenterSizeX, s.m_foveationCenterSizeY, s.m_foveationCenterShiftX,
            s.m_foveationCenterShiftY, s.m_foveationEdgeRatioX, s.m_foveationEdgeRatioY, w, h);
        m_chartDump.Capture(device, context, texture.Get(), 0, meta);
    }
}
