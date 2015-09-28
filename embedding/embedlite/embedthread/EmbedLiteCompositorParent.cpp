/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EmbedLog.h"

#include "EmbedLiteCompositorParent.h"
#include "BasicLayers.h"
#include "EmbedLiteViewThreadParent.h"
#include "EmbedLiteApp.h"
#include "EmbedLiteView.h"
#include "mozilla/layers/LayerManagerComposite.h"
#include "mozilla/layers/AsyncCompositionManager.h"
#include "mozilla/layers/LayerTransactionParent.h"
#include "mozilla/layers/CompositorOGL.h"
#include "gfxUtils.h"

#include "math.h"

#include "GLContext.h"                  // for GLContext
#include "GLScreenBuffer.h"             // for GLScreenBuffer
#include "SharedSurfaceEGL.h"           // for SurfaceFactory_EGLImage
#include "SharedSurfaceGL.h"            // for SurfaceFactory_GLTexture, etc
#include "SurfaceTypes.h"               // for SurfaceStreamType
#include "ClientLayerManager.h"         // for ClientLayerManager, etc

using namespace mozilla::layers;
using namespace mozilla::gfx;
using namespace mozilla::gl;

namespace mozilla {
namespace embedlite {

EmbedLiteCompositorParent::EmbedLiteCompositorParent(nsIWidget* aWidget,
                                                     bool aRenderToEGLSurface,
                                                     int aSurfaceWidth,
                                                     int aSurfaceHeight,
                                                     uint32_t id)
  : CompositorParent(aWidget, aRenderToEGLSurface, aSurfaceWidth, aSurfaceHeight)
  , mId(id)
  , mCurrentCompositeTask(nullptr)
  , mLastViewSize(aSurfaceWidth, aSurfaceHeight)
  , mInitialPaintCount(0)
{
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  LOGT("this:%p, view:%p", this, view);
  MOZ_ASSERT(view, "Something went wrong, Compositor not suspended on destroy?");
  EmbedLiteViewThreadParent* pview = static_cast<EmbedLiteViewThreadParent*>(view->GetImpl());
  pview->SetCompositor(this);
}

EmbedLiteCompositorParent::~EmbedLiteCompositorParent()
{
  LOGT();
  EmbedLiteApp::GetInstance()->ViewDestroyed(mId);
}

PLayerTransactionParent*
EmbedLiteCompositorParent::AllocPLayerTransactionParent(const nsTArray<LayersBackend>& aBackendHints,
                                                        const uint64_t& aId,
                                                        TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                                        bool* aSuccess)
{
  PLayerTransactionParent* p =
    CompositorParent::AllocPLayerTransactionParent(aBackendHints,
                                                   aId,
                                                   aTextureFactoryIdentifier,
                                                   aSuccess);

  // Prepare Offscreen rendering context
  PrepareOffscreen();
  return p;
}

void
EmbedLiteCompositorParent::PrepareOffscreen()
{
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  EmbedLiteViewListener* listener = view ? view->GetListener() : nullptr;
  if (listener) {
    listener->CompositorCreated();
  }

  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(RootLayerTreeId());
  NS_ENSURE_TRUE(state && state->mLayerManager, );

  GLContext* context = static_cast<CompositorOGL*>(state->mLayerManager->GetCompositor())->gl();
  NS_ENSURE_TRUE(context, );

  if (context->IsOffscreen()) {
    GLScreenBuffer* screen = context->Screen();
    if (screen) {
      SurfaceStreamType streamType =
        SurfaceStream::ChooseGLStreamType(SurfaceStream::OffMainThread,
                                          screen->PreserveBuffer());
      SurfaceFactory_GL* factory = nullptr;
      if (context->GetContextType() == GLContextType::EGL && sEGLLibrary.HasKHRImageTexture2D()) {
        // [Basic/OGL Layers, OMTC] WebGL layer init.
        factory = SurfaceFactory_EGLImage::Create(context, screen->Caps());
      } else {
        // [Basic Layers, OMTC] WebGL layer init.
        // Well, this *should* work...
        factory = new SurfaceFactory_GLTexture(context, nullptr, screen->Caps());
      }
      if (factory) {
        screen->Morph(factory, streamType);
      }
    }
  }
}

void
EmbedLiteCompositorParent::UpdateTransformState()
{
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(RootLayerTreeId());
  NS_ENSURE_TRUE(state && state->mLayerManager, );


  CompositorOGL *compositor = static_cast<CompositorOGL*>(state->mLayerManager->GetCompositor());
  NS_ENSURE_TRUE(compositor, );

  GLContext* context = compositor->gl();
  NS_ENSURE_TRUE(context, );

  if (context->IsOffscreen() && context->OffscreenSize() != mLastViewSize) {
    context->ResizeOffscreen(mLastViewSize);
    ScheduleRenderOnCompositorThread();
  }
}

void
EmbedLiteCompositorParent::ScheduleTask(CancelableTask* task, int time)
{
  if (Invalidate()) {
    task->Cancel();
    CancelCurrentCompositeTask();
  } else {
    CompositorParent::ScheduleTask(task, time);
  }
}

bool
EmbedLiteCompositorParent::Invalidate()
{
  LOGF();
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  if (!view) {
    LOGE("view not available.. forgot SuspendComposition call?");
    return true;
  }

  UpdateTransformState();

  if (view->GetListener() && !view->GetListener()->Invalidate()) {
    mCurrentCompositeTask = NewRunnableMethod(this, &EmbedLiteCompositorParent::RenderGL);
    MessageLoop::current()->PostDelayedTask(FROM_HERE, mCurrentCompositeTask, 16);
    return true;
  }

  return false;
}

bool EmbedLiteCompositorParent::RenderToContext(gfx::DrawTarget* aTarget)
{
  LOGF();
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(RootLayerTreeId());

  NS_ENSURE_TRUE(state->mLayerManager, false);
  if (!state->mLayerManager->GetRoot()) {
    // Nothing to paint yet, just return silently
    return false;
  }
  CompositeToTarget(aTarget);
  return true;
}

bool EmbedLiteCompositorParent::RenderGL()
{
  if (mCurrentCompositeTask) {
    mCurrentCompositeTask->Cancel();
    mCurrentCompositeTask = nullptr;
  }

  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(RootLayerTreeId());
  NS_ENSURE_TRUE(state && state->mLayerManager, false);

  GLContext* context = static_cast<CompositorOGL*>(state->mLayerManager->GetCompositor())->gl();
  NS_ENSURE_TRUE(context, false);
  if (!context->IsCurrent()) {
    context->MakeCurrent(true);
  }
  NS_ENSURE_TRUE(context->IsCurrent(), false);

  {
    ScopedScissorRect autoScissor(context);
    GLenum oldTexUnit;
    context->GetUIntegerv(LOCAL_GL_ACTIVE_TEXTURE, &oldTexUnit);
    CompositeToTarget(nullptr);
    context->fActiveTexture(oldTexUnit);
  }

  if (context->IsOffscreen()) {
    GLScreenBuffer* screen = context->Screen();
    MOZ_ASSERT(screen);
    if (screen->Size().IsEmpty() || !screen->PublishFrame(screen->Size())) {
      NS_ERROR("Failed to publish context frame");
      return false;
    }
    // Temporary hack, we need two extra paints in order to get initial picture
    if (mInitialPaintCount < 2) {
      ScheduleRenderOnCompositorThread();
      mInitialPaintCount++;
    }
  }

  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  if (view) {
    view->GetListener()->CompositingFinished();
  }

  return false;
}

void EmbedLiteCompositorParent::SetSurfaceSize(int width, int height)
{
  mLastViewSize.SizeTo(width, height);
  SetEGLSurfaceSize(width, height);
}

void* EmbedLiteCompositorParent::GetPlatformImage(int* width, int* height)
{
  const CompositorParent::LayerTreeState* state = CompositorParent::GetIndirectShadowTree(RootLayerTreeId());
  NS_ENSURE_TRUE(state && state->mLayerManager, nullptr);

  GLContext* context = static_cast<CompositorOGL*>(state->mLayerManager->GetCompositor())->gl();
  NS_ENSURE_TRUE(context && context->IsOffscreen(), nullptr);

  SharedSurface* sharedSurf = context->RequestFrame();
  NS_ENSURE_TRUE(sharedSurf, nullptr);

  *width = sharedSurf->Size().width;
  *height = sharedSurf->Size().height;

  if (sharedSurf->Type() == SharedSurfaceType::EGLImageShare) {
    SharedSurface_EGLImage* eglImageSurf = SharedSurface_EGLImage::Cast(sharedSurf);
    return eglImageSurf->mImage;
  }

  return nullptr;
}

void
EmbedLiteCompositorParent::SuspendRendering()
{
  CompositorParent::SchedulePauseOnCompositorThread();
}

void
EmbedLiteCompositorParent::ResumeRendering()
{
  CompositorParent::ScheduleResumeOnCompositorThread(mLastViewSize.width, mLastViewSize.height);
}

void mozilla::embedlite::EmbedLiteCompositorParent::DrawWindowUnderlay(LayerManagerComposite *aManager, nsIntRect aRect)
{
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  if (view) {
    view->GetListener()->DrawUnderlay();
  }
}

void EmbedLiteCompositorParent::DrawWindowOverlay(LayerManagerComposite *aManager, nsIntRect aRect)
{
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  if (view) {
    view->GetListener()->DrawOverlay(aRect);
  }
}

bool EmbedLiteCompositorParent::PreRender(LayerManagerComposite* aManager)
{
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
  if (view) {
    return view->GetListener()->PreRender();
  }
  return true;
}

void EmbedLiteCompositorParent::ClearCompositorSurface(nscolor aColor)
{
  CancelCurrentCompositeTask();
  const CompositorParent::LayerTreeState* state = GetIndirectShadowTree(RootLayerTreeId());
  MOZ_ASSERT(state && state->mLayerManager);
  GLContext* context = static_cast<CompositorOGL*>(state->mLayerManager->GetCompositor())->gl();
  if (context) {
    CompositorLoop()->PostTask(FROM_HERE, NewRunnableFunction(&ClearCompositorSurfaceImpl, context, aColor));
  } else {
    NS_WARNING("Trying to clear content of compositor without GL context!");
  }
}

void EmbedLiteCompositorParent::ClearCompositorSurfaceImpl(GLContext* aContext, nscolor aColor)
{
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  if (aContext->MakeCurrent()) {
    LOGT("color: %x", aColor);
    float r = NS_GET_R(aColor) / 255;
    float g = NS_GET_G(aColor) / 255;
    float b = NS_GET_B(aColor) / 255;
    float a = NS_GET_A(aColor) / 255;
    aContext->fClearColor(r, g, b, a);
    aContext->fClear(LOCAL_GL_COLOR_BUFFER_BIT | LOCAL_GL_DEPTH_BUFFER_BIT);
    aContext->SwapBuffers();
  }
}

} // namespace embedlite
} // namespace mozilla

