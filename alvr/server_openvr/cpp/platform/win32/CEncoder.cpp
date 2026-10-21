#include "CEncoder.h"
#include "alvr_server/Hdr10.h"

CEncoder::CEncoder()
    : m_bExiting(false)
    , m_targetTimestampNs(0) {
    m_encodeFinished.Set();
}

CEncoder::~CEncoder() {
    if (m_videoEncoder) {
        m_videoEncoder->Shutdown();
        m_videoEncoder.reset();
    }
}

void CEncoder::Initialize(std::shared_ptr<CD3DRender> d3dRender) {
    using hdr10::Backend;
    auto& settings = Settings::Instance();
    const bool requestedPq = settings.m_hdrPq, requestedHdr = settings.m_enableHdr;
    const bool hevc = settings.m_codec == ALVR_CODEC_HEVC;
    const bool preferPq = requestedPq && hevc
        && settings.m_use10bitEncoder && !settings.m_force_sw_encoding;
    uint32_t encoderWidth, encoderHeight;
    FrameRender::GetEncodingResolution(&encoderWidth, &encoderHeight);
    std::string errors;
    const auto attempt = [&](Backend backend) {
        const auto plan = hdr10::MakeInputPlan(backend, preferPq, hevc,
            settings.m_use10bitEncoder, requestedHdr, settings.m_highPrecisionSdr);
        settings.m_hdrPq = plan.pq;
        settings.m_enableHdr = plan.hdr;
        const char* name = backend == Backend::Nvenc ? "NVENC" : backend == Backend::Amf ? "AMF" : "software";
        try {
            switch (backend) {
            case Backend::Nvenc:
                m_videoEncoder = std::make_shared<VideoEncoderNVENC>(d3dRender, encoderWidth, encoderHeight);
                break;
            case Backend::Amf:
                m_videoEncoder = std::make_shared<VideoEncoderAMF>(d3dRender, encoderWidth, encoderHeight);
                break;
#ifdef ALVR_GPL
            case Backend::Software:
                m_videoEncoder = std::make_shared<VideoEncoderSW>(d3dRender, encoderWidth, encoderHeight);
                break;
#endif
            default: return false;
            }
            m_videoEncoder->Initialize();
        } catch (const std::exception& e) {
            errors += std::string(name) + ": " + e.what() + "; ";
            m_videoEncoder.reset();
            return false;
        }
        // Backend and its signalling are final before any conversion textures exist.
        m_FrameRender = std::make_shared<FrameRender>(d3dRender);
        if (!m_FrameRender->Startup(plan))
            throw MakeException("Failed to create %s frame conversion pipeline", name);
        if (requestedPq && !plan.pq)
            Info("HDR PQ unavailable: selected %s (requires NVENC HEVC 10-bit); using Srgb. %s", name, errors.c_str());
        if (plan.pq && settings.m_clampHdrExtendedRange)
            Info("HDR PQ: ignoring clamp_hdr_extended_range to preserve values above 1.0.");
        return true;
    };
#ifdef ALVR_GPL
    if (settings.m_force_sw_encoding && attempt(Backend::Software)) return;
#endif
    if (preferPq) {
        if (attempt(Backend::Nvenc) || attempt(Backend::Amf)) return;
    } else {
        if (attempt(Backend::Amf) || attempt(Backend::Nvenc)) return;
    }
#ifdef ALVR_GPL
    if (attempt(Backend::Software)) return;
#endif
    throw MakeException("All video encoders are unavailable: %s", errors.c_str());
}

void CEncoder::SetViewsConfig(
    vr::HmdRect2_t projLeft,
    vr::HmdMatrix34_t eyeToHeadLeft,
    vr::HmdRect2_t projRight,
    vr::HmdMatrix34_t eyeToHeadRight
) {
    m_FrameRender->SetViewsConfig(projLeft, eyeToHeadLeft, projRight, eyeToHeadRight);
}

bool CEncoder::CopyToStaging(
    ID3D11Texture2D* pTexture[][2],
    vr::VRTextureBounds_t bounds[][2],
    vr::HmdMatrix34_t poses[],
    int layerCount,
    bool recentering,
    uint64_t presentationTime,
    uint64_t targetTimestampNs,
    const std::string& message,
    const std::string& debugText
) {
    m_presentationTime = presentationTime;
    m_targetTimestampNs = targetTimestampNs;

    m_FrameRender->RenderFrame(
        pTexture, bounds, poses, layerCount, recentering, message, debugText
    );
    return true;
}

void CEncoder::Run() {
    Debug("CEncoder: Start thread. Id=%d\n", GetCurrentThreadId());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_MOST_URGENT);

    while (!m_bExiting) {
        m_newFrameReady.Wait();
        if (m_bExiting)
            break;

        if (m_FrameRender->GetTexture()) {
            m_videoEncoder->Transmit(
                m_FrameRender->GetTexture().Get(),
                m_presentationTime,
                m_targetTimestampNs,
                m_scheduler.CheckIDRInsertion()
            );
        }

        m_encodeFinished.Set();
    }
}

void CEncoder::Stop() {
    m_bExiting = true;
    m_newFrameReady.Set();
    Join();
    m_FrameRender.reset();
}

void CEncoder::NewFrameReady() {
    m_encodeFinished.Reset();
    m_newFrameReady.Set();
}

void CEncoder::WaitForEncode() { m_encodeFinished.Wait(); }

void CEncoder::OnStreamStart() { m_scheduler.OnStreamStart(); }

void CEncoder::OnPacketLoss() { m_scheduler.OnPacketLoss(); }

void CEncoder::InsertIDR() { m_scheduler.InsertIDR(); }

void CEncoder::CaptureFrame() { }
