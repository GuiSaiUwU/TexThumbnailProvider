// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include <shlwapi.h>
#include <Wincrypt.h>   // For CryptStringToBinary.
#include <thumbcache.h> // For IThumbnailProvider.
#include <wincodec.h>   // Windows Imaging Codecs
#include <msxml6.h>
#include <new>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "msxml6.lib")

// this thumbnail provider implements IInitializeWithStream to enable being hosted
// in an isolated process for robustness

#include "s3tc.h"

class CRecipeThumbProvider : public IInitializeWithStream,
                             public IThumbnailProvider
{
public:
    CRecipeThumbProvider() : _cRef(1), _pStream(NULL)
    {
    }

    virtual ~CRecipeThumbProvider()
    {
        if (_pStream)
        {
            _pStream->Release();
        }
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(CRecipeThumbProvider, IInitializeWithStream),
            QITABENT(CRecipeThumbProvider, IThumbnailProvider),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        ULONG cRef = InterlockedDecrement(&_cRef);
        if (!cRef)
        {
            delete this;
        }
        return cRef;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream *pStream, DWORD grfMode);

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha);

private:
    long _cRef;
    IStream *_pStream;     // provided during initialization.
};

HRESULT CRecipeThumbProvider_CreateInstance(REFIID riid, void **ppv)
{
    CRecipeThumbProvider *pNew = new (std::nothrow) CRecipeThumbProvider();
    HRESULT hr = pNew ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pNew->QueryInterface(riid, ppv);
        pNew->Release();
    }
    return hr;
}

// IInitializeWithStream
IFACEMETHODIMP CRecipeThumbProvider::Initialize(IStream *pStream, DWORD)
{
    HRESULT hr = E_UNEXPECTED;  // can only be inited once
    if (_pStream == NULL)
    {
        // take a reference to the stream if we have not been inited yet
        hr = pStream->QueryInterface(&_pStream);
    }
    return hr;
}

enum tex_format {
    tex_format_etc1 = 0x1,
    tex_format_etc2_eac = 0x2,
    tex_format_etc2 = 0x3,
    tex_format_dxt1 = 0xA,
    tex_format_dxt5 = 0xC,
    tex_format_bgra8 = 0x14
};
#define tex_magic "TEX"

typedef struct {
    uint8_t magic[4];
    uint16_t image_width;
    uint16_t image_height;
    uint8_t unk1;
    uint8_t tex_format;
    uint8_t unk2;
    bool has_mipmaps;
} TEX_HEADER;


void swapUInt8(uint8_t* a, uint8_t* b) {
    uint8_t temp = *a;
    *a = *b;
    *b = temp;
}


HRESULT CreateHBitmapFromTex(IStream* pTexStream, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)
{
    *phbmp = NULL;
    *pdwAlpha = WTSAT_UNKNOWN;

    TEX_HEADER header;
    ULONG bytesRead;

    HRESULT hr = pTexStream->Read(&header, sizeof(header), &bytesRead);
    if (FAILED(hr) || bytesRead != sizeof(header))
        return E_FAIL;

    if (memcmp(header.magic, "TEX", 3) != 0)
        return E_INVALIDARG;

    const UINT width = header.image_width;
    const UINT height = header.image_height;
    const UINT stride = width * 4;

    if (header.tex_format == tex_format_bgra8)
    {
        const UINT imageSize = stride * height;

        BYTE* pixelData = (BYTE*)malloc(imageSize);
        if (!pixelData)
            return E_OUTOFMEMORY;

        hr = pTexStream->Read(pixelData, imageSize, &bytesRead);
        if (FAILED(hr) || bytesRead != imageSize)
        {
            free(pixelData);
            return E_FAIL;
        }

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -((LONG)height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits;
        HBITMAP hBitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!hBitmap)
        {
            free(pixelData);
            return E_OUTOFMEMORY;
        }

        memcpy(bits, pixelData, imageSize);
        *phbmp = hBitmap;
        *pdwAlpha = WTSAT_ARGB;

        free(pixelData);
        return S_OK;
    }
    else if (header.tex_format == tex_format_dxt5 || header.tex_format == tex_format_dxt1)
    {
        const UINT blockWidth = (width + 3) / 4;
        const UINT blockHeight = (height + 3) / 4;
        const UINT blockSize = header.tex_format == tex_format_dxt5 ? 16 : 8;
        const UINT dataSize = blockWidth * blockHeight * blockSize;

        uint8_t* dxtData = (uint8_t*)malloc(dataSize);
        if (!dxtData)
            return E_OUTOFMEMORY;

        hr = pTexStream->Read(dxtData, dataSize, &bytesRead);
        if (FAILED(hr) || bytesRead != dataSize)
        {
            free(dxtData);
            return E_FAIL;
        }

        // Allocate buffer for output image (BGRA 32-bit)
        unsigned long* image = (unsigned long*)malloc(stride * height);
        if (!image)
        {
            free(dxtData);
            return E_OUTOFMEMORY;
        }

        if (header.tex_format == tex_format_dxt5) {
            BlockDecompressImageDXT5(width, height, dxtData, image);
        }
        else {
            BlockDecompressImageDXT1(width, height, dxtData, image);
        }

        free(dxtData);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -((LONG)height); // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits;

        unsigned long* finalImage = (unsigned long*)malloc(stride * height);
        if (!finalImage)
        {
            free(image);
            return E_OUTOFMEMORY;
        }

        
        for (size_t i = 0; i < width * height; ++i)
        {
            unsigned long rgba = image[i];
            unsigned char r = (rgba >> 24) & 0xFF;
            unsigned char g = (rgba >> 16) & 0xFF;
            unsigned char b = (rgba >> 8) & 0xFF;
            unsigned char a = rgba & 0xFF;

            finalImage[i] = (
                (unsigned long)a << 24) |
                ((unsigned long)r << 16) |
                ((unsigned long)g << 8) |
                ((unsigned long)b);
        }
        
        free(image);
        image = finalImage;

        HBITMAP hBitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!hBitmap)
        {
            free(image);
            return E_OUTOFMEMORY;
        }

        memcpy(bits, image, stride * height);
        *phbmp = hBitmap;
        *pdwAlpha = WTSAT_ARGB;

        free(image);
        return S_OK;
    }

    return E_NOTIMPL;
}


// IThumbnailProvider
IFACEMETHODIMP CRecipeThumbProvider::GetThumbnail(UINT /* cx */, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)
{
    HRESULT hr = CreateHBitmapFromTex(this->_pStream, phbmp, pdwAlpha);

    return hr;
}
