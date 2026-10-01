// anyar CLI — app icon → .ico (Windows, via WIC)
//
// Decodes any WIC-readable image (PNG, JPEG, BMP, ICO, …), scales it to the
// standard icon sizes with high-quality cubic filtering and writes an .ico
// whose entries are PNG-compressed (supported since Windows Vista).

#include "cli.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstdint>
#include <fstream>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace anyar_cli {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

/// Scale @p source to size×size and encode it as PNG bytes.
bool encode_png(IWICImagingFactory* f, IWICBitmapSource* source, UINT size,
                std::vector<uint8_t>& out) {
    Com<IWICBitmapScaler> scaler;
    if (FAILED(f->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(source, size, size,
                                  WICBitmapInterpolationModeHighQualityCubic))) {
        return false;
    }
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return false;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(frame.put(), nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(size, size)) ||
        FAILED(frame->SetPixelFormat(&fmt)) ||
        FAILED(frame->WriteSource(scaler.p, nullptr)) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit())) {
        return false;
    }
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.p, &mem))) return false;
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    const size_t n = static_cast<size_t>(st.cbSize.QuadPart);
    const void* data = GlobalLock(mem);
    if (!data) return false;
    out.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    GlobalUnlock(mem);
    return true;
}

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

} // namespace

bool make_ico(const fs::path& src, const fs::path& dst) {
    std::string ext = src.extension().string();
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (ext == ".ico") {
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        Com<IWICImagingFactory> f;
        Com<IWICBitmapDecoder> dec;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(f.put()))) &&
            SUCCEEDED(f->CreateDecoderFromFilename(src.wstring().c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, dec.put())) &&
            SUCCEEDED(dec->GetFrame(0, frame.put())) &&
            SUCCEEDED(frame->GetSize(&w, &h)) &&
            SUCCEEDED(f->CreateFormatConverter(conv.put())) &&
            SUCCEEDED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            if (w != h) {
                print_info("Icon " + src.filename().string() + " is " + std::to_string(w) + "x" +
                           std::to_string(h) + " — not square, it will be stretched");
            }
            const UINT sizes[] = {16, 24, 32, 48, 64, 128, 256};
            std::vector<std::vector<uint8_t>> images;
            ok = true;
            for (UINT s : sizes) {
                images.emplace_back();
                if (!encode_png(f.p, conv.p, s, images.back())) { ok = false; break; }
            }
            if (ok) {
                // ICONDIR + ICONDIRENTRY[n] + PNG payloads
                std::vector<uint8_t> ico;
                put16(ico, 0);
                put16(ico, 1);  // type: icon
                put16(ico, static_cast<uint16_t>(images.size()));
                uint32_t offset = 6 + 16 * static_cast<uint32_t>(images.size());
                for (size_t i = 0; i < images.size(); ++i) {
                    const UINT s = sizes[i];
                    ico.push_back(static_cast<uint8_t>(s >= 256 ? 0 : s));  // 0 = 256
                    ico.push_back(static_cast<uint8_t>(s >= 256 ? 0 : s));
                    ico.push_back(0);  // palette colours
                    ico.push_back(0);  // reserved
                    put16(ico, 1);     // planes
                    put16(ico, 32);    // bits per pixel
                    put32(ico, static_cast<uint32_t>(images[i].size()));
                    put32(ico, offset);
                    offset += static_cast<uint32_t>(images[i].size());
                }
                for (const auto& img : images) ico.insert(ico.end(), img.begin(), img.end());
                std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(ico.data()),
                          static_cast<std::streamsize>(ico.size()));
                ok = static_cast<bool>(out);
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

} // namespace anyar_cli
