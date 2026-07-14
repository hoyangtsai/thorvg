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

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <cstring>

#include "tvgGstMediaLoader.h"

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/

static constexpr uint32_t BUFFER_COUNT = 3;
static constexpr auto PREROLL_TIMEOUT = 10 * GST_SECOND;
static constexpr gint64 AUDIO_BUFFER_TIME_US = 100000;
static constexpr gint64 AUDIO_LATENCY_TIME_US = 10000;

enum class SegmentState : uint8_t {None, Active, Pending};

struct GstImpl
{
    GstMediaLoader* loader = nullptr;

    GstElement* playbin = nullptr;       // video to appsink; audio to playbin's default sink
    uint32_t seekSeq = 0;                // latest seek seqnum for matching SEGMENT_DONE

    uint32_t* frames[BUFFER_COUNT] = {};
    float latestFrameTime = 0.0f;
    uint32_t write = 0;                  // next frame index
    uint32_t latest = 0;                 // latest published frame index
    SegmentState segment = SegmentState::None;
    bool frameUpdated = false;

    tvg::StrictKey key;
};

static float _nanoToSec(gint64 time)
{
    if (!GST_CLOCK_TIME_IS_VALID(time) || time < 0) return 0.0f;
    return static_cast<float>(time) / static_cast<float>(GST_SECOND);
}

static bool _state(GstImpl* impl, GstState state)
{
    return gst_element_set_state(impl->playbin, state) != GST_STATE_CHANGE_FAILURE;
}

static void _setupAudio(TVG_UNUSED GstElement* playbin, GstElement* element, TVG_UNUSED gpointer data)
{
    auto factory = gst_element_get_factory(element);
    auto klass = factory ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : nullptr;
    if (!klass || !g_strrstr(klass, "Audio") || !g_strrstr(klass, "Sink")) return;

    auto obj = G_OBJECT_GET_CLASS(element);
    if (g_object_class_find_property(obj, "buffer-time")) {
        g_object_set(element, "buffer-time", AUDIO_BUFFER_TIME_US, nullptr);
    }
    if (g_object_class_find_property(obj, "latency-time")) {
        g_object_set(element, "latency-time", AUDIO_LATENCY_TIME_US, nullptr);
    }
}

static bool _push(GstImpl* impl, GstSample* sample)
{
    auto caps = gst_sample_get_caps(sample);
    auto buffer = gst_sample_get_buffer(sample);

    GstVideoInfo info;
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) return false;

    GstVideoFrame vframe;
    if (!gst_video_frame_map(&vframe, &info, buffer, GST_MAP_READ)) return false;

    auto width = static_cast<uint32_t>(GST_VIDEO_FRAME_WIDTH(&vframe));
    auto height = static_cast<uint32_t>(GST_VIDEO_FRAME_HEIGHT(&vframe));
    auto src = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));

    auto ret = false;
    auto lw = static_cast<uint32_t>(impl->loader->w);
    auto lh = static_cast<uint32_t>(impl->loader->h);

    // lw is 0 when the first preroll arrives before open() sets w/h.
    if (src && width > 0 && (lw == 0 || (width == lw && height == lh))) {
        auto srcStride = static_cast<uint32_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0));
        auto rowBytes = width * sizeof(uint32_t);

        auto dst = impl->frames[impl->write];
        if (!dst) dst = impl->frames[impl->write] = tvg::malloc<uint32_t>(rowBytes * height);

        if (dst) {
            if (srcStride == rowBytes) memcpy(dst, src, rowBytes * height);
            else {
                for (auto y = 0U; y < height; ++y) {
                    memcpy(reinterpret_cast<uint8_t*>(dst) + y * rowBytes, src + y * srcStride, rowBytes);
                }
            }

            tvg::ScopedLock lock(impl->key);
            impl->latestFrameTime = _nanoToSec(static_cast<gint64>(GST_BUFFER_PTS(buffer)));
            impl->latest = impl->write;
            impl->write = (impl->write + 1) % BUFFER_COUNT;
            impl->frameUpdated = true;
            ret = true;
        }
    }

    gst_video_frame_unmap(&vframe);
    return ret;
}

// Streaming-thread callback: a seek or preroll produced a still frame.
static GstFlowReturn _onPreroll(GstAppSink* sink, gpointer data)
{
    if (auto sample = gst_app_sink_pull_preroll(sink)) {
        _push(static_cast<GstImpl*>(data), sample);
        gst_sample_unref(sample);
    }
    return GST_FLOW_OK;
}

// Streaming-thread callback: playback produced a new frame.
static GstFlowReturn _onSample(GstAppSink* sink, gpointer data)
{
    if (auto sample = gst_app_sink_pull_sample(sink)) {
        _push(static_cast<GstImpl*>(data), sample);
        gst_sample_unref(sample);
    }
    return GST_FLOW_OK;
}

static void _close(GstImpl* impl)
{
    if (!impl->playbin) return;

    // Synchronous; stops the streaming threads, so no callback runs afterwards.
    gst_element_set_state(impl->playbin, GST_STATE_NULL);
    gst_object_unref(impl->playbin);
    impl->playbin = nullptr;
    impl->segment = SegmentState::None;
    impl->seekSeq = 0;
}

static void _finishPlayback(GstImpl* impl)
{
    gst_element_set_state(impl->playbin, GST_STATE_PAUSED);
    impl->segment = SegmentState::None;
    impl->loader->paused = true;

    tvg::ScopedLock lock(impl->key);
    impl->latestFrameTime = impl->loader->totalTime;
}

static bool _seek(GstImpl* impl, gint64 position, bool flush)
{
    auto flags = static_cast<uint32_t>(GST_SEEK_FLAG_ACCURATE);
    if (flush) flags |= GST_SEEK_FLAG_FLUSH;

    // Segment seeks finish with SEGMENT_DONE so _pollBus() can restart the loop.
    if (impl->loader->looping) flags |= GST_SEEK_FLAG_SEGMENT;

    auto event = gst_event_new_seek(1.0, GST_FORMAT_TIME, static_cast<GstSeekFlags>(flags),
                                    GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE);

    // send_event() takes ownership; preserve the seqnum to reject earlier completions.
    auto seq = gst_event_get_seqnum(event);
    if (!gst_element_send_event(impl->playbin, event)) return false;

    impl->seekSeq = seq;
    impl->segment = impl->loader->looping ? SegmentState::Active : SegmentState::None;
    return true;
}

static bool _seekCurrent(GstImpl* impl)
{
    // Flush-seek to apply the current loop mode without changing playback position.
    gint64 position = 0;
    return gst_element_query_position(impl->playbin, GST_FORMAT_TIME, &position) &&
           GST_CLOCK_TIME_IS_VALID(position) && position >= 0 && _seek(impl, position, true);
}

static bool _pollBus(GstImpl* impl)
{
    if (!impl->playbin) return false;

    auto bus = gst_element_get_bus(impl->playbin);
    if (!bus) return true;

    while (auto msg = gst_bus_pop(bus)) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_SEGMENT_DONE: {
                // Ignore a SEGMENT_DONE from an earlier seek.
                if (gst_message_get_seqnum(msg) != impl->seekSeq) break;
                if (impl->loader->paused) {
                    impl->segment = SegmentState::Pending;
                    break;
                }
                if (impl->loader->looping) {
                    if (!_seek(impl, 0, false)) _finishPlayback(impl);
                    else impl->segment = SegmentState::Pending;
                } else {
                    GstFormat format;
                    gint64 position = 0;
                    gst_message_parse_segment_done(msg, &format, &position);

                    // Let queued frames drain before the ordinary EOS stops playback.
                    if (format != GST_FORMAT_TIME || !_seek(impl, position, false)) {
                        _finishPlayback(impl);
                    }
                }
                break;
            }
            case GST_MESSAGE_EOS: {
                // EOS may still arrive if looping was enabled after ordinary playback started.
                if (!impl->loader->looping || !_seek(impl, 0, true)) _finishPlayback(impl);
                break;
            }
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gst_message_parse_error(msg, &err, nullptr);
                TVGERR("GST", "Pipeline error: %s", err ? err->message : "Unknown error");
                if (err) g_error_free(err);
                _close(impl);
                impl->loader->paused = true;
                break;
            }
            case GST_MESSAGE_DURATION_CHANGED: {
                gint64 duration = 0;
                if (gst_element_query_duration(impl->playbin, GST_FORMAT_TIME, &duration) &&
                    GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
                    impl->loader->totalTime = _nanoToSec(duration);
                }
                break;
            }
            default: break;
        }
        gst_message_unref(msg);
        if (!impl->playbin) break;
    }

    gst_object_unref(bus);
    return impl->playbin != nullptr;
}

/************************************************************************/
/* External Class Implementation                                        */
/************************************************************************/

GstMediaLoader::GstMediaLoader() :
    MediaLoader(FileType::Media),
    pImpl(new GstImpl)
{
    pImpl->loader = this;
}

GstMediaLoader::~GstMediaLoader()
{
    _close(pImpl);
    for (auto data : pImpl->frames) tvg::free(data);
    tvg::free(surface.data);
    delete pImpl;
}

bool GstMediaLoader::open(const char* path, TVG_UNUSED const LoaderOps* ops)
{
    if (!path) return false;

    // Safe to call per open(); omit gst_deinit() because GStreamer cannot be used afterwards.
    gst_init(nullptr, nullptr);

    auto playbin = gst_element_factory_make("playbin", nullptr);
    auto appsink = GST_APP_SINK(gst_element_factory_make("appsink", nullptr));
    auto uri = gst_filename_to_uri(path, nullptr);

    if (!playbin || !appsink || !uri) {
        TVGLOG("GST", "Missing playbin/appsink element or invalid path: %s", path);
        if (playbin) gst_object_unref(playbin);
        if (appsink) gst_object_unref(appsink);
        g_free(uri);
        return false;
    }

    g_signal_connect(playbin, "element-setup", G_CALLBACK(_setupAudio), nullptr);

    // Request video/x-raw frames in system memory and ColorSpace::ARGB8888S byte order.
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    constexpr auto format = "BGRA";
#else
    constexpr auto format = "ARGB";
#endif
    auto caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, format, nullptr);
    gst_app_sink_set_caps(appsink, caps);
    gst_caps_unref(caps);

    // Drop older queued frames instead of blocking the streaming thread.
    g_object_set(appsink, "max-buffers", 1u, "drop", TRUE, nullptr);

    GstAppSinkCallbacks callbacks = {};
    callbacks.new_preroll = _onPreroll;
    callbacks.new_sample = _onSample;
    gst_app_sink_set_callbacks(appsink, &callbacks, pImpl, nullptr);

    g_object_set(playbin, "uri", uri, "video-sink", appsink, nullptr);
    g_free(uri);

    // Use the system clock so clock queries cannot block on a stalled audio sink.
    auto clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(playbin), clock);
    gst_object_unref(clock);

    pImpl->playbin = playbin;

    // Bound preroll so a stalled sink fails open() instead of blocking indefinitely.
    if (gst_element_set_state(playbin, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
        gst_element_get_state(playbin, nullptr, nullptr, PREROLL_TIMEOUT) != GST_STATE_CHANGE_SUCCESS) {
        TVGLOG("GST", "Failed to open media: %s", path);
        _close(pImpl);
        return false;
    }

    gint64 duration = 0;
    if (!gst_element_query_duration(playbin, GST_FORMAT_TIME, &duration) || duration <= 0) {
        TVGLOG("GST", "Invalid media duration: %s", path);
        _close(pImpl);
        return false;
    }

    GstVideoInfo info;
    auto pad = gst_element_get_static_pad(GST_ELEMENT(appsink), "sink");
    auto prerolled = pad ? gst_pad_get_current_caps(pad) : nullptr;
    auto valid = prerolled && gst_video_info_from_caps(&info, prerolled);
    if (prerolled) gst_caps_unref(prerolled);
    if (pad) gst_object_unref(pad);

    if (!valid || info.width <= 0 || info.height <= 0) {
        TVGLOG("GST", "No video track found: %s", path);
        _close(pImpl);
        return false;
    }

    w = static_cast<float>(info.width);
    h = static_cast<float>(info.height);
    totalTime = _nanoToSec(duration);
    curTime = 0.0f;

    return true;
}

bool GstMediaLoader::read()
{
    if (!Loader::read()) return true;
    if (!pImpl->playbin || w == 0 || h == 0) return false;

    surface.cs = ColorSpace::ARGB8888S;
    surface.w = static_cast<uint32_t>(w);
    surface.h = static_cast<uint32_t>(h);
    surface.stride = surface.w;
    surface.channelSize = sizeof(uint32_t);
    surface.premultiplied = false;
    surface.alphaIgnored = true;

    g_object_set(pImpl->playbin, "volume", static_cast<double>(audioVolume), "mute", static_cast<gboolean>(muted), nullptr);
    paused = true;
    return sync();
}

bool GstMediaLoader::sync()
{
    if (!_pollBus(pImpl)) return false;

    gint64 position = 0;
    auto queried = gst_element_query_position(pImpl->playbin, GST_FORMAT_TIME, &position);
    auto validPosition = queried && GST_CLOCK_TIME_IS_VALID(position) && position >= 0;

    {
        tvg::ScopedLock lock(pImpl->key);

        curTime = validPosition ? _nanoToSec(position) : pImpl->latestFrameTime;
        if (!pImpl->frameUpdated) return surface.data && sharing > 0;

        auto frame = pImpl->frames[pImpl->latest];
        if (!frame) return false;

        auto size = surface.stride * surface.h * surface.channelSize;
        if (!surface.data) surface.data = tvg::malloc<pixel_t>(size);
        if (!surface.data) return false;

        memcpy(surface.data, frame, size);
        surface.cs = ColorSpace::ARGB8888S;   // rasterConvertCS() can update this.
        surface.premultiplied = false;        // rasterPremultiply() can update this.
        pImpl->frameUpdated = false;
    }

    return true;
}

Result GstMediaLoader::play()
{
    if (!pImpl->playbin) return Result::InsufficientCondition;

    // Restart ended playback or prepare gapless looping when possible.
    if (curTime >= totalTime) {
        if (!_seek(pImpl, 0, true)) return Result::Unknown;
    } else if (paused && pImpl->segment == SegmentState::Pending) {
        if (!_seekCurrent(pImpl)) return Result::Unknown;
    } else if (looping && pImpl->segment == SegmentState::None) {
        _seekCurrent(pImpl);
    }

    if (!_state(pImpl, GST_STATE_PLAYING)) return Result::Unknown;
    paused = false;
    return Result::Success;
}

Result GstMediaLoader::pause()
{
    if (!pImpl->playbin) return Result::InsufficientCondition;

    if (!_state(pImpl, GST_STATE_PAUSED)) return Result::Unknown;
    paused = true;
    return Result::Success;
}

Result GstMediaLoader::stop()
{
    if (!pImpl->playbin) return Result::InsufficientCondition;

    if (!_state(pImpl, GST_STATE_PAUSED)) return Result::Unknown;
    paused = true;

    // The flush seek prerolls again: the still at 0 arrives via the preroll callback.
    if (!_seek(pImpl, 0, true)) return Result::Unknown;
    curTime = 0.0f;
    return Result::Success;
}

Result GstMediaLoader::seek(float seconds)
{
    if (seconds < 0.0f || seconds > totalTime) return Result::InvalidArguments;
    if (!pImpl->playbin) return Result::InsufficientCondition;

    if (!_seek(pImpl, static_cast<gint64>(static_cast<double>(seconds) * GST_SECOND), true)) return Result::Unknown;
    curTime = seconds;
    return Result::Success;
}

Result GstMediaLoader::loop(bool on)
{
    if (!pImpl->playbin) return Result::InsufficientCondition;

    if (looping == on) return Result::Success;
    looping = on;
    if (on || pImpl->segment != SegmentState::Pending) return Result::Success;

    // Continue from the current position without segment looping.
    if (!_seekCurrent(pImpl)) {
        looping = true;
        return Result::Unknown;
    }
    return Result::Success;
}

Result GstMediaLoader::volume(float volume)
{
    if (!pImpl->playbin) return Result::InsufficientCondition;

    audioVolume = volume;
    g_object_set(pImpl->playbin, "volume", static_cast<double>(volume), nullptr);
    return Result::Success;
}

Result GstMediaLoader::mute(bool on)
{
    if (!pImpl->playbin) return Result::InsufficientCondition;

    muted = on;
    g_object_set(pImpl->playbin, "mute", static_cast<gboolean>(on), nullptr);
    return Result::Success;
}
