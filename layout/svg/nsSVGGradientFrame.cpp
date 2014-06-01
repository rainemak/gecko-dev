/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Main header first:
#include "nsSVGGradientFrame.h"
#include <algorithm>

// Keep others in (case-insensitive) order:
#include "gfxPattern.h"
#include "mozilla/dom/SVGGradientElement.h"
#include "mozilla/dom/SVGStopElement.h"
#include "nsContentUtils.h"
#include "nsSVGEffects.h"
#include "nsSVGAnimatedTransformList.h"
#include "gfxColor.h"

// XXX Tight coupling with content classes ahead!

using namespace mozilla;
using namespace mozilla::dom;

//----------------------------------------------------------------------
// Helper classes

class MOZ_STACK_CLASS nsSVGGradientFrame::AutoGradientReferencer
{
public:
  AutoGradientReferencer(nsSVGGradientFrame *aFrame
                         MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
    : mFrame(aFrame)
  {
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    // Reference loops should normally be detected in advance and handled, so
    // we're not expecting to encounter them here
    NS_ABORT_IF_FALSE(!mFrame->mLoopFlag, "Undetected reference loop!");
    mFrame->mLoopFlag = true;
  }
  ~AutoGradientReferencer() {
    mFrame->mLoopFlag = false;
  }
private:
  nsSVGGradientFrame *mFrame;
  MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

//----------------------------------------------------------------------
// Implementation

nsSVGGradientFrame::nsSVGGradientFrame(nsStyleContext* aContext) :
  nsSVGGradientFrameBase(aContext),
  mLoopFlag(false),
  mNoHRefURI(false)
{
}

NS_IMPL_FRAMEARENA_HELPERS(nsSVGGradientFrame)

//----------------------------------------------------------------------
// nsIFrame methods:

nsresult
nsSVGGradientFrame::AttributeChanged(int32_t         aNameSpaceID,
                                     nsIAtom*        aAttribute,
                                     int32_t         aModType)
{
  if (aNameSpaceID == kNameSpaceID_None &&
      (aAttribute == nsGkAtoms::gradientUnits ||
       aAttribute == nsGkAtoms::gradientTransform ||
       aAttribute == nsGkAtoms::spreadMethod)) {
    nsSVGEffects::InvalidateDirectRenderingObservers(this);
  } else if (aNameSpaceID == kNameSpaceID_XLink &&
             aAttribute == nsGkAtoms::href) {
    // Blow away our reference, if any
    Properties().Delete(nsSVGEffects::HrefProperty());
    mNoHRefURI = false;
    // And update whoever references us
    nsSVGEffects::InvalidateDirectRenderingObservers(this);
  }

  return nsSVGGradientFrameBase::AttributeChanged(aNameSpaceID,
                                                  aAttribute, aModType);
}

//----------------------------------------------------------------------

uint16_t
nsSVGGradientFrame::GetEnumValue(uint32_t aIndex, nsIContent *aDefault)
{
  const nsSVGEnum& thisEnum =
    static_cast<dom::SVGGradientElement*>(mContent)->mEnumAttributes[aIndex];

  if (thisEnum.IsExplicitlySet())
    return thisEnum.GetAnimValue();

  AutoGradientReferencer gradientRef(this);

  nsSVGGradientFrame *next = GetReferencedGradientIfNotInUse();
  return next ? next->GetEnumValue(aIndex, aDefault) :
    static_cast<dom::SVGGradientElement*>(aDefault)->
      mEnumAttributes[aIndex].GetAnimValue();
}

uint16_t
nsSVGGradientFrame::GetGradientUnits()
{
  // This getter is called every time the others are called - maybe cache it?
  return GetEnumValue(dom::SVGGradientElement::GRADIENTUNITS);
}

uint16_t
nsSVGGradientFrame::GetSpreadMethod()
{
  return GetEnumValue(dom::SVGGradientElement::SPREADMETHOD);
}

const nsSVGAnimatedTransformList*
nsSVGGradientFrame::GetGradientTransformList(nsIContent* aDefault)
{
  nsSVGAnimatedTransformList *thisTransformList =
    static_cast<dom::SVGGradientElement*>(mContent)->GetAnimatedTransformList();

  if (thisTransformList && thisTransformList->IsExplicitlySet())
    return thisTransformList;

  AutoGradientReferencer gradientRef(this);

  nsSVGGradientFrame *next = GetReferencedGradientIfNotInUse();
  return next ? next->GetGradientTransformList(aDefault) :
    static_cast<const dom::SVGGradientElement*>(aDefault)
      ->mGradientTransform.get();
}

gfxMatrix
nsSVGGradientFrame::GetGradientTransform(nsIFrame *aSource,
                                         const gfxRect *aOverrideBounds)
{
  gfxMatrix bboxMatrix;

  uint16_t gradientUnits = GetGradientUnits();
  if (gradientUnits != SVG_UNIT_TYPE_USERSPACEONUSE) {
    NS_ASSERTION(gradientUnits == SVG_UNIT_TYPE_OBJECTBOUNDINGBOX,
                 "Unknown gradientUnits type");
    // objectBoundingBox is the default anyway

    gfxRect bbox =
      aOverrideBounds ? *aOverrideBounds : nsSVGUtils::GetBBox(aSource);
    bboxMatrix =
      gfxMatrix(bbox.Width(), 0, 0, bbox.Height(), bbox.X(), bbox.Y());
  }

  const nsSVGAnimatedTransformList* animTransformList =
    GetGradientTransformList(mContent);
  if (!animTransformList)
    return bboxMatrix;

  gfxMatrix gradientTransform =
    animTransformList->GetAnimValue().GetConsolidationMatrix();
  return bboxMatrix.PreMultiply(gradientTransform);
}

dom::SVGLinearGradientElement*
nsSVGGradientFrame::GetLinearGradientWithLength(uint32_t aIndex,
  dom::SVGLinearGradientElement* aDefault)
{
  // If this was a linear gradient with the required length, we would have
  // already found it in nsSVGLinearGradientFrame::GetLinearGradientWithLength.
  // Since we didn't find the length, continue looking down the chain.

  AutoGradientReferencer gradientRef(this);

  nsSVGGradientFrame *next = GetReferencedGradientIfNotInUse();
  return next ? next->GetLinearGradientWithLength(aIndex, aDefault) : aDefault;
}

dom::SVGRadialGradientElement*
nsSVGGradientFrame::GetRadialGradientWithLength(uint32_t aIndex,
  dom::SVGRadialGradientElement* aDefault)
{
  // If this was a radial gradient with the required length, we would have
  // already found it in nsSVGRadialGradientFrame::GetRadialGradientWithLength.
  // Since we didn't find the length, continue looking down the chain.

  AutoGradientReferencer gradientRef(this);

  nsSVGGradientFrame *next = GetReferencedGradientIfNotInUse();
  return next ? next->GetRadialGradientWithLength(aIndex, aDefault) : aDefault;
}

//----------------------------------------------------------------------
// nsSVGPaintServerFrame methods:

//helper
static void GetStopInformation(nsIFrame* aStopFrame,
                               float *aOffset,
                               nscolor *aStopColor,
                               float *aStopOpacity)
{
  nsIContent* stopContent = aStopFrame->GetContent();
  MOZ_ASSERT(stopContent && stopContent->IsSVG(nsGkAtoms::stop));

  static_cast<SVGStopElement*>(stopContent)->
    GetAnimatedNumberValues(aOffset, nullptr);

  *aOffset = mozilla::clamped(*aOffset, 0.0f, 1.0f);
  *aStopColor = aStopFrame->StyleSVGReset()->mStopColor;
  *aStopOpacity = aStopFrame->StyleSVGReset()->mStopOpacity;
}

already_AddRefed<gfxPattern>
nsSVGGradientFrame::GetPaintServerPattern(nsIFrame *aSource,
                                          const gfxMatrix& aContextMatrix,
                                          nsStyleSVGPaint nsStyleSVG::*aFillOrStroke,
                                          float aGraphicOpacity,
                                          const gfxRect *aOverrideBounds)
{
  uint16_t gradientUnits = GetGradientUnits();
  MOZ_ASSERT(gradientUnits == SVG_UNIT_TYPE_OBJECTBOUNDINGBOX ||
             gradientUnits == SVG_UNIT_TYPE_USERSPACEONUSE);
  if (gradientUnits == SVG_UNIT_TYPE_USERSPACEONUSE) {
    // Set mSource for this consumer.
    // If this gradient is applied to text, our caller will be the glyph, which
    // is not an element, so we need to get the parent
    mSource = aSource->GetContent()->IsNodeOfType(nsINode::eTEXT) ?
                aSource->GetParent() : aSource;
  }

  nsAutoTArray<nsIFrame*,8> stopFrames;
  GetStopFrames(&stopFrames);

  uint32_t nStops = stopFrames.Length();

  // SVG specification says that no stops should be treated like
  // the corresponding fill or stroke had "none" specified.
  if (nStops == 0) {
    nsRefPtr<gfxPattern> pattern = new gfxPattern(gfxRGBA(0, 0, 0, 0));
    return pattern.forget();
  }

  if (nStops == 1 || GradientVectorLengthIsZero()) {
    // The gradient paints a single colour, using the stop-color of the last
    // gradient step if there are more than one.
    float stopOpacity = stopFrames[nStops-1]->StyleSVGReset()->mStopOpacity;
    nscolor stopColor = stopFrames[nStops-1]->StyleSVGReset()->mStopColor;

    nsRefPtr<gfxPattern> pattern = new gfxPattern(
                           gfxRGBA(NS_GET_R(stopColor)/255.0,
                                   NS_GET_G(stopColor)/255.0,
                                   NS_GET_B(stopColor)/255.0,
                                   NS_GET_A(stopColor)/255.0 *
                                     stopOpacity * aGraphicOpacity));
    return pattern.forget();
  }

  // Get the transform list (if there is one). We do this after the returns
  // above since this call can be expensive when "gradientUnits" is set to
  // "objectBoundingBox" (since that requiring a GetBBox() call).
  gfxMatrix patternMatrix = GetGradientTransform(aSource, aOverrideBounds);

  if (patternMatrix.IsSingular()) {
    return nullptr;
  }

  // revert the vector effect transform so that the gradient appears unchanged
  if (aFillOrStroke == &nsStyleSVG::mStroke) {
    patternMatrix.Multiply(nsSVGUtils::GetStrokeTransform(aSource).Invert());
  }

  patternMatrix.Invert();

  nsRefPtr<gfxPattern> gradient = CreateGradient();
  if (!gradient || gradient->CairoStatus())
    return nullptr;

  uint16_t aSpread = GetSpreadMethod();
  if (aSpread == SVG_SPREADMETHOD_PAD)
    gradient->SetExtend(gfxPattern::EXTEND_PAD);
  else if (aSpread == SVG_SPREADMETHOD_REFLECT)
    gradient->SetExtend(gfxPattern::EXTEND_REFLECT);
  else if (aSpread == SVG_SPREADMETHOD_REPEAT)
    gradient->SetExtend(gfxPattern::EXTEND_REPEAT);

  gradient->SetMatrix(patternMatrix);

  // setup stops
  float lastOffset = 0.0f;

  for (uint32_t i = 0; i < nStops; i++) {
    float offset, stopOpacity;
    nscolor stopColor;

    GetStopInformation(stopFrames[i], &offset, &stopColor, &stopOpacity);

    if (offset < lastOffset)
      offset = lastOffset;
    else
      lastOffset = offset;

    gradient->AddColorStop(offset,
                           gfxRGBA(NS_GET_R(stopColor)/255.0,
                                   NS_GET_G(stopColor)/255.0,
                                   NS_GET_B(stopColor)/255.0,
                                   NS_GET_A(stopColor)/255.0 *
                                     stopOpacity * aGraphicOpacity));
  }

  return gradient.forget();
}

// Private (helper) methods

nsSVGGradientFrame *
nsSVGGradientFrame::GetReferencedGradient()
{
  if (mNoHRefURI)
    return nullptr;

  nsSVGPaintingProperty *property = static_cast<nsSVGPaintingProperty*>
    (Properties().Get(nsSVGEffects::HrefProperty()));

  if (!property) {
    // Fetch our gradient element's xlink:href attribute
    dom::SVGGradientElement*grad = static_cast<dom::SVGGradientElement*>(mContent);
    nsAutoString href;
    grad->mStringAttributes[dom::SVGGradientElement::HREF].GetAnimValue(href, grad);
    if (href.IsEmpty()) {
      mNoHRefURI = true;
      return nullptr; // no URL
    }

    // Convert href to an nsIURI
    nsCOMPtr<nsIURI> targetURI;
    nsCOMPtr<nsIURI> base = mContent->GetBaseURI();
    nsContentUtils::NewURIWithDocumentCharset(getter_AddRefs(targetURI), href,
                                              mContent->GetCurrentDoc(), base);

    property =
      nsSVGEffects::GetPaintingProperty(targetURI, this, nsSVGEffects::HrefProperty());
    if (!property)
      return nullptr;
  }

  nsIFrame *result = property->GetReferencedFrame();
  if (!result)
    return nullptr;

  nsIAtom* frameType = result->GetType();
  if (frameType != nsGkAtoms::svgLinearGradientFrame &&
      frameType != nsGkAtoms::svgRadialGradientFrame)
    return nullptr;

  return static_cast<nsSVGGradientFrame*>(result);
}

nsSVGGradientFrame *
nsSVGGradientFrame::GetReferencedGradientIfNotInUse()
{
  nsSVGGradientFrame *referenced = GetReferencedGradient();
  if (!referenced)
    return nullptr;

  if (referenced->mLoopFlag) {
    // XXXjwatt: we should really send an error to the JavaScript Console here:
    NS_WARNING("gradient reference loop detected while inheriting attribute!");
    return nullptr;
  }

  return referenced;
}

void
nsSVGGradientFrame::GetStopFrames(nsTArray<nsIFrame*>* aStopFrames)
{
  nsIFrame *stopFrame = nullptr;
  for (stopFrame = mFrames.FirstChild(); stopFrame;
       stopFrame = stopFrame->GetNextSibling()) {
    if (stopFrame->GetType() == nsGkAtoms::svgStopFrame) {
      aStopFrames->AppendElement(stopFrame);
    }
  }
  if (aStopFrames->Length() > 0) {
    return;
  }

  // Our gradient element doesn't have stops - try to "inherit" them

  AutoGradientReferencer gradientRef(this);
  nsSVGGradientFrame* next = GetReferencedGradientIfNotInUse();
  if (!next) {
    return;
  }

  return next->GetStopFrames(aStopFrames);
}

// -------------------------------------------------------------------------
// Linear Gradients
// -------------------------------------------------------------------------

#ifdef DEBUG
void
nsSVGLinearGradientFrame::Init(nsIContent*       aContent,
                               nsContainerFrame* aParent,
                               nsIFrame*         aPrevInFlow)
{
  NS_ASSERTION(aContent->IsSVG(nsGkAtoms::linearGradient),
               "Content is not an SVG linearGradient");

  nsSVGLinearGradientFrameBase::Init(aContent, aParent, aPrevInFlow);
}
#endif /* DEBUG */

nsIAtom*
nsSVGLinearGradientFrame::GetType() const
{
  return nsGkAtoms::svgLinearGradientFrame;
}

nsresult
nsSVGLinearGradientFrame::AttributeChanged(int32_t         aNameSpaceID,
                                           nsIAtom*        aAttribute,
                                           int32_t         aModType)
{
  if (aNameSpaceID == kNameSpaceID_None &&
      (aAttribute == nsGkAtoms::x1 ||
       aAttribute == nsGkAtoms::y1 ||
       aAttribute == nsGkAtoms::x2 ||
       aAttribute == nsGkAtoms::y2)) {
    nsSVGEffects::InvalidateDirectRenderingObservers(this);
  }

  return nsSVGGradientFrame::AttributeChanged(aNameSpaceID,
                                              aAttribute, aModType);
}

//----------------------------------------------------------------------

float
nsSVGLinearGradientFrame::GetLengthValue(uint32_t aIndex)
{
  dom::SVGLinearGradientElement* lengthElement =
    GetLinearGradientWithLength(aIndex,
      static_cast<dom::SVGLinearGradientElement*>(mContent));
  // We passed in mContent as a fallback, so, assuming mContent is non-null, the
  // return value should also be non-null.
  NS_ABORT_IF_FALSE(lengthElement,
    "Got unexpected null element from GetLinearGradientWithLength");
  const nsSVGLength2 &length = lengthElement->mLengthAttributes[aIndex];

  // Object bounding box units are handled by setting the appropriate
  // transform in GetGradientTransform, but we need to handle user
  // space units as part of the individual Get* routines.  Fixes 323669.

  uint16_t gradientUnits = GetGradientUnits();
  if (gradientUnits == SVG_UNIT_TYPE_USERSPACEONUSE) {
    return nsSVGUtils::UserSpace(mSource, &length);
  }

  NS_ASSERTION(
    gradientUnits == SVG_UNIT_TYPE_OBJECTBOUNDINGBOX,
    "Unknown gradientUnits type");

  return length.GetAnimValue(static_cast<SVGSVGElement*>(nullptr));
}

dom::SVGLinearGradientElement*
nsSVGLinearGradientFrame::GetLinearGradientWithLength(uint32_t aIndex,
  dom::SVGLinearGradientElement* aDefault)
{
  dom::SVGLinearGradientElement* thisElement =
    static_cast<dom::SVGLinearGradientElement*>(mContent);
  const nsSVGLength2 &length = thisElement->mLengthAttributes[aIndex];

  if (length.IsExplicitlySet()) {
    return thisElement;
  }

  return nsSVGLinearGradientFrameBase::GetLinearGradientWithLength(aIndex,
                                                                   aDefault);
}

bool
nsSVGLinearGradientFrame::GradientVectorLengthIsZero()
{
  return GetLengthValue(dom::SVGLinearGradientElement::ATTR_X1) ==
         GetLengthValue(dom::SVGLinearGradientElement::ATTR_X2) &&
         GetLengthValue(dom::SVGLinearGradientElement::ATTR_Y1) ==
         GetLengthValue(dom::SVGLinearGradientElement::ATTR_Y2);
}

already_AddRefed<gfxPattern>
nsSVGLinearGradientFrame::CreateGradient()
{
  float x1, y1, x2, y2;

  x1 = GetLengthValue(dom::SVGLinearGradientElement::ATTR_X1);
  y1 = GetLengthValue(dom::SVGLinearGradientElement::ATTR_Y1);
  x2 = GetLengthValue(dom::SVGLinearGradientElement::ATTR_X2);
  y2 = GetLengthValue(dom::SVGLinearGradientElement::ATTR_Y2);

  nsRefPtr<gfxPattern> pattern = new gfxPattern(x1, y1, x2, y2);
  return pattern.forget();
}

// -------------------------------------------------------------------------
// Radial Gradients
// -------------------------------------------------------------------------

#ifdef DEBUG
void
nsSVGRadialGradientFrame::Init(nsIContent*       aContent,
                               nsContainerFrame* aParent,
                               nsIFrame*         aPrevInFlow)
{
  NS_ASSERTION(aContent->IsSVG(nsGkAtoms::radialGradient),
               "Content is not an SVG radialGradient");

  nsSVGRadialGradientFrameBase::Init(aContent, aParent, aPrevInFlow);
}
#endif /* DEBUG */

nsIAtom*
nsSVGRadialGradientFrame::GetType() const
{
  return nsGkAtoms::svgRadialGradientFrame;
}

nsresult
nsSVGRadialGradientFrame::AttributeChanged(int32_t         aNameSpaceID,
                                           nsIAtom*        aAttribute,
                                           int32_t         aModType)
{
  if (aNameSpaceID == kNameSpaceID_None &&
      (aAttribute == nsGkAtoms::r ||
       aAttribute == nsGkAtoms::cx ||
       aAttribute == nsGkAtoms::cy ||
       aAttribute == nsGkAtoms::fx ||
       aAttribute == nsGkAtoms::fy)) {
    nsSVGEffects::InvalidateDirectRenderingObservers(this);
  }

  return nsSVGGradientFrame::AttributeChanged(aNameSpaceID,
                                              aAttribute, aModType);
}

//----------------------------------------------------------------------

float
nsSVGRadialGradientFrame::GetLengthValue(uint32_t aIndex)
{
  dom::SVGRadialGradientElement* lengthElement =
    GetRadialGradientWithLength(aIndex,
      static_cast<dom::SVGRadialGradientElement*>(mContent));
  // We passed in mContent as a fallback, so, assuming mContent is non-null,
  // the return value should also be non-null.
  NS_ABORT_IF_FALSE(lengthElement,
    "Got unexpected null element from GetRadialGradientWithLength");
  return GetLengthValueFromElement(aIndex, *lengthElement);
}

float
nsSVGRadialGradientFrame::GetLengthValue(uint32_t aIndex, float aDefaultValue)
{
  dom::SVGRadialGradientElement* lengthElement =
    GetRadialGradientWithLength(aIndex, nullptr);

  return lengthElement ? GetLengthValueFromElement(aIndex, *lengthElement)
                       : aDefaultValue;
}

float
nsSVGRadialGradientFrame::GetLengthValueFromElement(uint32_t aIndex,
  dom::SVGRadialGradientElement& aElement)
{
  const nsSVGLength2 &length = aElement.mLengthAttributes[aIndex];

  // Object bounding box units are handled by setting the appropriate
  // transform in GetGradientTransform, but we need to handle user
  // space units as part of the individual Get* routines.  Fixes 323669.

  uint16_t gradientUnits = GetGradientUnits();
  if (gradientUnits == SVG_UNIT_TYPE_USERSPACEONUSE) {
    return nsSVGUtils::UserSpace(mSource, &length);
  }

  NS_ASSERTION(
    gradientUnits == SVG_UNIT_TYPE_OBJECTBOUNDINGBOX,
    "Unknown gradientUnits type");

  return length.GetAnimValue(static_cast<SVGSVGElement*>(nullptr));
}

dom::SVGRadialGradientElement*
nsSVGRadialGradientFrame::GetRadialGradientWithLength(uint32_t aIndex,
  dom::SVGRadialGradientElement* aDefault)
{
  dom::SVGRadialGradientElement* thisElement =
    static_cast<dom::SVGRadialGradientElement*>(mContent);
  const nsSVGLength2 &length = thisElement->mLengthAttributes[aIndex];

  if (length.IsExplicitlySet()) {
    return thisElement;
  }

  return nsSVGRadialGradientFrameBase::GetRadialGradientWithLength(aIndex,
                                                                   aDefault);
}

bool
nsSVGRadialGradientFrame::GradientVectorLengthIsZero()
{
  return GetLengthValue(dom::SVGRadialGradientElement::ATTR_R) == 0;
}

already_AddRefed<gfxPattern>
nsSVGRadialGradientFrame::CreateGradient()
{
  float cx, cy, r, fx, fy;

  cx = GetLengthValue(dom::SVGRadialGradientElement::ATTR_CX);
  cy = GetLengthValue(dom::SVGRadialGradientElement::ATTR_CY);
  r  = GetLengthValue(dom::SVGRadialGradientElement::ATTR_R);
  // If fx or fy are not set, use cx/cy instead
  fx = GetLengthValue(dom::SVGRadialGradientElement::ATTR_FX, cx);
  fy = GetLengthValue(dom::SVGRadialGradientElement::ATTR_FY, cy);

  if (fx != cx || fy != cy) {
    // The focal point (fFx and fFy) must be clamped to be *inside* - not on -
    // the circumference of the gradient or we'll get rendering anomalies. We
    // calculate the distance from the focal point to the gradient center and
    // make sure it is *less* than the gradient radius.
    // 1/128 is the limit of the fractional part of cairo's 24.8 fixed point
    // representation divided by 2 to ensure that we get different cairo
    // fractions
    double dMax = std::max(0.0, r - 1.0/128);
    float dx = fx - cx;
    float dy = fy - cy;
    double d = sqrt((dx * dx) + (dy * dy));
    if (d > dMax) {
      double angle = atan2(dy, dx);
      fx = (float)(dMax * cos(angle)) + cx;
      fy = (float)(dMax * sin(angle)) + cy;
    }
  }

  nsRefPtr<gfxPattern> pattern = new gfxPattern(fx, fy, 0, cx, cy, r);
  return pattern.forget();
}

// -------------------------------------------------------------------------
// Public functions
// -------------------------------------------------------------------------

nsIFrame*
NS_NewSVGLinearGradientFrame(nsIPresShell*   aPresShell,
                             nsStyleContext* aContext)
{
  return new (aPresShell) nsSVGLinearGradientFrame(aContext);
}

NS_IMPL_FRAMEARENA_HELPERS(nsSVGLinearGradientFrame)

nsIFrame*
NS_NewSVGRadialGradientFrame(nsIPresShell*   aPresShell,
                             nsStyleContext* aContext)
{
  return new (aPresShell) nsSVGRadialGradientFrame(aContext);
}

NS_IMPL_FRAMEARENA_HELPERS(nsSVGRadialGradientFrame)
