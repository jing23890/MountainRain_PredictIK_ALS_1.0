#include "PIKDebugOverlay.h"

#include "PIKAnimInstance.h"
#include "GameFramework/Pawn.h"
#include "Styling/CoreStyle.h"
#include "Rendering/DrawElements.h"

namespace PIKDebugOverlay
{
    constexpr float Left = 24.f;
    constexpr float Top = 24.f;
    constexpr float Width = 430.f;
    constexpr float ChartHeight = 96.f;
    constexpr float Gap = 12.f;
    constexpr float Padding = 10.f;

    FSlateFontInfo Font(float Size)
    {
        FSlateFontInfo Result = FCoreStyle::Get().GetFontStyle("NormalFont");
        Result.Size = static_cast<int32>(Size);
        return Result;
    }

    void Text(
        const FGeometry& Geometry,
        FSlateWindowElementList& Elements,
        int32 Layer,
        const FVector2D& Position,
        const FString& Value,
        const FLinearColor& Color,
        float Size = 12.f)
    {
        FSlateDrawElement::MakeText(
            Elements,
            Layer,
            Geometry.ToPaintGeometry(FVector2f(Width,20.f), FSlateLayoutTransform(FVector2f(Position))),
            FText::FromString(Value),
            Font(Size),
            ESlateDrawEffect::None,
            Color);
    }

    void Box(
        const FGeometry& Geometry,
        FSlateWindowElementList& Elements,
        int32 Layer,
        const FVector2D& Position,
        const FVector2D& Size,
        const FLinearColor& Color)
    {
        FSlateDrawElement::MakeBox(
            Elements,
            Layer,
            Geometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(Position))),
            FCoreStyle::Get().GetBrush("WhiteBrush"),
            ESlateDrawEffect::None,
            Color);
    }
}

void SPIKDebugOverlay::Construct(const FArguments& InArgs)
{
    AnimInstance = InArgs._AnimInstance;
    SetCanTick(true);
    SetVisibility(EVisibility::HitTestInvisible);
}

FVector2D SPIKDebugOverlay::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
    return FVector2D(PIKDebugOverlay::Left + PIKDebugOverlay::Width, 474.f);
}

void SPIKDebugOverlay::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
    SLeafWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
    Invalidate(EInvalidateWidgetReason::Paint);
}

int32 SPIKDebugOverlay::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    const UPIKAnimInstance* Anim = AnimInstance.Get();
    if (!Anim)
    {
        return LayerId;
    }

    const FLinearColor Panel(0.015f, 0.02f, 0.03f, 0.88f);
    const FLinearColor PanelBorder(0.12f, 0.18f, 0.24f, 0.95f);
    const FLinearColor White(0.88f, 0.93f, 1.f, 1.f);
    const FLinearColor Muted(0.52f, 0.62f, 0.72f, 1.f);
    const FLinearColor Cyan(0.1f, 0.85f, 1.f, 1.f);
    const FLinearColor Orange(1.f, 0.48f, 0.12f, 1.f);

    const FVector2D PanelSize(PIKDebugOverlay::Width, 450.f);
    PIKDebugOverlay::Box(AllottedGeometry, OutDrawElements, LayerId, FVector2D(PIKDebugOverlay::Left, PIKDebugOverlay::Top), PanelSize, Panel);
    PIKDebugOverlay::Box(AllottedGeometry, OutDrawElements, LayerId + 1, FVector2D(PIKDebugOverlay::Left, PIKDebugOverlay::Top), FVector2D(2.f, PanelSize.Y), PanelBorder);

    const FVector2D Origin(PIKDebugOverlay::Left + PIKDebugOverlay::Padding, PIKDebugOverlay::Top + PIKDebugOverlay::Padding);
    PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 2, Origin,
        FString::Printf(TEXT("PREDICT IK | %s"),*GetNameSafe(Anim->TryGetPawnOwner())), Cyan, 12.f);
    PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 2, Origin + FVector2D(0.f, 22.f),
        FString::Printf(TEXT("Status: %s"), *Anim->PIK_Status), White);
    PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 2, Origin + FVector2D(0.f, 40.f),
        FString::Printf(TEXT("Alpha %.2f    Traces %d    Pelvis %+0.1f cm"), Anim->PIK_Alpha, Anim->PIK_TraceCount, Anim->PIK_GetPelvisOffsetWorldCm()), White);
    PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 2, Origin + FVector2D(0.f, 57.f),
        FString::Printf(TEXT("Pivot target %+0.1f cm    reach cap %+0.1f cm"), Anim->PIK_DebugTargetPelvisOffsetCm, Anim->PIK_DebugReachLimitCm), Muted);

    const auto StateText = [](const FPIKFootState& State)
    {
        if (!State.bInitialized) return FString(TEXT("INIT"));
        if (State.bUsingPrediction) return State.bPlanted ? FString(TEXT("PREDICT->PLANT")) : FString(TEXT("PREDICT"));
        return State.bPlanted ? FString(TEXT("PLANTED")) : FString(TEXT("GROUND"));
    };
    PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 2, Origin + FVector2D(0.f, 78.f),
        FString::Printf(TEXT("L %-14s  path %0.2f  target Z %0.1f"), *StateText(Anim->PIK_FootState_L), Anim->PIK_FootState_L.PathProgress, Anim->PIK_FootState_L.TargetAnkleWS.Z), Cyan);
    PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 2, Origin + FVector2D(0.f, 96.f),
        FString::Printf(TEXT("R %-14s  path %0.2f  target Z %0.1f"), *StateText(Anim->PIK_FootState_R), Anim->PIK_FootState_R.PathProgress, Anim->PIK_FootState_R.TargetAnkleWS.Z), Orange);

    const FVector2D ChartOrigin = Origin + FVector2D(0.f, 121.f);
    const FVector2D ChartSize(PIKDebugOverlay::Width - 2.f * PIKDebugOverlay::Padding, PIKDebugOverlay::ChartHeight);
    const TArray<FPIKDebugSample>& Samples = Anim->PIK_GetDebugSamples();

    auto DrawChart = [&](const FVector2D& At, const FString& Title, float MinValue, float MaxValue,
                         auto ValueA, auto ValueB, const FLinearColor& ColorA, const FLinearColor& ColorB)
    {
        PIKDebugOverlay::Box(AllottedGeometry, OutDrawElements, LayerId + 1, At, ChartSize, FLinearColor(0.025f, 0.04f, 0.06f, 0.95f));
        PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 3, At + FVector2D(6.f, 4.f), Title, Muted, 10.f);
        PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 3, At + FVector2D(ChartSize.X - 120.f, 4.f), TEXT("L"), ColorA, 10.f);
        PIKDebugOverlay::Text(AllottedGeometry, OutDrawElements, LayerId + 3, At + FVector2D(ChartSize.X - 86.f, 4.f), TEXT("R"), ColorB, 10.f);

        for (int32 Grid = 1; Grid < 4; ++Grid)
        {
            const float Y = At.Y + ChartSize.Y * static_cast<float>(Grid) / 4.f;
            TArray<FVector2D> GridLine{ FVector2D(At.X, Y), FVector2D(At.X + ChartSize.X, Y) };
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, AllottedGeometry.ToPaintGeometry(), GridLine, ESlateDrawEffect::None, FLinearColor(0.12f, 0.17f, 0.22f, 0.8f), false, 1.f);
        }

        if (Samples.Num() < 2 || FMath::IsNearlyEqual(MinValue, MaxValue)) return;
        const int32 First = 0;
        TArray<FVector2D> PointsA;
        TArray<FVector2D> PointsB;
        PointsA.Reserve(Samples.Num() - First);
        PointsB.Reserve(Samples.Num() - First);
        const float Range = MaxValue - MinValue;
        for (int32 Index = First; Index < Samples.Num(); ++Index)
        {
            const float DurationSec = FMath::Max(Samples.Last().TimeSec-Samples[First].TimeSec,0.001f);
            const float X = At.X + ChartSize.X * (Samples[Index].TimeSec-Samples[First].TimeSec)/DurationSec;
            const float Value1 = FMath::Clamp((ValueA(Samples[Index]) - MinValue) / Range, 0.f, 1.f);
            const float Value2 = FMath::Clamp((ValueB(Samples[Index]) - MinValue) / Range, 0.f, 1.f);
            PointsA.Add(FVector2D(X, At.Y + ChartSize.Y - Value1 * ChartSize.Y));
            PointsB.Add(FVector2D(X, At.Y + ChartSize.Y - Value2 * ChartSize.Y));
        }
        FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 3, AllottedGeometry.ToPaintGeometry(), PointsA, ESlateDrawEffect::None, ColorA, true, 1.8f);
        FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 3, AllottedGeometry.ToPaintGeometry(), PointsB, ESlateDrawEffect::None, ColorB, true, 1.8f);
    };

    DrawChart(ChartOrigin, TEXT("IK PATH PROGRESS L/R"), 0.f, 1.f,
        [](const FPIKDebugSample& S) { return S.PathProgressL; }, [](const FPIKDebugSample& S) { return S.PathProgressR; }, Cyan, Orange);
    DrawChart(ChartOrigin + FVector2D(0.f, PIKDebugOverlay::ChartHeight + PIKDebugOverlay::Gap), TEXT("ANIMATION CURVES: FOOT HEIGHT"), 0.f, 40.f,
        [](const FPIKDebugSample& S) { return S.FootHeightL; }, [](const FPIKDebugSample& S) { return S.FootHeightR; }, Cyan, Orange);
    DrawChart(ChartOrigin + FVector2D(0.f, 2.f * (PIKDebugOverlay::ChartHeight + PIKDebugOverlay::Gap)), TEXT("ANIMATION CURVES: TIME TO LAND"), 0.f, 1.2f,
        [](const FPIKDebugSample& S) { return S.TimeToLandL; }, [](const FPIKDebugSample& S) { return S.TimeToLandR; }, Cyan, Orange);

    return LayerId + 4;
}
