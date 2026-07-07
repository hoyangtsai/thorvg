/*
 * Copyright (c) 2026 ThorVG project. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <math.h>
#include <atomic>
#include <mutex>

#include "tvgMfMediaLoader.h"

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/

// Media Foundation timestamps are in 100-nanosecond units.
static constexpr auto HNS_SECOND = 10000000.0f;

static std::once_flag _mfOnce;  // MFStartup() is process-global.

// Receives media engine events on Media Foundation worker threads.
struct MfNotify : IMFMediaEngineNotify
{
    // Manual-reset events: open() blocks on them like the gst preroll wait.
    HANDLE loadedEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // metadata (size/duration) is available
    HANDLE readyEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // the first frame is decoded and transferable
    std::atomic<uint32_t> error{0};  // MF_MEDIA_ENGINE_ERR code; consumed by _pump()
    std::atomic<bool> failed{false};
    LONG refCnt = 1;

    virtual ~MfNotify()
    {
        CloseHandle(loadedEvt);
        CloseHandle(readyEvt);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
    {
        if (!obj) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *obj = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refCnt);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        auto cnt = InterlockedDecrement(&refCnt);
        if (cnt == 0) delete(this);
        return cnt;
    }

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1, TVG_UNUSED DWORD param2) override
    {
        switch (event) {
            case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: SetEvent(loadedEvt); break;
            case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY: SetEvent(readyEvt); break;
            case MF_MEDIA_ENGINE_EVENT_ERROR: {
                error = static_cast<uint32_t>(param1);
                failed = true;
                // Failure also releases any open() waiter.
                SetEvent(loadedEvt);
                SetEvent(readyEvt);
                break;
            }
            default: break;
        }
        return S_OK;
    }
};

struct MfImpl
{
    MfMediaLoader* loader = nullptr;

    // The media engine drives decoding, clocking, and audio output (SAR).
    IMFMediaEngine* engine = nullptr;
    MfNotify* notify = nullptr;

    // D3D11 frame-server surfaces for BGRA readback into the loader surface.
    IMFDXGIDeviceManager* manager = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* texture = nullptr;  // render target for TransferVideoFrame()
    ID3D11Texture2D* staging = nullptr;  // cpu-readable copy of texture
};

static float _seconds(LONGLONG time)
{
    if (time < 0) return 0.0f;
    return static_cast<float>(time) / HNS_SECOND;
}

template<typename T>
static void _release(T*& obj)
{
    if (!obj) return;
    obj->Release();
    obj = nullptr;
}

// WARP keeps decoding working on machines without a usable GPU.
static bool _device(MfImpl* impl)
{
    constexpr auto flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &impl->device, nullptr, &impl->context)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &impl->device, nullptr, &impl->context))) return false;

    // The engine decodes on its own threads; the device must be multithread-protected.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(impl->device->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &impl->manager))) return false;
    return SUCCEEDED(impl->manager->ResetDevice(impl->device, token));
}

// Textures for TransferVideoFrame() readback; the size is fixed after open().
static bool _prepare(MfImpl* impl, uint32_t width, uint32_t height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // == ColorSpace::ARGB8888S in memory
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(impl->device->CreateTexture2D(&desc, nullptr, &impl->texture))) return false;

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    return SUCCEEDED(impl->device->CreateTexture2D(&desc, nullptr, &impl->staging));
}

static void _finishPlayback(MfImpl* impl)
{
    impl->engine->Pause();
    impl->loader->paused = true;
    impl->loader->curTime = impl->loader->totalTime;
}

// Handle end-of-stream and errors at render cadence; looping is gapless via SetLoop().
static void _pump(MfImpl* impl)
{
    if (impl->notify->failed.exchange(false)) {
        TVGLOG("MF", "Media engine error: %u", impl->notify->error.load());
        _finishPlayback(impl);
        return;
    }

    if (impl->engine->IsEnded() && !impl->loader->paused) _finishPlayback(impl);
}

static void _close(MfImpl* impl)
{
    if (impl->engine) {
        impl->engine->Shutdown();  // stops playback and the engine threads
        impl->engine->Release();
        impl->engine = nullptr;
    }

    // The engine may still hold a reference; the notify is self-contained and refcounted.
    _release(impl->notify);
    _release(impl->texture);
    _release(impl->staging);
    _release(impl->context);
    _release(impl->device);
    _release(impl->manager);
}

static BSTR _url(const char* path)
{
    auto len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (len <= 0) return nullptr;

    auto wpath = tvg::malloc<wchar_t>(len * sizeof(wchar_t));
    if (!wpath) return nullptr;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, len);
    auto url = SysAllocString(wpath);
    tvg::free(wpath);
    return url;
}

static bool _open(MfImpl* impl, const char* path)
{
    if (!_device(impl)) {
        TVGLOG("MF", "Failed to create a D3D11 device: %s", path);
        return false;
    }

    IMFMediaEngineClassFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFMediaEngineClassFactory), reinterpret_cast<void**>(&factory)))) {
        TVGLOG("MF", "Missing media engine class factory: %s", path);
        return false;
    }

    impl->notify = new MfNotify;

    IMFAttributes* attrs = nullptr;
    auto ok = SUCCEEDED(MFCreateAttributes(&attrs, 3)) &&
        SUCCEEDED(attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, impl->notify)) &&
        SUCCEEDED(attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, impl->manager)) &&
        SUCCEEDED(attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM)) &&
        SUCCEEDED(factory->CreateInstance(0, attrs, &impl->engine));

    _release(attrs);
    _release(factory);
    if (!ok) return false;

    auto url = _url(path);
    if (!url) return false;

    // Async load: LOADEDMETADATA/FIRSTFRAMEREADY (or ERROR) arrive on the notify.
    impl->engine->SetAutoPlay(FALSE);
    impl->engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    ok = SUCCEEDED(impl->engine->SetSource(url));
    SysFreeString(url);
    return ok;
}

/************************************************************************/
/* External Class Implementation                                        */
/************************************************************************/

MfMediaLoader::MfMediaLoader() :
    MediaLoader(FileType::Media),
    pImpl(new MfImpl)
{
    pImpl->loader = this;
}

MfMediaLoader::~MfMediaLoader()
{
    _close(pImpl);
    tvg::free(surface.data);
    delete pImpl;
}

bool MfMediaLoader::open(const char* path, TVG_UNUSED const LoaderOps* ops)
{
    if (!path) return false;

    // COM is per-thread; RPC_E_CHANGED_MODE means the app owns an apartment here already.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::call_once(_mfOnce, []() { MFStartup(MF_VERSION, MFSTARTUP_FULL); });

    if (!_open(pImpl, path)) {
        TVGLOG("MF", "Failed to open media: %s", path);
        _close(pImpl);
        return false;
    }

    // Block until the metadata is ready, mirroring the gst preroll to PAUSED.
    WaitForSingleObject(pImpl->notify->loadedEvt, INFINITE);
    if (pImpl->notify->failed) {
        TVGLOG("MF", "Failed to open media: %s (error: %u)", path, pImpl->notify->error.load());
        _close(pImpl);
        return false;
    }

    DWORD width = 0, height = 0;
    if (pImpl->engine->HasVideo() != TRUE || FAILED(pImpl->engine->GetNativeVideoSize(&width, &height)) || width == 0 || height == 0) {
        TVGLOG("MF", "No video track found: %s", path);
        _close(pImpl);
        return false;
    }

    auto duration = pImpl->engine->GetDuration();
    if (!isfinite(duration) || duration <= 0.0) {
        TVGLOG("MF", "Invalid media duration: %s", path);
        _close(pImpl);
        return false;
    }

    // Wait for the first transferable still so Picture can render right after load();
    // a timeout is not fatal since sync() picks the frame up once it lands.
    if (WaitForSingleObject(pImpl->notify->readyEvt, 10000) != WAIT_OBJECT_0) TVGLOG("MF", "First frame not ready yet: %s", path);
    if (pImpl->notify->failed) {
        TVGLOG("MF", "Failed to decode the first frame: %s", path);
        _close(pImpl);
        return false;
    }

    w = static_cast<float>(width);
    h = static_cast<float>(height);
    totalTime = static_cast<float>(duration);
    curTime = 0.0f;

    return true;
}

bool MfMediaLoader::read()
{
    if (!Loader::read()) return true;
    if (!pImpl->engine || w == 0 || h == 0) return false;

    surface.cs = ColorSpace::ARGB8888S;
    surface.w = static_cast<uint32_t>(w);
    surface.h = static_cast<uint32_t>(h);
    surface.stride = surface.w;
    surface.channelSize = sizeof(uint32_t);
    surface.premultiplied = false;
    surface.alphaIgnored = true;

    if (!_prepare(pImpl, surface.w, surface.h)) return false;

    pImpl->engine->SetVolume(static_cast<double>(audioVolume));
    pImpl->engine->SetMuted(static_cast<BOOL>(muted));
    paused = true;

    // The first frame is already decoded during open(); publish it now.
    sync();

    return true;
}

bool MfMediaLoader::close()
{
    if (!Loader::close()) return false;

    _close(pImpl);
    return true;
}

bool MfMediaLoader::sync()
{
    if (!pImpl->engine) return false;

    _pump(pImpl);

    // Frame-server mode: poll for a new frame at render cadence.
    LONGLONG pts = 0;
    if (pImpl->engine->OnVideoStreamTick(&pts) != S_OK) return sharing > 0;

    curTime = _seconds(pts);

    RECT rect = {0, 0, static_cast<LONG>(surface.w), static_cast<LONG>(surface.h)};
    MFARGB border = {0, 0, 0, 255};
    if (FAILED(pImpl->engine->TransferVideoFrame(pImpl->texture, nullptr, &rect, &border))) return false;

    // GPU → CPU readback through the staging texture.
    pImpl->context->CopyResource(pImpl->staging, pImpl->texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(pImpl->context->Map(pImpl->staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;

    auto size = surface.stride * surface.h * surface.channelSize;
    if (!surface.data) surface.data = tvg::malloc<pixel_t>(size);

    if (surface.data) {
        auto src = static_cast<uint8_t*>(mapped.pData);
        auto rowBytes = surface.w * sizeof(uint32_t);
        if (mapped.RowPitch == rowBytes) memcpy(surface.data, src, rowBytes * surface.h);
        else {
            for (auto y = 0U; y < surface.h; ++y) {
                memcpy(reinterpret_cast<uint8_t*>(surface.data) + y * rowBytes, src + y * mapped.RowPitch, rowBytes);
            }
        }
        surface.cs = ColorSpace::ARGB8888S;   // rasterConvertCS() can update this.
        surface.premultiplied = false;        // rasterPremultiply() can update this.
    }

    pImpl->context->Unmap(pImpl->staging, 0);

    return surface.data != nullptr;
}

Result MfMediaLoader::play()
{
    if (!pImpl->engine) return Result::InsufficientCondition;

    // Restart from the beginning when playback already reached the end.
    if (!looping && totalTime > 0.0f && curTime >= totalTime) pImpl->engine->SetCurrentTime(0.0);

    paused = false;
    pImpl->engine->Play();
    return Result::Success;
}

Result MfMediaLoader::pause()
{
    if (!pImpl->engine) return Result::InsufficientCondition;

    paused = true;
    pImpl->engine->Pause();
    return Result::Success;
}

Result MfMediaLoader::stop()
{
    if (!pImpl->engine) return Result::InsufficientCondition;

    paused = true;
    curTime = 0.0f;
    pImpl->engine->Pause();

    // The seeked still at 0 arrives via OnVideoStreamTick() on the next sync().
    pImpl->engine->SetCurrentTime(0.0);
    return Result::Success;
}

Result MfMediaLoader::seek(float seconds)
{
    if (seconds < 0.0f || (totalTime > 0.0f && seconds > totalTime)) return Result::InvalidArguments;
    if (!pImpl->engine) return Result::InsufficientCondition;

    curTime = seconds;
    pImpl->engine->SetCurrentTime(static_cast<double>(seconds));
    return Result::Success;
}

Result MfMediaLoader::loop(bool on)
{
    if (!pImpl->engine) return Result::InsufficientCondition;

    looping = on;
    pImpl->engine->SetLoop(static_cast<BOOL>(on));  // the engine rolls over gaplessly by itself
    return Result::Success;
}

Result MfMediaLoader::volume(float volume)
{
    if (!pImpl->engine) return Result::InsufficientCondition;

    audioVolume = volume;
    pImpl->engine->SetVolume(static_cast<double>(volume));
    return Result::Success;
}

Result MfMediaLoader::mute(bool on)
{
    if (!pImpl->engine) return Result::InsufficientCondition;

    muted = on;
    pImpl->engine->SetMuted(static_cast<BOOL>(on));
    return Result::Success;
}
