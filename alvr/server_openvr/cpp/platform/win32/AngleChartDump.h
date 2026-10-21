#pragma once
#include <DirectXPackedVector.h>
#include <wincodec.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <algorithm>
#include <cmath>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// Copy once, poll without waiting for the GPU, then encode/write on a worker.
class AngleChartDump {
    struct Image {
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC desc {};
        std::vector<uint8_t> bytes;
    };
    Image images[2];
    std::future<void> writer;
    std::string metadata;
public:
    bool armed = false;
    bool Started() const { return images[0].staging && !images[1].staging; }
    void Capture(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11Texture2D* source, int index, const std::string& meta = {}) {
        auto& im = images[index];
        source->GetDesc(&im.desc);
        auto desc = im.desc;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        OK_OR_THROW(device->CreateTexture2D(&desc, nullptr, &im.staging), "Chart dump staging");
        context->CopyResource(im.staging.Get(), source);
        if (index == 0) metadata = meta;
    }
    void Poll(ID3D11DeviceContext* context) {
        if (!images[1].staging) return;
        for (auto& im : images) {
            if (!im.bytes.empty()) continue;
            D3D11_MAPPED_SUBRESOURCE map {};
            const HRESULT hr = context->Map(im.staging.Get(), 0, D3D11_MAP_READ,
                                            D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (FAILED(hr)) { Error("angle_chart dump Map failed: %lx", hr); images[1].staging.Reset(); return; }
            const UINT row = im.desc.Width * (im.desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
            im.bytes.resize(size_t(row) * im.desc.Height);
            for (UINT y = 0; y < im.desc.Height; ++y)
                memcpy(im.bytes.data() + size_t(y) * row, (uint8_t*)map.pData + size_t(y) * map.RowPitch, row);
            context->Unmap(im.staging.Get(), 0);
        }
        auto chart = std::move(images[0]), input = std::move(images[1]);
        images[0] = {}; images[1] = {};
        writer = std::async(std::launch::async, [chart = std::move(chart), input = std::move(input), meta = metadata]() mutable {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
                auto dir = std::filesystem::path(L"C:\\Users\\noelm\\claude-tmp\\angle-chart") / std::to_string(stamp);
                std::filesystem::create_directories(dir);
                std::ofstream(dir / "meta.json") << meta;
                Save(chart, dir, true);
                Save(input, dir, false);
                Info("angle_chart dump written: %s", dir.string().c_str());
            } catch (const std::exception& e) { Error("angle_chart dump failed: %s", e.what()); }
            if (SUCCEEDED(co)) CoUninitialize();
        });
    }
    bool Busy() const {
        return images[0].staging || (writer.valid()
            && writer.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
    }
private:
    static void Save(Image& im, const std::filesystem::path& dir, bool eyes) {
        if (im.desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            std::vector<uint8_t> rgba(size_t(im.desc.Width) * im.desc.Height * 4);
            auto half = reinterpret_cast<const DirectX::PackedVector::HALF*>(im.bytes.data());
            for (size_t i = 0; i < rgba.size(); ++i) {
                float v = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
                rgba[i] = uint8_t(std::round(std::clamp(v, 0.f, 1.f) * 255.f));
            }
            im.bytes = std::move(rgba);
        }
        ComPtr<IWICImagingFactory> factory;
        OK_OR_THROW(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory)), "Chart dump WIC");
        const UINT width = im.desc.Width / (eyes ? 2 : 1), stride = im.desc.Width * 4;
        for (int eye = 0; eye < (eyes ? 2 : 1); ++eye) {
            auto path = dir / (eyes ? (eye ? "chart-R.png" : "chart-L.png") : "encoder-input.png");
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            OK_OR_THROW(factory->CreateStream(&stream), "Chart dump stream");
            OK_OR_THROW(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Chart dump file");
            OK_OR_THROW(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Chart dump PNG");
            OK_OR_THROW(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Chart dump encoder");
            OK_OR_THROW(encoder->CreateNewFrame(&frame, nullptr), "Chart dump frame");
            OK_OR_THROW(frame->Initialize(nullptr), "Chart dump initialize");
            OK_OR_THROW(frame->SetSize(width, im.desc.Height), "Chart dump size");
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
            OK_OR_THROW(frame->SetPixelFormat(&format), "Chart dump RGBA");
            OK_OR_THROW(frame->WritePixels(im.desc.Height, stride,
                        UINT(im.bytes.size() - eye * width * 4), im.bytes.data() + eye * width * 4), "Chart dump pixels");
            OK_OR_THROW(frame->Commit(), "Chart dump commit");
            OK_OR_THROW(encoder->Commit(), "Chart dump finish");
        }
    }
};
