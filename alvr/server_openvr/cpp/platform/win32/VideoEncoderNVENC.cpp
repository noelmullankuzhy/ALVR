#include "VideoEncoderNVENC.h"
#include "NvCodecUtils.h"

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/Utils.h"

// NV_ENC_CAPS_NUM_ENCODER_ENGINES and NV_ENC_INITIALIZE_PARAMS::splitEncodeMode need API 12.1;
// NvEncoder::LoadNvEncApi already refuses drivers older than the bundled header.
static_assert(
    NVENCAPI_MAJOR_VERSION > 12 || (NVENCAPI_MAJOR_VERSION == 12 && NVENCAPI_MINOR_VERSION >= 1),
    "NVENC split encoding needs API 12.1"
);

// alvr_session NvencSplitEncodeMode::TwoIfAvailable; not an NV_ENC_SPLIT_ENCODE_MODE value.
static const uint32_t SPLIT_TWO_IF_AVAILABLE = 16;

static const char* SplitModeName(uint32_t mode) {
    switch (mode) {
    case NV_ENC_SPLIT_AUTO_MODE:
        return "Auto";
    case NV_ENC_SPLIT_TWO_FORCED_MODE:
        return "ForceTwo";
    case NV_ENC_SPLIT_DISABLE_MODE:
        return "Disabled";
    case SPLIT_TWO_IF_AVAILABLE:
        return "TwoIfAvailable";
    default:
        return "Other";
    }
}

static void Validate10bitInput(ID3D11Texture2D* texture) {
    if (Settings::Instance().m_use10bitEncoder
        && (!Settings::Instance().m_enableHdr || Settings::Instance().m_hdrPq)) {
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_P010) {
            throw MakeException("10-bit NVENC input must be P010; got DXGI format %u", desc.Format);
        }
    }
}

VideoEncoderNVENC::VideoEncoderNVENC(std::shared_ptr<CD3DRender> pD3DRender, int width, int height)
    : m_pD3DRender(pD3DRender)
    , m_codec(Settings::Instance().m_codec)
    , m_refreshRate(Settings::Instance().m_refreshRate)
    , m_renderWidth(width)
    , m_renderHeight(height)
    , m_bitrateInMBits(30) { }

VideoEncoderNVENC::~VideoEncoderNVENC() { }

void VideoEncoderNVENC::Initialize() {
    //
    // Initialize Encoder
    //

    NV_ENC_BUFFER_FORMAT format
        = Settings::Instance().m_enableHdr ? NV_ENC_BUFFER_FORMAT_NV12 : NV_ENC_BUFFER_FORMAT_ABGR;

    if (Settings::Instance().m_use10bitEncoder) {
        // SDR uses an explicit BT.709 conversion too, even with 8-bit composition.
        format = NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    }

    Debug(
        "Initializing CNvEncoder. Width=%d Height=%d Format=%d\n",
        m_renderWidth,
        m_renderHeight,
        format
    );

    try {
        m_NvNecoder = std::make_shared<NvEncoderD3D11>(
            m_pD3DRender->GetDevice(), m_renderWidth, m_renderHeight, format, 0
        );
    } catch (NVENCException e) {
        throw MakeException(
            "NvEnc NvEncoderD3D11 failed. Code=%d %hs\n", e.getErrorCode(), e.what()
        );
    }

    // The session is open (NvEncoder constructor), so caps can be queried before CreateEncoder.
    GUID codecGuid = m_codec == ALVR_CODEC_AV1 ? NV_ENC_CODEC_AV1_GUID
        : m_codec == ALVR_CODEC_HEVC           ? NV_ENC_CODEC_HEVC_GUID
                                               : NV_ENC_CODEC_H264_GUID;
    m_nvencEngineCount
        = m_NvNecoder->GetCapabilityValue(codecGuid, NV_ENC_CAPS_NUM_ENCODER_ENGINES);
    Info("NVENC engines: %d (NV_ENC_CAPS_NUM_ENCODER_ENGINES)\n", m_nvencEngineCount);

    NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
    initializeParams.encodeConfig = &encodeConfig;

    FillEncodeConfig(
        initializeParams,
        m_refreshRate,
        m_renderWidth,
        m_renderHeight,
        m_bitrateInMBits * 1'000'000L
    );
    Info(
        "NVENC API %u.%u: split setting=%s requested=%s, preset=P%u, tuning=%u, coded=%ux%u, "
        "codec=%d\n",
        NVENCAPI_MAJOR_VERSION,
        NVENCAPI_MINOR_VERSION,
        SplitModeName(Settings::Instance().m_nvencSplitEncodeMode),
        SplitModeName(initializeParams.splitEncodeMode),
        Settings::Instance().m_nvencQualityPreset,
        initializeParams.tuningInfo,
        initializeParams.encodeWidth,
        initializeParams.encodeHeight,
        m_codec
    );
    try {
        m_NvNecoder->CreateEncoder(&initializeParams);
    } catch (NVENCException e) {
        if (e.getErrorCode() == NV_ENC_ERR_INVALID_PARAM) {
            throw MakeException(
                "This GPU does not support H.265 encoding. (NvEncoderCuda NV_ENC_ERR_INVALID_PARAM)"
            );
        }
        throw MakeException("NvEnc CreateEncoder failed. Code=%d %hs", e.getErrorCode(), e.what());
    }

    // API 12.1 has no read-back of the split actually used, so derive it from the engine count
    // and the NV_ENC_SPLIT_ENCODE_MODE rules in nvEncodeAPI.h.
    const char* splitGranted = "driver choice (not reported)";
    if (m_codec == ALVR_CODEC_H264) {
        splitGranted = "none (H.264)";
    } else if (initializeParams.splitEncodeMode == NV_ENC_SPLIT_TWO_FORCED_MODE) {
        splitGranted = m_nvencEngineCount >= 2 ? "two strips" : "one strip (one engine)";
    } else if (initializeParams.splitEncodeMode == NV_ENC_SPLIT_DISABLE_MODE) {
        splitGranted = "none";
    }
    Info(
        "NVENC split granted=%s (derived from engines=%d; API has no read-back)\n",
        splitGranted,
        m_nvencEngineCount
    );

    Debug("CNvEncoder is successfully initialized.\n");
}

void VideoEncoderNVENC::Shutdown() {
    std::vector<std::vector<uint8_t>> vPacket;
    if (m_NvNecoder)
        m_NvNecoder->EndEncode(vPacket);

    for (std::vector<uint8_t>& packet : vPacket) {
        if (fpOut) {
            fpOut.write(reinterpret_cast<char*>(packet.data()), packet.size());
        }
    }
    if (m_NvNecoder) {
        m_NvNecoder->DestroyEncoder();
        m_NvNecoder.reset();
    }

    Debug("CNvEncoder::Shutdown\n");

    if (fpOut) {
        fpOut.close();
    }
}

void VideoEncoderNVENC::Transmit(
    ID3D11Texture2D* pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR
) {
    auto params = GetDynamicEncoderParams();
    if (params.updated) {
        m_bitrateInMBits = params.bitrate_bps / 1'000'000;
        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        initializeParams.encodeConfig = &encodeConfig;
        FillEncodeConfig(
            initializeParams,
            params.framerate,
            m_renderWidth,
            m_renderHeight,
            m_bitrateInMBits * 1'000'000L
        );
        NV_ENC_RECONFIGURE_PARAMS reconfigureParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
        reconfigureParams.reInitEncodeParams = initializeParams;
        m_NvNecoder->Reconfigure(&reconfigureParams);
    }

    std::vector<std::vector<uint8_t>> vPacket;

    const NvEncInputFrame* encoderInputFrame = m_NvNecoder->GetNextInputFrame();

    ID3D11Texture2D* pInputTexture
        = reinterpret_cast<ID3D11Texture2D*>(encoderInputFrame->inputPtr);
    Validate10bitInput(pTexture);
    m_pD3DRender->GetContext()->CopyResource(pInputTexture, pTexture);

    NV_ENC_PIC_PARAMS picParams = {};
    if (insertIDR) {
        Debug("Inserting IDR frame.\n");
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    }
    // Synchronous encode: nvEncEncodePicture, completion wait and nvEncLockBitstream copy-out.
    // Excludes the network push in ParseFrameNals below.
    auto encodeStart = std::chrono::steady_clock::now();
    m_NvNecoder->EncodeFrame(vPacket, &picParams);
    auto encodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - encodeStart
    )
                        .count();
    ReportNvencEncoded(targetTimestampNs, encodeNs);

    for (std::vector<uint8_t>& packet : vPacket) {
        uint8_t* buf = packet.data();
        int len = (int)packet.size();

        // NVENC's AV1 encoding includes a bunch of IVF wrapping,
        // so we need to strip it down to just the OBUs
        if (m_codec == ALVR_CODEC_AV1) {
            const uint8_t ivf_magic[4] = { 0x44, 0x4B, 0x49, 0x46 };
            if (len >= 4 && !memcmp(buf, ivf_magic, 4)) {
                buf += 32;
                len -= 32;
            }
            if (len <= 12) {
                continue;
            }
            buf += 12; // skip past the IVF packet size header thing
            len -= 12;
        }

        if (len <= 0) {
            continue;
        }

        if (fpOut) {
            fpOut.write(reinterpret_cast<char*>(buf), len);
        }

        ParseFrameNals(m_codec, buf, len, targetTimestampNs, insertIDR);
    }
}

void VideoEncoderNVENC::FillEncodeConfig(
    NV_ENC_INITIALIZE_PARAMS& initializeParams,
    int refreshRate,
    int renderWidth,
    int renderHeight,
    uint64_t bitrate_bps
) {
    auto& encodeConfig = *initializeParams.encodeConfig;

    GUID encoderGUID;
    switch (m_codec) {
    case ALVR_CODEC_H264:
        encoderGUID = NV_ENC_CODEC_H264_GUID;
        break;
    case ALVR_CODEC_HEVC:
        encoderGUID = NV_ENC_CODEC_HEVC_GUID;
        break;
    case ALVR_CODEC_AV1:
        encoderGUID = NV_ENC_CODEC_AV1_GUID;
        break;
    }

    GUID qualityPreset;
    // See recommended NVENC settings for low-latency encoding.
    // https://docs.nvidia.com/video-technologies/video-codec-sdk/nvenc-video-encoder-api-prog-guide/#recommended-nvenc-settings
    switch (Settings::Instance().m_nvencQualityPreset) {
    case 7:
        qualityPreset = NV_ENC_PRESET_P7_GUID;
        break;
    case 6:
        qualityPreset = NV_ENC_PRESET_P6_GUID;
        break;
    case 5:
        qualityPreset = NV_ENC_PRESET_P5_GUID;
        break;
    case 4:
        qualityPreset = NV_ENC_PRESET_P4_GUID;
        break;
    case 3:
        qualityPreset = NV_ENC_PRESET_P3_GUID;
        break;
    case 2:
        qualityPreset = NV_ENC_PRESET_P2_GUID;
        break;
    case 1:
    default:
        qualityPreset = NV_ENC_PRESET_P1_GUID;
        break;
    }

    NV_ENC_TUNING_INFO tuningPreset
        = static_cast<NV_ENC_TUNING_INFO>(Settings::Instance().m_nvencTuningPreset);

    m_NvNecoder->CreateDefaultEncoderParams(
        &initializeParams, encoderGUID, qualityPreset, tuningPreset
    );

    initializeParams.encodeWidth = initializeParams.darWidth = renderWidth;
    initializeParams.encodeHeight = initializeParams.darHeight = renderHeight;
    initializeParams.frameRateNum = refreshRate;
    initializeParams.frameRateDen = 1;

    if (Settings::Instance().m_nvencRefreshRate != -1) {
        initializeParams.frameRateNum = Settings::Instance().m_nvencRefreshRate;
    }

    initializeParams.enableWeightedPrediction
        = Settings::Instance().m_nvencEnableWeightedPrediction;
    uint32_t splitEncodeMode = Settings::Instance().m_nvencSplitEncodeMode;
    if (splitEncodeMode == SPLIT_TWO_IF_AVAILABLE) {
        // Split encoding is not applicable to H264 (NV_ENC_SPLIT_ENCODE_MODE).
        splitEncodeMode
            = m_nvencEngineCount >= 2 && (m_codec == ALVR_CODEC_HEVC || m_codec == ALVR_CODEC_AV1)
            ? NV_ENC_SPLIT_TWO_FORCED_MODE
            : NV_ENC_SPLIT_DISABLE_MODE;
    }
    initializeParams.splitEncodeMode = splitEncodeMode;
    if (initializeParams.splitEncodeMode == NV_ENC_SPLIT_TWO_FORCED_MODE) {
        // Keep split blockers off for initialization and bitrate reconfiguration alike.
        initializeParams.enableWeightedPrediction = 0;
        initializeParams.enableSubFrameWrite = 0;
        initializeParams.enableOutputInVidmem = 0;
        if (m_codec == ALVR_CODEC_HEVC) {
            encodeConfig.encodeCodecConfig.hevcConfig.enableAlphaLayerEncoding = 0;
        }
    }

    // 16 is recommended when using reference frame invalidation. But it has caused bad visual
    // quality. Now, use 0 (use default).
    uint32_t maxNumRefFrames = 0;
    uint32_t gopLength = NVENC_INFINITE_GOPLENGTH;

    if (Settings::Instance().m_nvencMaxNumRefFrames != -1) {
        maxNumRefFrames = Settings::Instance().m_nvencMaxNumRefFrames;
    }
    if (Settings::Instance().m_nvencGopLength != -1) {
        gopLength = Settings::Instance().m_nvencGopLength;
    }

    switch (m_codec) {
    case ALVR_CODEC_H264: {
        auto& config = encodeConfig.encodeCodecConfig.h264Config;
        config.repeatSPSPPS = 1;
        config.enableIntraRefresh = Settings::Instance().m_nvencEnableIntraRefresh;

        if (Settings::Instance().m_nvencIntraRefreshPeriod != -1) {
            config.intraRefreshPeriod = Settings::Instance().m_nvencIntraRefreshPeriod;
        }
        if (Settings::Instance().m_nvencIntraRefreshCount != -1) {
            config.intraRefreshCnt = Settings::Instance().m_nvencIntraRefreshCount;
        }

        switch (Settings::Instance().m_entropyCoding) {
        case ALVR_CABAC:
            config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
            break;
        case ALVR_CAVLC:
            config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
            break;
        }

        config.maxNumRefFrames = maxNumRefFrames;
        config.idrPeriod = gopLength;

        if (Settings::Instance().m_fillerData) {
            config.enableFillerDataInsertion = Settings::Instance().m_rateControlMode == ALVR_CBR;
        }

        config.h264VUIParameters.videoSignalTypePresentFlag = 1;
        config.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        config.h264VUIParameters.videoFullRangeFlag
            = Settings::Instance().m_useFullRangeEncoding ? 1 : 0;
        config.h264VUIParameters.colourDescriptionPresentFlag = 1;
        if (Settings::Instance().m_enableHdr) {
            config.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.h264VUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        } else {
            config.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            config.h264VUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
    } break;
    case ALVR_CODEC_HEVC: {
        auto& config = encodeConfig.encodeCodecConfig.hevcConfig;
        config.repeatSPSPPS = 1;
        config.enableIntraRefresh = Settings::Instance().m_nvencEnableIntraRefresh;

        if (Settings::Instance().m_nvencIntraRefreshPeriod != -1) {
            config.intraRefreshPeriod = Settings::Instance().m_nvencIntraRefreshPeriod;
        }
        if (Settings::Instance().m_nvencIntraRefreshCount != -1) {
            config.intraRefreshCnt = Settings::Instance().m_nvencIntraRefreshCount;
        }

        config.maxNumRefFramesInDPB = maxNumRefFrames;
        config.idrPeriod = gopLength;

        if (Settings::Instance().m_use10bitEncoder) {
            encodeConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 = 2;
            if (!Settings::Instance().m_enableHdr || Settings::Instance().m_hdrPq) {
                encodeConfig.profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
            }
        }

        if (Settings::Instance().m_fillerData) {
            config.enableFillerDataInsertion = Settings::Instance().m_rateControlMode == ALVR_CBR;
        }

        config.hevcVUIParameters.videoSignalTypePresentFlag = 1;
        config.hevcVUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        config.hevcVUIParameters.videoFullRangeFlag
            = Settings::Instance().m_useFullRangeEncoding ? 1 : 0;
        config.hevcVUIParameters.colourDescriptionPresentFlag = 1;
        if (Settings::Instance().m_hdrPq) {
            config.hevcVUIParameters.videoFullRangeFlag = 0;
            config.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.hevcVUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
            config.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
            config.hevcVUIParameters.chromaSampleLocationFlag = 1;
            config.hevcVUIParameters.chromaSampleLocationTop = 1;
            config.hevcVUIParameters.chromaSampleLocationBot = 1;
        } else if (Settings::Instance().m_enableHdr) {
            config.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.hevcVUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        } else {
            config.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            config.hevcVUIParameters.transferCharacteristics
                = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
            if (Settings::Instance().m_use10bitEncoder) {
                // sdr_p010.hlsl averages 2x2: H.265 Annex E, Figure E.1 type 1
                // is centred horizontally and vertically. Progressive fields agree.
                config.hevcVUIParameters.chromaSampleLocationFlag = 1;
                config.hevcVUIParameters.chromaSampleLocationTop = 1;
                config.hevcVUIParameters.chromaSampleLocationBot = 1;
            }
        }
    } break;
    case ALVR_CODEC_AV1: {
        auto& config = encodeConfig.encodeCodecConfig.av1Config;
        config.repeatSeqHdr = 1;
        config.enableIntraRefresh = Settings::Instance().m_nvencEnableIntraRefresh;

        if (Settings::Instance().m_nvencIntraRefreshPeriod != -1) {
            config.intraRefreshPeriod = Settings::Instance().m_nvencIntraRefreshPeriod;
        }
        if (Settings::Instance().m_nvencIntraRefreshCount != -1) {
            config.intraRefreshCnt = Settings::Instance().m_nvencIntraRefreshCount;
        }

        config.maxNumRefFramesInDPB = maxNumRefFrames;
        config.idrPeriod = gopLength;

        if (Settings::Instance().m_use10bitEncoder) {
            config.pixelBitDepthMinus8 = 2;
        }

        if (Settings::Instance().m_fillerData) {
            config.enableBitstreamPadding = Settings::Instance().m_rateControlMode == ALVR_CBR;
        }

        config.chromaFormatIDC = 1; // 4:2:0, 4:4:4 currently not supported
        config.colorRange = Settings::Instance().m_useFullRangeEncoding ? 1 : 0;
        if (Settings::Instance().m_enableHdr) {
            config.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
            config.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
        } else {
            config.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            config.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            config.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
    } break;
    }

    // Disable automatic IDR insertion by NVENC. We need to manually insert IDR when packet is
    // dropped if don't use reference frame invalidation.
    encodeConfig.gopLength = gopLength;
    encodeConfig.frameIntervalP = 1;

    if (Settings::Instance().m_nvencPFrameStrategy != -1) {
        encodeConfig.frameIntervalP = Settings::Instance().m_nvencPFrameStrategy;
    }

    switch (Settings::Instance().m_rateControlMode) {
    case ALVR_CBR:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        break;
    case ALVR_VBR:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        break;
    }
    encodeConfig.rcParams.multiPass
        = static_cast<NV_ENC_MULTI_PASS>(Settings::Instance().m_nvencMultiPass);
    encodeConfig.rcParams.lowDelayKeyFrameScale = 1;

    if (Settings::Instance().m_nvencLowDelayKeyFrameScale != -1) {
        encodeConfig.rcParams.lowDelayKeyFrameScale
            = Settings::Instance().m_nvencLowDelayKeyFrameScale;
    }

    uint32_t maxFrameSize = static_cast<uint32_t>(bitrate_bps / refreshRate);
    Debug("VideoEncoderNVENC: maxFrameSize=%d bits\n", maxFrameSize);
    encodeConfig.rcParams.vbvBufferSize = maxFrameSize * 1.1;
    encodeConfig.rcParams.vbvInitialDelay = maxFrameSize * 1.1;
    encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(bitrate_bps);
    encodeConfig.rcParams.averageBitRate = static_cast<uint32_t>(bitrate_bps);
    if (Settings::Instance().m_nvencAdaptiveQuantizationMode == SpatialAQ) {
        encodeConfig.rcParams.enableAQ = 1;
    } else if (Settings::Instance().m_nvencAdaptiveQuantizationMode == TemporalAQ) {
        encodeConfig.rcParams.enableTemporalAQ = 1;
    }

    if (Settings::Instance().m_nvencRateControlMode != -1) {
        encodeConfig.rcParams.rateControlMode
            = (NV_ENC_PARAMS_RC_MODE)Settings::Instance().m_nvencRateControlMode;
    }
    if (Settings::Instance().m_nvencRcBufferSize != -1) {
        encodeConfig.rcParams.vbvBufferSize = Settings::Instance().m_nvencRcBufferSize;
    }
    if (Settings::Instance().m_nvencRcInitialDelay != -1) {
        encodeConfig.rcParams.vbvInitialDelay = Settings::Instance().m_nvencRcInitialDelay;
    }
    if (Settings::Instance().m_nvencRcMaxBitrate != -1) {
        encodeConfig.rcParams.maxBitRate = Settings::Instance().m_nvencRcMaxBitrate;
    }
    if (Settings::Instance().m_nvencRcAverageBitrate != -1) {
        encodeConfig.rcParams.averageBitRate = Settings::Instance().m_nvencRcAverageBitrate;
    }
}
