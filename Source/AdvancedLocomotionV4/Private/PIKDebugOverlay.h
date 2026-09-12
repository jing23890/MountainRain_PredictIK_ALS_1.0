#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"

class UPIKAnimInstance;

/** Lightweight in-game Slate overlay for inspecting the PredictIK runtime. */
class SPIKDebugOverlay final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SPIKDebugOverlay) {}
        SLATE_ARGUMENT(TWeakObjectPtr<UPIKAnimInstance>, AnimInstance)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
    virtual int32 OnPaint(
        const FPaintArgs& Args,
        const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle& InWidgetStyle,
        bool bParentEnabled) const override;
    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
    TWeakObjectPtr<UPIKAnimInstance> AnimInstance;
};
