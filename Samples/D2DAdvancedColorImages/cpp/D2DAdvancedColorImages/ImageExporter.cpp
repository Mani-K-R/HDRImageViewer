#include "pch.h"
#include "DirectXHelper.h"
#include "ImageExporter.h"
#include "MagicConstants.h"
#include "SimpleTonemapEffect.h"

using namespace Microsoft::WRL;

using namespace D2DAdvancedColorImages;
using DX::CHK;

ImageExporter::ImageExporter()
{
    throw ref new Platform::NotImplementedException;
}

ImageExporter::~ImageExporter()
{
}

/// <summary>
/// Converts an HDR image to SDR using a pipeline equivalent to
/// RenderEffectKind::HdrTonemap. Not yet suitable for general purpose use.
/// </summary>
/// <param name="wicFormat">WIC container format GUID (GUID_ContainerFormat...)</param>
void ImageExporter::ExportToSdr(ImageLoader* loader, DX::DeviceResources* res, IStream* stream, GUID wicFormat)
{
    auto ctx = res->GetD2DDeviceContext();

    // Effect graph: ImageSource > ColorManagement  > HDRTonemap > WhiteScale
    // This graph is derived from, but not identical to RenderEffectKind::HdrTonemap.
    // TODO: Is there any way to keep this better in sync with the main render pipeline?

    ComPtr<ID2D1TransformedImageSource> source = loader->GetLoadedImage(1.0f);

    ComPtr<ID2D1Effect> colorManage;
    CHK(ctx->CreateEffect(CLSID_D2D1ColorManagement, &colorManage));
    colorManage->SetInput(0, source.Get());
    CHK(colorManage->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST));

    ComPtr<ID2D1ColorContext> sourceCtx = loader->GetImageColorContext();
    CHK(colorManage->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, sourceCtx.Get()));

    ComPtr<ID2D1ColorContext1> destCtx;
    // scRGB
    CHK(ctx->CreateColorContextFromDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &destCtx));
    CHK(colorManage->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, destCtx.Get()));

    GUID tmGuid = {};
    if (DX::CheckPlatformSupport(DX::Win1809)) tmGuid = CLSID_D2D1HdrToneMap;
    else tmGuid = CLSID_CustomSimpleTonemapEffect;

    ComPtr<ID2D1Effect> tonemap;
    CHK(ctx->CreateEffect(tmGuid, &tonemap));
    tonemap->SetInputEffect(0, colorManage.Get());
    CHK(tonemap->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, sc_DefaultSdrDispMaxNits));
    CHK(tonemap->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE, D2D1_HDRTONEMAP_DISPLAY_MODE_SDR));

    ComPtr<ID2D1Effect> whiteScale;
    CHK(ctx->CreateEffect(CLSID_D2D1ColorMatrix, &whiteScale));
    whiteScale->SetInputEffect(0, tonemap.Get());

    float scale = D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL / sc_DefaultSdrDispMaxNits;
    D2D1_MATRIX_5X4_F matrix = D2D1::Matrix5x4F(
        scale, 0, 0, 0,  // [R] Multiply each color channel
        0, scale, 0, 0,  // [G] by the scale factor in 
        0, 0, scale, 0,  // [B] linear gamma space.
        0, 0, 0, 1,      // [A] Preserve alpha values.
        0, 0, 0, 0);     //     No offset.

    CHK(whiteScale->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix));

    ComPtr<ID2D1Image> d2dImage;
    whiteScale->GetOutput(&d2dImage);

    ImageExporter::ExportToWic(d2dImage.Get(), loader->GetImageInfo().size, res, stream, wicFormat);
}

/// <summary>
/// Encodes to WIC using default encode options.
/// </summary>
/// <remarks>
/// Relies on IWICImageBitmapFrameEncode's pixel format conversion (which performs gamma correction).
/// </remarks>
void ImageExporter::ExportToWic(ID2D1Image* img, Windows::Foundation::Size size, DX::DeviceResources* res, IStream* stream, GUID wicFormat)
{
    auto dev = res->GetD2DDevice();
    auto wic = res->GetWicImagingFactory();

    ComPtr<IWICBitmapEncoder> encoder;
    CHK(wic->CreateEncoder(wicFormat, nullptr, &encoder));
    CHK(encoder->Initialize(stream, WICBitmapEncoderNoCache));

    ComPtr<IWICBitmapFrameEncode> frame;
    CHK(encoder->CreateNewFrame(&frame, nullptr));
    CHK(frame->Initialize(nullptr));

    // IWICImageEncoder's internal pixel format conversion from float to uint does not perform gamma correction.
    // For simplicity, rely on the IWICBitmapFrameEncode's format converter which does perform gamma correction.
    WICImageParameters params = {
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,                             // DpiX
        96.0f,                             // DpiY
        0,                                 // OffsetX
        0,                                 // OffsetY
        static_cast<uint32_t>(size.Width), // SizeX
        static_cast<uint32_t>(size.Height) // SizeY
    };

    ComPtr<IWICImageEncoder> imageEncoder;
    CHK(wic->CreateImageEncoder(dev, &imageEncoder));
    CHK(imageEncoder->WriteFrame(img, frame.Get(), &params));
    CHK(frame->Commit());
    CHK(encoder->Commit());
    CHK(stream->Commit(STGC_DEFAULT));
}
