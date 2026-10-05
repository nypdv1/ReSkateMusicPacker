// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "packer.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace music {
namespace {
using Microsoft::WRL::ComPtr;
void require(bool ok) { if (!ok) throw std::runtime_error("Could not generate the playlist cover"); }
struct Com {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Com() { if (SUCCEEDED(status)) CoUninitialize(); }
};
struct Canvas {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    ~Canvas() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};
struct Font {
    HDC dc;
    HFONT font;
    HGDIOBJ previous;
    Font(HDC target, int size, int weight) : dc(target),
        font(CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI")),
        previous(font ? SelectObject(dc, font) : nullptr) { require(font && previous); }
    ~Font() { SelectObject(dc, previous); DeleteObject(font); }
};
std::wstring wrapped(HDC dc, std::wstring text, int width, int& lines) {
    std::wstring result;
    lines = 0;
    while (!text.empty()) {
        std::size_t count = 0, space = 0;
        for (std::size_t i = 1; i <= text.size(); ++i) {
            if (i < text.size() && text[i - 1] >= 0xD800 && text[i - 1] <= 0xDBFF) continue;
            SIZE extent{};
            require(GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(i), &extent) != FALSE);
            if (extent.cx > width) break;
            count = i;
            if (text[i - 1] == L' ') space = i;
        }
        if (count < text.size() && space) count = space;
        if (!count) count = (text.front() >= 0xD800 && text.front() <= 0xDBFF && text.size() > 1) ? 2 : 1;
        auto line = text.substr(0, count);
        while (!line.empty() && line.back() == L' ') line.pop_back();
        if (!result.empty()) result += L'\n';
        result += line;
        ++lines;
        text.erase(0, count);
        while (!text.empty() && text.front() == L' ') text.erase(0, 1);
    }
    return result;
}

std::vector<std::byte> encode_image_png(const std::vector<unsigned char>& pixels) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
    require(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)));
    require(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)));
    require(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    require(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)));
    require(SUCCEEDED(frame->SetSize(512, 512)));
    auto format = GUID_WICPixelFormat32bppBGRA;
    require(SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA);
    require(SUCCEEDED(frame->WritePixels(512, 512 * 4, static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()))));
    require(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()));
    STATSTG stat{};
    require(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)));
    HGLOBAL global{};
    require(SUCCEEDED(GetHGlobalFromStream(stream.Get(), &global)));
    const auto* bytes = static_cast<const std::byte*>(GlobalLock(global));
    require(bytes != nullptr);
    std::vector<std::byte> result(bytes, bytes + static_cast<std::size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(global);
    return result;
}
}
std::vector<std::byte> playlist_artwork_png(const std::string& name) {
    if (!usable_name(name)) throw std::runtime_error("A generated cover needs a valid playlist name");
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    require(count > 0);
    std::wstring title(static_cast<std::size_t>(count), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), title.data(), count) == count);
    Com com;
    require(SUCCEEDED(com.status) || com.status == RPC_E_CHANGED_MODE);
    Canvas canvas;
    require(canvas.dc != nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 512;
    info.bmiHeader.biHeight = -512;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* data{};
    canvas.bitmap = CreateDIBSection(canvas.dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    require(canvas.bitmap && data);
    canvas.previous = SelectObject(canvas.dc, canvas.bitmap);
    require(canvas.previous != nullptr);
    auto* pixels = static_cast<unsigned char*>(data);
    for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
        const auto at = static_cast<std::size_t>(y * 512 + x) * 4;
        const bool accent = (x - 450) * (x - 450) + (y - 40) * (y - 40) < 170 * 170;
        pixels[at] = static_cast<unsigned char>(80 + y * 50 / 512 + (accent ? 20 : 0));
        pixels[at + 1] = static_cast<unsigned char>(35 + x * 50 / 512 + (accent ? 15 : 0));
        pixels[at + 2] = static_cast<unsigned char>(45 + y * 15 / 512 + (accent ? 25 : 0));
        pixels[at + 3] = 255;
    }
    SetBkMode(canvas.dc, TRANSPARENT);
    SetTextColor(canvas.dc, RGB(255, 255, 255));
    {
        Font font(canvas.dc, 22, FW_SEMIBOLD);
        RECT badge{48, 48, 464, 84};
        DrawTextW(canvas.dc, L"PLAYLIST", -1, &badge, DT_LEFT | DT_NOPREFIX);
    }
    for (int size = 64; size >= 16; size -= 2) {
        Font font(canvas.dc, size, FW_BOLD);
        int lines{};
        const auto text = wrapped(canvas.dc, title, 416, lines);
        TEXTMETRICW metrics{};
        require(GetTextMetricsW(canvas.dc, &metrics) != FALSE);
        const auto height = lines * metrics.tmHeight;
        if (height > 280 && size > 16) continue;
        RECT rect{48, 150 + (280 - height) / 2, 464, 430};
        require(DrawTextW(canvas.dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CENTER | DT_NOPREFIX) != 0);
        break;
    }
    require(GdiFlush() != FALSE);
    for (std::size_t i = 3; i < 512 * 512 * 4; i += 4) pixels[i] = 255;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
    require(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)));
    require(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)));
    require(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    require(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)));
    require(SUCCEEDED(frame->SetSize(512, 512)));
    auto format = GUID_WICPixelFormat32bppBGRA;
    require(SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA);
    require(SUCCEEDED(frame->WritePixels(512, 512 * 4, 512 * 512 * 4, pixels)));
    require(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()));
    STATSTG stat{};
    require(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart < 4 * 1024 * 1024);
    HGLOBAL global{};
    require(SUCCEEDED(GetHGlobalFromStream(stream.Get(), &global)));
    const auto* bytes = static_cast<const std::byte*>(GlobalLock(global));
    require(bytes != nullptr);
    std::vector<std::byte> result(bytes, bytes + static_cast<std::size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(global);
    return result;
}

std::vector<std::byte> image_artwork_png(const std::filesystem::path& image) {
    Com com;
    if (FAILED(com.status) && com.status != RPC_E_CHANGED_MODE) throw std::runtime_error("Could not initialize image decoding");
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> source;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(image.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &source)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("Could not load image: " + image.string());

    UINT width{}, height{};
    require(SUCCEEDED(converter->GetSize(&width, &height)) && width && height);
    const auto scale = 512.0 / std::max(width, height);
    const UINT scaledWidth = std::max<UINT>(1, static_cast<UINT>(width * scale));
    const UINT scaledHeight = std::max<UINT>(1, static_cast<UINT>(height * scale));
    ComPtr<IWICBitmapScaler> scaler;
    require(SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(converter.Get(), scaledWidth, scaledHeight, WICBitmapInterpolationModeFant)));

    std::vector<unsigned char> pixels(512 * 512 * 4, 0);
    for (std::size_t p = 0; p < pixels.size(); p += 4) {
        pixels[p] = 0x35; pixels[p + 1] = 0x20; pixels[p + 2] = 0x24; pixels[p + 3] = 255;
    }
    const UINT stride = scaledWidth * 4;
    std::vector<unsigned char> scaled(static_cast<std::size_t>(stride) * scaledHeight);
    require(SUCCEEDED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(scaled.size()), scaled.data())));
    const UINT left = (512 - scaledWidth) / 2, top = (512 - scaledHeight) / 2;
    for (UINT y = 0; y < scaledHeight; ++y)
        std::memcpy(pixels.data() + (static_cast<std::size_t>(top + y) * 512 + left) * 4,
                    scaled.data() + static_cast<std::size_t>(y) * stride, stride);
    return encode_image_png(pixels);
}
}
