#include "pch.h"
#include "PlaceholderImageHelper.h"
#if __has_include("PlaceholderImageHelper.g.cpp")
#include "PlaceholderImageHelper.g.cpp"
#endif

#include "SVG/nanosvg.h"
#include "StringUtils.h"
#include "Helpers\COMHelper.h"

#include <webp\decode.h>
#include <webp\demux.h>

#include <shcore.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <BufferSurface.h>

using namespace D2D1;
using namespace winrt::Windows::ApplicationModel;

namespace winrt::Telegram::Native::implementation
{
    winrt::slim_mutex PlaceholderImageHelper::s_criticalSection;
    winrt::com_ptr<PlaceholderImageHelper> PlaceholderImageHelper::s_current{ nullptr };

    class CustomFontFileEnumerator
        : public winrt::implements<CustomFontFileEnumerator, IDWriteFontFileEnumerator>
    {
        winrt::com_ptr<IDWriteFactory> m_factory;
        std::vector<const wchar_t*> m_filenames;
        int32_t m_index;
        winrt::com_ptr<IDWriteFontFile> m_theFile;

    public:
        CustomFontFileEnumerator(IDWriteFactory* factory, void const* collectionKey, uint32_t collectionKeySize)
            : m_factory()
            , m_index(0)
        {
            auto keys = static_cast<const wchar_t* const*>(collectionKey);

            m_filenames = std::vector<const wchar_t*>(keys, keys + 2);
            m_factory.copy_from(factory);
        }

        IFACEMETHODIMP MoveNext(BOOL* hasCurrentFile) override
        {
            if (m_index == m_filenames.size())
            {
                *hasCurrentFile = FALSE;
            }
            else if (SUCCEEDED(m_factory->CreateFontFileReference(m_filenames[m_index++], nullptr, m_theFile.put())))
            {
                *hasCurrentFile = TRUE;
            }
            else
            {
                *hasCurrentFile = FALSE;
            }

            return S_OK;
        }

        IFACEMETHODIMP GetCurrentFontFile(IDWriteFontFile** fontFile) override
        {
            m_theFile.copy_to(fontFile);
            return S_OK;
        }
    };



    class CustomFontLoader
        : public winrt::implements<CustomFontLoader, IDWriteFontCollectionLoader>
    {
    public:
        IFACEMETHODIMP CreateEnumeratorFromKey(
            IDWriteFactory* factory,
            void const* collectionKey,
            uint32_t collectionKeySize,
            IDWriteFontFileEnumerator** fontFileEnumerator) override
        {
            return ExceptionBoundary(
                [=]
                {
                    auto enumerator = winrt::make_self<CustomFontFileEnumerator>(factory, collectionKey, collectionKeySize);
                    enumerator.as<IDWriteFontFileEnumerator>().copy_to(fontFileEnumerator);
                });
        }
    };

    IBuffer PlaceholderImageHelper::DrawWebP(hstring fileName, int32_t maxWidth, Windows::Foundation::Size& size) noexcept
    {
        size = Windows::Foundation::Size{ 0,0 };

        FILE* file = _wfopen(fileName.data(), L"rb");
        if (file == NULL)
        {
            return nullptr;
        }

        fseek(file, 0, SEEK_END);
        size_t length = ftell(file);
        fseek(file, 0, SEEK_SET);
        char* buffer = (char*)malloc(length);
        fread(buffer, 1, length, file);
        fclose(file);

        WebPData webPData;
        webPData.bytes = (uint8_t*)buffer;
        webPData.size = length;

        auto spDemuxer = std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)>
        {
            WebPDemux(&webPData),
            WebPDemuxDelete
        };
        if (!spDemuxer)
        {
            //throw ref new InvalidArgumentException(ref new String(L"Failed to create demuxer"));
            free(buffer);
            return nullptr;
        }

        IBuffer surface;
        WebPIterator iter;
        if (WebPDemuxGetFrame(spDemuxer.get(), 1, &iter))
        {
            WebPDecoderConfig config;
            int ret = WebPInitDecoderConfig(&config);
            if (!ret)
            {
                //throw ref new FailureException(ref new String(L"WebPInitDecoderConfig failed"));
                free(buffer);
                return nullptr;
            }

            ret = (WebPGetFeatures(iter.fragment.bytes, iter.fragment.size, &config.input) == VP8_STATUS_OK);
            if (!ret)
            {
                //throw ref new FailureException(ref new String(L"WebPGetFeatures failed"));
                free(buffer);
                return nullptr;
            }

            int width = iter.width;
            int height = iter.height;

            if (iter.width > maxWidth || iter.height > maxWidth)
            {
                auto ratioX = (double)maxWidth / iter.width;
                auto ratioY = (double)maxWidth / iter.height;
                auto ratio = std::min(ratioX, ratioY);

                width = (int)(iter.width * ratio);
                height = (int)(iter.height * ratio);
            }

            size.Width = width;
            size.Height = height;

            surface = Telegram::Native::BufferSurface::Create(width * 4 * height);
            auto pixels = surface.data();
            //uint8_t* pixels = new uint8_t[(width * 4) * height];

            if (width != iter.width || height != iter.height)
            {
                config.options.scaled_width = width;
                config.options.scaled_height = height;
                config.options.use_scaling = 1;
                config.options.no_fancy_upsampling = 1;
            }

            config.output.colorspace = MODE_bgrA;
            config.output.is_external_memory = 1;
            config.output.u.RGBA.rgba = pixels;
            config.output.u.RGBA.stride = width * 4;
            config.output.u.RGBA.size = (width * 4) * height;

            ret = WebPDecode(iter.fragment.bytes, iter.fragment.size, &config);

            if (ret != VP8_STATUS_OK)
            {
                //throw ref new FailureException(ref new String(L"Failed to decode frame"));
                //delete[] pixels;

                free(buffer);
                return nullptr;
            }

            //delete[] pixels;

        }

        free(buffer);
        return surface;
    }

    winrt::Windows::Foundation::IAsyncAction PlaceholderImageHelper::DrawSvgAsync(hstring path, Color foreground, IRandomAccessStream randomAccessStream, double dpi)
    {
        winrt::apartment_context ui_thread;
        co_await winrt::resume_background();

        Windows::Foundation::Size size;
        winrt::check_hresult(InternalDrawSvg(path, foreground, randomAccessStream, dpi, size));
        randomAccessStream.Seek(0);

        co_await ui_thread;
    }

    void PlaceholderImageHelper::DrawSvg(hstring path, Color foreground, IRandomAccessStream randomAccessStream, double dpi, Windows::Foundation::Size& size)
    {
        winrt::check_hresult(InternalDrawSvg(path, foreground, randomAccessStream, dpi, size));
    }

    void PlaceholderImageHelper::DrawIdenticon(IVector<uint8_t> hash, int side, IRandomAccessStream randomAccessStream)
    {
        winrt::check_hresult(InternalDrawIdenticon(hash, side, randomAccessStream));
    }

    void PlaceholderImageHelper::DrawThumbnailPlaceholder(hstring fileName, float blurAmount, IRandomAccessStream randomAccessStream)
    {
        winrt::check_hresult(InternalDrawThumbnailPlaceholder(fileName, blurAmount, randomAccessStream));
    }

    void PlaceholderImageHelper::DrawThumbnailPlaceholder(IVector<uint8_t> bytes, float blurAmount, IRandomAccessStream randomAccessStream)
    {
        winrt::check_hresult(InternalDrawThumbnailPlaceholder(bytes, blurAmount, randomAccessStream));
    }

    HRESULT PlaceholderImageHelper::InternalDrawSvg(hstring path, Color foreground, IRandomAccessStream randomAccessStream, double dpi, Windows::Foundation::Size& size)
    {
        slim_lock_guard const guard(m_criticalSection);

        HRESULT result;

        auto data = winrt::to_string(path);

        struct NSVGimage* image;
        image = nsvgParse((char*)data.c_str(), "px", 96);

        auto unique = std::shared_ptr<NSVGimage>(image, [](NSVGimage* p)
            {
                nsvgDelete(p);
            });

        auto imageWidth = image->width * dpi;
        auto imageHeight = image->height * dpi;
        size = Windows::Foundation::Size(imageWidth, imageHeight);

        winrt::com_ptr<ID2D1Bitmap1> targetBitmap;
        D2D1_BITMAP_PROPERTIES1 properties = { { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96, D2D1_BITMAP_OPTIONS_TARGET, 0 };
        ReturnIfFailed(result, m_d2dContext->CreateBitmap(D2D1_SIZE_U{ (uint32_t)imageWidth, (uint32_t)imageHeight }, nullptr, 0, &properties, targetBitmap.put()));

        m_d2dContext->SetTarget(targetBitmap.get());
        m_d2dContext->BeginDraw();
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Scale(1 * dpi, 1 * dpi));

        winrt::com_ptr<ID2D1SolidColorBrush> blackBrush;
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(foreground.R / 255.0f, foreground.G / 255.0f, foreground.B / 255.0f, foreground.A / 255.0f), blackBrush.put()));

        for (auto shape = image->shapes; shape != NULL; shape = shape->next)
        {
            if (!(shape->flags & NSVG_FLAGS_VISIBLE) || (shape->fill.type == NSVG_PAINT_NONE && shape->stroke.type == NSVG_PAINT_NONE))
            {
                continue;
            }

            blackBrush->SetOpacity(shape->opacity);

            winrt::com_ptr<ID2D1PathGeometry1> geometry;
            ReturnIfFailed(result, m_d2dFactory->CreatePathGeometry(geometry.put()));

            winrt::com_ptr<ID2D1GeometrySink> sink;
            ReturnIfFailed(result, geometry->Open(sink.put()));

            for (NSVGpath* path = shape->paths; path != NULL; path = path->next)
            {
                sink->BeginFigure({ path->pts[0], path->pts[1] }, D2D1_FIGURE_BEGIN_FILLED);

                for (int i = 0; i < path->npts - 1; i += 3)
                {
                    float* p = &path->pts[i * 2];
                    sink->AddBezier({ { p[2], p[3] }, { p[4], p[5] }, { p[6], p[7] } });
                }

                sink->EndFigure(path->closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            }

            ReturnIfFailed(result, sink->Close());

            if (shape->fill.type != NSVG_PAINT_NONE)
            {
                switch (shape->fillRule)
                {
                case NSVG_FILLRULE_EVENODD:
                    sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                    break;
                default:
                    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
                    break;
                }

                m_d2dContext->FillGeometry(geometry.get(), blackBrush.get());
            }

            if (shape->stroke.type != NSVG_PAINT_NONE)
            {
                D2D1_STROKE_STYLE_PROPERTIES1 strokeProperties{};
                strokeProperties.miterLimit = shape->miterLimit;

                switch (shape->strokeLineCap)
                {
                case NSVG_CAP_BUTT:
                    strokeProperties.startCap = strokeProperties.endCap = D2D1_CAP_STYLE_FLAT;
                    break;
                case NSVG_CAP_ROUND:
                    strokeProperties.startCap = strokeProperties.endCap = D2D1_CAP_STYLE_ROUND;
                    break;
                case NSVG_CAP_SQUARE:
                    strokeProperties.startCap = strokeProperties.endCap = D2D1_CAP_STYLE_SQUARE;
                    break;
                default:
                    break;
                }

                switch (shape->strokeLineJoin)
                {
                case NSVG_JOIN_BEVEL:
                    strokeProperties.lineJoin = D2D1_LINE_JOIN_BEVEL;
                    break;
                case NSVG_JOIN_MITER:
                    strokeProperties.lineJoin = D2D1_LINE_JOIN_MITER;
                    break;
                case NSVG_JOIN_ROUND:
                    strokeProperties.lineJoin = D2D1_LINE_JOIN_ROUND;
                    break;
                default:
                    break;
                }

                winrt::com_ptr<ID2D1StrokeStyle1> strokeStyle;
                ReturnIfFailed(result, m_d2dFactory->CreateStrokeStyle(strokeProperties, NULL, 0, strokeStyle.put()));

                m_d2dContext->DrawGeometry(geometry.get(), blackBrush.get(), shape->strokeWidth, strokeStyle.get());
            }
        }

        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());

        if ((result = m_d2dContext->EndDraw()) == D2DERR_RECREATE_TARGET)
        {
            ReturnIfFailed(result, CreateDeviceResources());
            return InternalDrawSvg(path, foreground, randomAccessStream, dpi, size);
        }

        return SaveImageToStream(targetBitmap.get(), GUID_ContainerFormatPng, randomAccessStream);
    }

    HRESULT PlaceholderImageHelper::InternalDrawIdenticon(IVector<uint8_t> hash, int side, IRandomAccessStream randomAccessStream)
    {
        slim_lock_guard const guard(m_criticalSection);

        HRESULT result;

        m_d2dContext->SetTarget(m_targetBitmap.get());
        m_d2dContext->BeginDraw();

        auto width = side;
        auto height = side;

        if (16 == hash.Size())
        {
            int bitPointer = 0;
            float rectSize = (float)std::floor(std::min(width, height) / 8.0f);
            float xOffset = std::max(0.0f, (width - rectSize * 8) / 2);
            float yOffset = std::max(0.0f, (height - rectSize * 8) / 2);
            for (int iy = 0; iy < 8; iy++)
            {
                for (int ix = 0; ix < 8; ix++)
                {
                    int byteValue = (hash.GetAt(bitPointer / 8) >> (bitPointer % 8)) & 0x3;
                    bitPointer += 2;
                    int colorIndex = std::abs(byteValue) % 4;
                    D2D1_RECT_F layoutRect = { (int)(xOffset + ix * rectSize), (int)(iy * rectSize + yOffset), (int)(xOffset + ix * rectSize + rectSize), (int)(iy * rectSize + rectSize + yOffset) };
                    m_d2dContext->FillRectangle(layoutRect, m_identiconBrushes[colorIndex].get());
                }
            }
        }
        else
        {
            int bitPointer = 0;
            float rectSize = (float)std::floor(std::min(width, height) / 12.0f);
            float xOffset = std::max(0.0f, (width - rectSize * 12) / 2);
            float yOffset = std::max(0.0f, (height - rectSize * 12) / 2);
            for (int iy = 0; iy < 12; iy++)
            {
                for (int ix = 0; ix < 12; ix++)
                {
                    int byteValue = (hash.GetAt(bitPointer / 8) >> (bitPointer % 8)) & 0x3;
                    int colorIndex = std::abs(byteValue) % 4;
                    D2D1_RECT_F layoutRect = { (int)(xOffset + ix * rectSize), (int)(iy * rectSize + yOffset), (int)(xOffset + ix * rectSize + rectSize), (int)(iy * rectSize + rectSize + yOffset) };
                    m_d2dContext->FillRectangle(layoutRect, m_identiconBrushes[colorIndex].get());
                    bitPointer += 2;
                }
            }
        }

        if ((result = m_d2dContext->EndDraw()) == D2DERR_RECREATE_TARGET)
        {
            ReturnIfFailed(result, CreateDeviceResources());
            return InternalDrawIdenticon(hash, side, randomAccessStream);
        }

        return SaveImageToStream(m_targetBitmap.get(), GUID_ContainerFormatPng, randomAccessStream);
    }

    HRESULT PlaceholderImageHelper::InternalDrawThumbnailPlaceholder(hstring fileName, float blurAmount, IRandomAccessStream randomAccessStream)
    {
        slim_lock_guard const guard(m_criticalSection);

        HANDLE file = CreateFile2FromAppW(fileName.data(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            return ERROR_FILE_NOT_FOUND;
        }

        HRESULT result;
        winrt::com_ptr<IWICBitmapDecoder> wicBitmapDecoder;
        //ReturnIfFailed(result, m_wicFactory->CreateDecoderFromFilename(fileName->Data(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &wicBitmapDecoder));
        ReturnIfFailed(result, m_wicFactory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file), nullptr, WICDecodeMetadataCacheOnLoad, wicBitmapDecoder.put()));

        winrt::com_ptr<IWICBitmapFrameDecode> wicFrameDecode;
        ReturnIfFailed(result, wicBitmapDecoder->GetFrame(0, wicFrameDecode.put()));

        winrt::com_ptr<IWICFormatConverter> wicFormatConverter;
        ReturnIfFailed(result, m_wicFactory->CreateFormatConverter(wicFormatConverter.put()));
        ReturnIfFailed(result, wicFormatConverter->Initialize(wicFrameDecode.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom));

        ReturnIfFailed(result, InternalDrawThumbnailPlaceholder(wicFormatConverter.get(), blurAmount, randomAccessStream, false));

        CloseHandle(file);

        return result;
    }

    HRESULT PlaceholderImageHelper::InternalDrawThumbnailPlaceholder(IVector<uint8_t> bytes, float blurAmount, IRandomAccessStream randomAccessStream)
    {
        slim_lock_guard const guard(m_criticalSection);

        HRESULT result;
        winrt::com_ptr<IStream> stream;
        ReturnIfFailed(result, CreateStreamOverRandomAccessStream(winrt::get_unknown(randomAccessStream), IID_PPV_ARGS(&stream)));

        auto yolo = std::vector<byte>(bytes.begin(), bytes.end());

        ReturnIfFailed(result, stream->Write(yolo.data(), bytes.Size(), nullptr));
        ReturnIfFailed(result, stream->Seek({ 0 }, STREAM_SEEK_SET, nullptr));

        winrt::com_ptr<IWICBitmapDecoder> wicBitmapDecoder;
        ReturnIfFailed(result, m_wicFactory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, wicBitmapDecoder.put()));

        winrt::com_ptr<IWICBitmapFrameDecode> wicFrameDecode;
        ReturnIfFailed(result, wicBitmapDecoder->GetFrame(0, wicFrameDecode.put()));

        winrt::com_ptr<IWICFormatConverter> wicFormatConverter;
        ReturnIfFailed(result, m_wicFactory->CreateFormatConverter(wicFormatConverter.put()));
        ReturnIfFailed(result, wicFormatConverter->Initialize(wicFrameDecode.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom));

        ReturnIfFailed(result, InternalDrawThumbnailPlaceholder(wicFormatConverter.get(), blurAmount, randomAccessStream, true));

        return result;
    }

    HRESULT PlaceholderImageHelper::InternalDrawThumbnailPlaceholder(IWICBitmapSource* wicBitmapSource, float blurAmount, IRandomAccessStream randomAccessStream, bool minithumbnail)
    {
        HRESULT result;
        winrt::com_ptr<ID2D1ImageSourceFromWic> imageSource;
        ReturnIfFailed(result, m_d2dContext->CreateImageSourceFromWic(wicBitmapSource, imageSource.put()));

        D2D1_SIZE_U size;
        ReturnIfFailed(result, wicBitmapSource->GetSize(&size.width, &size.height));

        //if (minithumbnail) {
        //	size.width *= 2;
        //	size.height *= 2;
        //}

        winrt::com_ptr<ID2D1Bitmap1> targetBitmap;
        D2D1_BITMAP_PROPERTIES1 properties = { { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96, D2D1_BITMAP_OPTIONS_TARGET, 0 };
        ReturnIfFailed(result, m_d2dContext->CreateBitmap(size, nullptr, 0, &properties, targetBitmap.put()));

        //winrt::com_ptr<ID2D1Effect> scaleEffect;
        //ReturnIfFailed(result, m_d2dContext->CreateEffect(CLSID_D2D1Scale, scaleEffect.put()));
        //ReturnIfFailed(result, scaleEffect->SetValue(D2D1_SCALE_PROP_SCALE, D2D1_VECTOR_2F({ 2, 2 })));
        //ReturnIfFailed(result, scaleEffect->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_NEAREST_NEIGHBOR));
        //scaleEffect->SetInput(0, imageSource.get());

        //winrt::com_ptr<ID2D1Image> test;
        //scaleEffect->SetInput(0, imageSource.get());
        //scaleEffect->GetOutput(test.put());

        ReturnIfFailed(result, m_gaussianBlurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blurAmount));

        //m_gaussianBlurEffect->SetInput(0, test.get());
        m_gaussianBlurEffect->SetInput(0, imageSource.get());

        m_d2dContext->SetTarget(targetBitmap.get());
        m_d2dContext->BeginDraw();
        //m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        m_d2dContext->Clear(D2D1::ColorF(ColorF::Black, 0.0f));
        m_d2dContext->DrawImage(m_gaussianBlurEffect.get());

        if ((result = m_d2dContext->EndDraw()) == D2DERR_RECREATE_TARGET)
        {
            ReturnIfFailed(result, CreateDeviceResources());
            return InternalDrawThumbnailPlaceholder(wicBitmapSource, blurAmount, randomAccessStream, minithumbnail);
        }

        return SaveImageToStream(targetBitmap.get(), GUID_ContainerFormatPng, randomAccessStream);
    }

    PlaceholderImageHelper::PlaceholderImageHelper()
    {
        winrt::check_hresult(CreateDeviceIndependentResources());
        winrt::check_hresult(CreateDeviceResources());
    }

    HRESULT PlaceholderImageHelper::CreateDeviceIndependentResources()
    {
        HRESULT result;
        D2D1_FACTORY_OPTIONS options = {};
        ReturnIfFailed(result, D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, m_d2dFactory.put_void()));
        ReturnIfFailed(result, CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory)));
        ReturnIfFailed(result, DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)m_dwriteFactory.put()));



        hstring path1 = Package::Current().InstalledLocation().Path() + L"\\Assets\\Fonts\\Telegram.ttf";
        hstring path2 = Package::Current().InstalledLocation().Path() + L"\\Assets\\Emoji\\apple.ttf";

        auto keySize = path1.size() + path2.size();
        const wchar_t* keys[]
        {
            path1.c_str(),
            path2.c_str()
        };

        m_customLoader = winrt::make_self<CustomFontLoader>();

        ReturnIfFailed(result, m_dwriteFactory->RegisterFontCollectionLoader(m_customLoader.get()));
        ReturnIfFailed(result, m_dwriteFactory->CreateCustomFontCollection(m_customLoader.get(), keys, keySize, m_fontCollection.put()));
        ReturnIfFailed(result, m_dwriteFactory->GetSystemFontCollection(m_systemCollection.put()));

        ReturnIfFailed(result, m_dwriteFactory->CreateTextFormat(
            L"Telegram",							// font family name
            m_fontCollection.get(),					// system font collection
            DWRITE_FONT_WEIGHT_NORMAL,				// font weight 
            DWRITE_FONT_STYLE_NORMAL,				// font style
            DWRITE_FONT_STRETCH_NORMAL,				// default font stretch
            92.0f,									// font size
            L"",									// locale name
            m_symbolFormat.put()
        ));
        ReturnIfFailed(result, m_symbolFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
        ReturnIfFailed(result, m_symbolFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));

        ReturnIfFailed(result, m_dwriteFactory->CreateTextFormat(
            L"Segoe MDL2 Assets",					// font family name
            nullptr,								// system font collection
            DWRITE_FONT_WEIGHT_NORMAL,				// font weight 
            DWRITE_FONT_STYLE_NORMAL,				// font style
            DWRITE_FONT_STRETCH_NORMAL,				// default font stretch
            82.0f,									// font size
            L"",									// locale name
            m_mdl2Format.put()
        ));
        ReturnIfFailed(result, m_mdl2Format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
        ReturnIfFailed(result, m_mdl2Format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));

        ReturnIfFailed(result, m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",							// font family name
            nullptr,								// system font collection
            DWRITE_FONT_WEIGHT_NORMAL,				// font weight 
            DWRITE_FONT_STYLE_NORMAL,				// font style
            DWRITE_FONT_STRETCH_NORMAL,				// default font stretch
            82.0f,									// font size
            L"",									// locale name
            m_textFormat.put()
        ));
        ReturnIfFailed(result, m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
        return m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    HRESULT PlaceholderImageHelper::CreateDeviceResources()
    {
        HRESULT result;
        UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        winrt::com_ptr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr,               // specify null to use the default adapter
            D3D_DRIVER_TYPE_HARDWARE, 0,
            creationFlags,									// optionally set debug and Direct2D compatibility flags
            NULL,											// list of feature levels this app can support
            0,												// number of possible feature levels
            D3D11_SDK_VERSION,
            m_d3dDevice.put(),								// returns the Direct3D device created
            &m_featureLevel,								// returns feature level of device created
            context.put()									// returns the device immediate context
        )))
        {
            // Try again using WARP (software rendering)
            ReturnIfFailed(result, D3D11CreateDevice(nullptr,
                D3D_DRIVER_TYPE_WARP, 0,
                creationFlags,
                NULL,
                0,
                D3D11_SDK_VERSION,
                m_d3dDevice.put(),
                &m_featureLevel,
                context.put()
            ));
        }

        winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
        ReturnIfFailed(result, m_d2dFactory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));

        winrt::com_ptr<ID2D1DeviceContext> d2dContext;
        ReturnIfFailed(result, m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext.put()));
        m_d2dContext = d2dContext.as<ID2D1DeviceContext2>();

        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_textBrush.put()));
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), m_black.put()));
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_transparent.put()));
        ReturnIfFailed(result, m_d2dContext->CreateEffect(CLSID_D2D1GaussianBlur, m_gaussianBlurEffect.put()));
        ReturnIfFailed(result, m_gaussianBlurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD));

        /*            Color.FromArgb(0xff, 0xff, 0xff, 0xff),
                Color.FromArgb(0xff, 0xd5, 0xe6, 0xf3),
                Color.FromArgb(0xff, 0x2d, 0x57, 0x75),
                Color.FromArgb(0xff, 0x2f, 0x99, 0xc9)
    */

        winrt::com_ptr<ID2D1SolidColorBrush> color1;
        winrt::com_ptr<ID2D1SolidColorBrush> color2;
        winrt::com_ptr<ID2D1SolidColorBrush> color3;
        winrt::com_ptr<ID2D1SolidColorBrush> color4;
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), color1.put()));
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0xd5 / 255.0f, 0xe6 / 255.0f, 0xf3 / 255.0f, 1.0f), color2.put()));
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0x2d / 255.0f, 0x57 / 255.0f, 0x75 / 255.0f, 1.0f), color3.put()));
        ReturnIfFailed(result, m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0x2f / 255.0f, 0x99 / 255.0f, 0xc9 / 255.0f, 1.0f), color4.put()));

        m_identiconBrushes.push_back(color1);
        m_identiconBrushes.push_back(color2);
        m_identiconBrushes.push_back(color3);
        m_identiconBrushes.push_back(color4);

        D2D1_SIZE_U size = { 192, 192 };
        D2D1_BITMAP_PROPERTIES1 properties = { { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96, D2D1_BITMAP_OPTIONS_TARGET, 0 };
        ReturnIfFailed(result, m_d2dContext->CreateBitmap(size, nullptr, 0, &properties, m_targetBitmap.put()));

        m_d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        return m_wicFactory->CreateImageEncoder(m_d2dDevice.get(), m_imageEncoder.put());
    }

    HRESULT PlaceholderImageHelper::MeasureText(const wchar_t* text, IDWriteTextFormat* format, DWRITE_TEXT_METRICS* textMetrics)
    {
        HRESULT result;
        winrt::com_ptr<IDWriteTextLayout> textLayout;
        ReturnIfFailed(result, m_dwriteFactory->CreateTextLayout(
            text,							// The string to be laid out and formatted.
            wcslen(text),					// The length of the string.
            format,							// The text format to apply to the string (contains font information, etc).
            192.0f,							// The width of the layout box.
            192.0f,							// The height of the layout box.
            textLayout.put()				// The IDWriteTextLayout interface pointer.
        ));

        return textLayout->GetMetrics(textMetrics);
    }

    float2 PlaceholderImageHelper::ContentEnd(hstring text, IVector<PlaceholderEntity> entities, double fontSize, double width)
    {
        winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI Emoji",						// font family name
            m_fontCollection.get(),			        // system font collection
            DWRITE_FONT_WEIGHT_NORMAL,				// font weight 
            DWRITE_FONT_STYLE_NORMAL,				// font style
            DWRITE_FONT_STRETCH_NORMAL,				// default font stretch
            fontSize,								// font size
            L"",									// locale name
            m_appleFormat.put()
        ));
        winrt::check_hresult(m_appleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
        winrt::check_hresult(m_appleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));

        winrt::com_ptr<IDWriteTextLayout> textLayout;
        winrt::check_hresult(m_dwriteFactory->CreateTextLayout(
            text.data(),					// The string to be laid out and formatted.
            wcslen(text.data()),			// The length of the string.
            m_appleFormat.get(),			// The text format to apply to the string (contains font information, etc).
            width,							// The width of the layout box.
            INFINITY,						// The height of the layout box.
            textLayout.put()				// The IDWriteTextLayout interface pointer.
        ));

        for (const PlaceholderEntity& entity : entities)
        {
            UINT32 startPosition = entity.Offset;
            UINT32 length = entity.Length;

            switch (entity.Type)
            {
            case PlaceholderEntityType::Bold:
                textLayout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, { startPosition, length });
                break;
            case PlaceholderEntityType::Italic:
                textLayout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, { startPosition, length });
                break;
            case PlaceholderEntityType::Strikethrough:
                textLayout->SetStrikethrough(TRUE, { startPosition, length });
                break;
            case PlaceholderEntityType::Underline:
                textLayout->SetUnderline(TRUE, { startPosition, length });
                break;
            case PlaceholderEntityType::Code:
                textLayout->SetFontCollection(m_systemCollection.get(), { startPosition, length });
                textLayout->SetFontFamilyName(L"Consolas", { startPosition, length });
                break;
            default:
                break;
            }
        }

        FLOAT x;
        FLOAT y;
        DWRITE_HIT_TEST_METRICS metrics;
        textLayout->HitTestTextPosition(text.size() - 1, false, &x, &y, &metrics);

        return float2(metrics.left + metrics.width, metrics.top + metrics.height);
    }

    IVector<Windows::Foundation::Rect> PlaceholderImageHelper::LineMetrics(hstring text, double fontSize, double width, bool rtl)
    {
        winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI Emoji",						// font family name
            m_fontCollection.get(),		            // system font collection
            DWRITE_FONT_WEIGHT_NORMAL,				// font weight 
            DWRITE_FONT_STYLE_NORMAL,				// font style
            DWRITE_FONT_STRETCH_NORMAL,				// default font stretch
            fontSize,								// font size
            L"",									// locale name
            m_appleFormat.put()
        ));
        winrt::check_hresult(m_appleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
        winrt::check_hresult(m_appleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
        winrt::check_hresult(m_appleFormat->SetReadingDirection(rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT));

        winrt::com_ptr<IDWriteTextLayout> textLayout;
        winrt::check_hresult(m_dwriteFactory->CreateTextLayout(
            text.data(),					// The string to be laid out and formatted.
            wcslen(text.data()),			// The length of the string.
            m_appleFormat.get(),			// The text format to apply to the string (contains font information, etc).
            width,							// The width of the layout box.
            INFINITY,						// The height of the layout box.
            textLayout.put()				// The IDWriteTextLayout interface pointer.
        ));

        DWRITE_TEXT_METRICS metrics;
        winrt::check_hresult(textLayout->GetMetrics(&metrics));

        UINT32 maxHitTestMetricsCount = metrics.lineCount * metrics.maxBidiReorderingDepth;
        UINT32 actualTestsCount;
        DWRITE_HIT_TEST_METRICS* ranges = new DWRITE_HIT_TEST_METRICS[maxHitTestMetricsCount];
        winrt::check_hresult(textLayout->HitTestTextRange(0, text.size(), 0, 0, ranges, maxHitTestMetricsCount, &actualTestsCount));

        std::vector<Windows::Foundation::Rect> rects;

        for (int i = 0; i < actualTestsCount; i++)
        {
            float left = ranges[i].left;
            float top = ranges[i].top;
            float right = ranges[i].left + ranges[i].width;
            float bottom = ranges[i].top + ranges[i].height;

            rects.push_back({ left, top, right - left, bottom - top });
        }

        return winrt::single_threaded_vector<Windows::Foundation::Rect>(std::move(rects));
    }

    //IVector<Windows::Foundation::Rect> PlaceholderImageHelper::EntityMetrics(hstring text, IVector<TextEntity> entities, double fontSize, double width, bool rtl)
    //{
    //	winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
    //		L"Segoe UI Emoji",						// font family name
    //		m_appleCollection.get(),				// system font collection
    //		DWRITE_FONT_WEIGHT_NORMAL,				// font weight 
    //		DWRITE_FONT_STYLE_NORMAL,				// font style
    //		DWRITE_FONT_STRETCH_NORMAL,				// default font stretch
    //		fontSize,								// font size
    //		L"",									// locale name
    //		m_appleFormat.put()
    //	));
    //	winrt::check_hresult(m_appleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
    //	winrt::check_hresult(m_appleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
    //	winrt::check_hresult(m_appleFormat->SetReadingDirection(rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT));

    //	winrt::com_ptr<IDWriteTextLayout> textLayout;
    //	winrt::check_hresult(m_dwriteFactory->CreateTextLayout(
    //		text.data(),					// The string to be laid out and formatted.
    //		wcslen(text.data()),			// The length of the string.
    //		m_appleFormat.get(),			// The text format to apply to the string (contains font information, etc).
    //		width,							// The width of the layout box.
    //		INFINITY,						// The height of the layout box.
    //		textLayout.put()				// The IDWriteTextLayout interface pointer.
    //	));

    //	DWRITE_TEXT_METRICS metrics;
    //	winrt::check_hresult(textLayout->GetMetrics(&metrics));

    //	UINT32 maxHitTestMetricsCount = metrics.lineCount * metrics.maxBidiReorderingDepth;
    //	DWRITE_HIT_TEST_METRICS* ranges = new DWRITE_HIT_TEST_METRICS[maxHitTestMetricsCount];

    //	std::vector<Windows::Foundation::Rect> rects;

    //	for (const TextEntity& entity : entities) {
    //		auto spoiler = entity.Type().try_as<TextEntityTypeSpoiler>();
    //		if (spoiler != nullptr) {
    //			UINT32 actualTestsCount;
    //			winrt::check_hresult(textLayout->HitTestTextRange(entity.Offset(), entity.Length(), 0, 0, ranges, maxHitTestMetricsCount, &actualTestsCount));

    //			for (int i = 0; i < actualTestsCount; i++) {
    //				float left = ranges[i].left;
    //				float top = ranges[i].top;
    //				float right = ranges[i].left + ranges[i].width;
    //				float bottom = ranges[i].top + ranges[i].height;

    //				rects.push_back({ left, top, right - left, bottom - top });
    //			}
    //		}
    //	}

    //	return winrt::single_threaded_vector<Windows::Foundation::Rect>(std::move(rects));
    //}

    void PlaceholderImageHelper::WriteBytes(IVector<byte> hash, IRandomAccessStream randomAccessStream) noexcept
    {
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(randomAccessStream), IID_PPV_ARGS(&stream)));

        auto yolo = std::vector<byte>(hash.begin(), hash.end());

        winrt::check_hresult(stream->Write(yolo.data(), hash.Size(), nullptr));
        winrt::check_hresult(stream->Seek({ 0 }, STREAM_SEEK_SET, nullptr));
    }

    HRESULT PlaceholderImageHelper::SaveImageToStream(ID2D1Image* image, REFGUID wicFormat, IRandomAccessStream randomAccessStream)
    {
        HRESULT result;
        winrt::com_ptr<IStream> stream;
        ReturnIfFailed(result, CreateStreamOverRandomAccessStream(winrt::get_unknown(randomAccessStream), IID_PPV_ARGS(&stream)));

        if (randomAccessStream.Size())
        {
            stream->SetSize({ 0 });
        }

        winrt::com_ptr<IWICBitmapEncoder> wicBitmapEncoder;
        ReturnIfFailed(result, m_wicFactory->CreateEncoder(wicFormat, nullptr, wicBitmapEncoder.put()));
        ReturnIfFailed(result, wicBitmapEncoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

        winrt::com_ptr<IWICBitmapFrameEncode> wicFrameEncode;
        ReturnIfFailed(result, wicBitmapEncoder->CreateNewFrame(wicFrameEncode.put(), nullptr));
        ReturnIfFailed(result, wicFrameEncode->Initialize(nullptr));

        ReturnIfFailed(result, m_imageEncoder->WriteFrame(image, wicFrameEncode.get(), nullptr));
        ReturnIfFailed(result, wicFrameEncode->Commit());
        ReturnIfFailed(result, wicBitmapEncoder->Commit());

        ReturnIfFailed(result, stream->Commit(STGC_DEFAULT));

        return stream->Seek({ 0 }, STREAM_SEEK_SET, nullptr);
    }
} // namespace winrt::Telegram::Native::implementation