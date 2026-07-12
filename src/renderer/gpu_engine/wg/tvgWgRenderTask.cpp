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

#include "tvgWgRenderTask.h"
#include <iostream>
#include <limits>

//***********************************************************************
// WgPaintTask
//***********************************************************************

void WgPaintTask::stage(WgCompositor& compositor)
{
    if (renderData->type() == tvg::Type::Shape)
        compositor.requestShape((WgRenderDataShape*)renderData);
    else if (renderData->type() == tvg::Type::Picture)
        compositor.requestImage((WgRenderDataPicture*)renderData);
}

void WgPaintTask::run(WgContext& context, WgCompositor& compositor, WGPUCommandEncoder encoder)
{
    if (renderData->type() == tvg::Type::Shape)
        compositor.renderShape(context, (WgRenderDataShape*)renderData, blendMethod);
    if (renderData->type() == tvg::Type::Picture)
        compositor.renderImage(context, (WgRenderDataPicture*)renderData, blendMethod);
    else assert(true);
}

//***********************************************************************
// WgSolidBatchTask
//***********************************************************************

WgSolidBatchTask::WgSolidBatchTask(WgRenderDataShape* renderData)
{
    assert(eligible(renderData, BlendMethod::Normal));
    viewport = renderData->viewport;
    vertexCount = renderData->meshShape.vbuffer.count;
    indexCount = renderData->meshShape.ibuffer.count;
    shapes.push(renderData);
}


bool WgSolidBatchTask::eligible(const WgRenderDataShape* renderData, BlendMethod blendMethod)
{
    if (!renderData || blendMethod != BlendMethod::Normal) return false;
    if (renderData->renderSettingsShape.skip || renderData->renderSettingsShape.fillType != WgRenderSettingsType::Solid) return false;
    if (!renderData->convex || renderData->viewport.invalid() || !renderData->clips.empty()) return false;
    if (renderData->meshShape.vbuffer.empty() || renderData->meshShape.ibuffer.empty()) return false;
    if (!renderData->renderSettingsStroke.skip && !renderData->meshStrokes.ibuffer.empty()) return false;
    if (renderData->meshShape.vbuffer.count > std::numeric_limits<uint32_t>::max() / sizeof(Point)) return false;
    if (renderData->meshShape.ibuffer.count > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t)) return false;
    return true;
}


bool WgSolidBatchTask::appendSolid(WgRenderDataShape* renderData)
{
    if (closed || !eligible(renderData, BlendMethod::Normal) || !(viewport == renderData->viewport)) return false;

    const uint64_t nextVertexCount = static_cast<uint64_t>(vertexCount) + renderData->meshShape.vbuffer.count;
    const uint64_t nextIndexCount = static_cast<uint64_t>(indexCount) + renderData->meshShape.ibuffer.count;
    if (nextVertexCount > std::numeric_limits<uint32_t>::max() / sizeof(Point)) return false;
    if (nextIndexCount > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t)) return false;

    shapes.push(renderData);
    vertexCount = static_cast<uint32_t>(nextVertexCount);
    indexCount = static_cast<uint32_t>(nextIndexCount);
    return true;
}


void WgSolidBatchTask::stage(WgCompositor& compositor)
{
    if (shapes.count == 1) compositor.requestShape(shapes[0]);
    else compositor.requestSolidBatch(shapes, range);
}


void WgSolidBatchTask::run(WgContext& context, WgCompositor& compositor, WGPUCommandEncoder encoder)
{
    if (shapes.count == 1) compositor.renderShape(context, shapes[0], BlendMethod::Normal);
    else compositor.renderSolidBatch(context, range);
}

//***********************************************************************
// WgSceneTask
//***********************************************************************

void WgSceneTask::stage(WgCompositor& compositor)
{
    ARRAY_FOREACH(task, children) (*task)->stage(compositor);
}

void WgSceneTask::run(WgContext& context, WgCompositor& compositor, WGPUCommandEncoder encoder)
{
    // begin the render pass for the current scene and clear the target content
    compositor.beginRenderPassMS(encoder, renderTarget, true);
    // run all children (scenes and shapes)
    runChildren(context, compositor, encoder);
    // we must to end current render pass for current scene
    compositor.endRenderPass();
    // we must to apply effect for current scene
    if (effect)
        runEffect(context, compositor, encoder);
    // there's no point in continuing if the scene has no destination target (e.g., the root scene)
    if (!renderTargetDst) return;
    // apply scene blending
    if (compose->method == MaskMethod::None) {
        compositor.beginRenderPassMS(encoder, renderTargetDst, false);
        compositor.renderScene(context, renderTarget, compose);
    // apply scene composition (for scenes, that have a handle to mask)
    } else if (renderTargetMsk) {
        compositor.beginRenderPassMS(encoder, renderTargetDst, false);
        compositor.composeScene(context, renderTarget, renderTargetMsk, compose);
    }
}


void WgSceneTask::runChildren(WgContext& context, WgCompositor& compositor, WGPUCommandEncoder encoder)
{
    ARRAY_FOREACH(task, children) {
        WgRenderTask* renderTask = *task;
        // we need to restore current render pass without clear
        compositor.beginRenderPassMS(encoder, renderTarget, false);
        // run children (shape or scene)
        renderTask->run(context, compositor, encoder);
    }
}


void WgSceneTask::closeSolidBatch()
{
    if (!children.empty()) children.last()->closeSolidBatch();
}


void WgSceneTask::runEffect(WgContext& context, WgCompositor& compositor, WGPUCommandEncoder encoder)
{
    assert(effect);
    switch (effect->type) {
        case SceneEffect::GaussianBlur: compositor.gaussianBlur(context, renderTarget, (RenderEffectGaussianBlur*)effect, compose); break;
        case SceneEffect::DropShadow: compositor.dropShadow(context, renderTarget, (RenderEffectDropShadow*)effect, compose); break;
        case SceneEffect::Fill: compositor.fillEffect(context, renderTarget, (RenderEffectFill*)effect, compose); break;
        case SceneEffect::Tint: compositor.tintEffect(context, renderTarget, (RenderEffectTint*)effect, compose); break;
        case SceneEffect::Tritone : compositor.tritoneEffect(context, renderTarget, (RenderEffectTritone*)effect, compose); break;
        default: break;
    }
}
