#include "PIKAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"
#include "PIKDebugOverlay.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PIK
{
    constexpr float ProgressEpsilon = 0.001f;  // 路径推进进度判等的微小容差
    constexpr float PositionEpsilonCm = 0.1f;  // 两点是否“几乎重合”的位置容差（厘米）
    FVector Horizontal(const FVector& V) { return FVector(V.X, V.Y, 0.0); } // 去掉 Z，只保留水平分量
}

void UPIKAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    // 动画实例重新初始化时清空全部 PIK 状态，保证热重载/切实例后从零开始。
    bPIK_ReferencePoseValid = false;
    PIK_DebugSamples.Reset();
    PIK_DebugSampleClock = 0.f;
    PIK_ResetPrediction();
}

void UPIKAnimInstance::BeginDestroy()
{
    PIK_RemoveDebugOverlay();
    Super::BeginDestroy();
}

void UPIKAnimInstance::PIK_UpdateDebugOverlay()
{
    // Slate 控件只能在游戏线程操作。
    if (!IsInGameThread()) return;

    const APawn* Pawn = TryGetPawnOwner();
    UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    // 只给“本地玩家控制”的角色显示屏幕面板（远端/录屏/无玩家时隐藏）。
    const bool bShouldShow = bPIK_DrawScreenDebug && GetWorld() && GetWorld()->IsGameWorld() &&
        Pawn && Pawn->IsPlayerControlled() && Pawn->IsLocallyControlled() && Viewport &&
        GetSkelMeshComponent() && GetSkelMeshComponent()->GetAnimInstance() == this;
    if (!bShouldShow)
    {
        PIK_RemoveDebugOverlay();
        return;
    }

    // 已存在且挂在同一个视口上时直接复用，避免重复创建。
    if (PIK_DebugOverlay.IsValid() && PIK_DebugViewport.Get() == Viewport) return;
    PIK_RemoveDebugOverlay();
    PIK_DebugViewport = Viewport;
    PIK_DebugOverlay = SNew(SPIKDebugOverlay).AnimInstance(this);
    // 层级 1000：叠在绝大多数 UI 之上（控件自身 HitTestInvisible，不挡点击）。
    Viewport->AddViewportWidgetContent(PIK_DebugOverlay.ToSharedRef(), 1000);
}

void UPIKAnimInstance::PIK_RemoveDebugOverlay()
{
    if (!PIK_DebugOverlay.IsValid()) return;
    if (PIK_DebugViewport.IsValid() && IsInGameThread())
    {
        PIK_DebugViewport->RemoveViewportWidgetContent(PIK_DebugOverlay.ToSharedRef());
    }
    PIK_DebugOverlay.Reset();
    PIK_DebugViewport.Reset();
}

void UPIKAnimInstance::NativeUninitializeAnimation()
{
    PIK_RemoveDebugOverlay();
    Super::NativeUninitializeAnimation();
}

void UPIKAnimInstance::PIK_RecordDebugSample(float DeltaSeconds)
{
    // 只在浮层存在（即正在展示曲线图）时记录，屏幕面板一关就停止采样。
    if (!PIK_DebugOverlay.IsValid()) return;
    PIK_DebugSampleClock += FMath::Max(DeltaSeconds, 0.f);

    // 把本帧各项关键数值打成一个采样点，供浮层画曲线用。
    FPIKDebugSample Sample;
    Sample.TimeSec = PIK_DebugSampleClock;
    Sample.Alpha = PIK_Alpha;
    Sample.PelvisOffsetCm = PIK_PelvisOffsetWorldCm;
    Sample.FootHeightL = GetCurveValue(TEXT("FootHeight_L"));
    Sample.FootHeightR = GetCurveValue(TEXT("FootHeight_R"));
    Sample.TimeToLandL = GetCurveValue(TEXT("FootTimeToLand_L"));
    Sample.TimeToLandR = GetCurveValue(TEXT("FootTimeToLand_R"));
    Sample.PathProgressL = PIK_FootState_L.PathProgress;
    Sample.PathProgressR = PIK_FootState_R.PathProgress;
    Sample.TargetHeightL = static_cast<float>(PIK_FootState_L.TargetAnkleWS.Z);
    Sample.TargetHeightR = static_cast<float>(PIK_FootState_R.TargetAnkleWS.Z);
    PIK_DebugSamples.Add(Sample);

    // 超过上限就丢掉最老的采样（近似环形缓冲）。
    const int32 MaxSamples = FMath::Clamp(PIK_DebugHistorySamples, 30, 600);
    if (PIK_DebugSamples.Num() > MaxSamples)
    {
        PIK_DebugSamples.RemoveAt(0, PIK_DebugSamples.Num() - MaxSamples);
    }
}

void UPIKAnimInstance::NativePostEvaluateAnimation()
{
    Super::NativePostEvaluateAnimation();
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    if (!Mesh) return;
    // 动画图评估完成后，缓存“参考脚”及其腿部骨骼的最终 Pose，供下一帧 NativeUpdate 使用。
    // 在 UE5.7 里 post-evaluation 发生在组件 Pose 缓冲翻转之前，
    // 所以这里读的是刚评估好的可编辑缓冲，而不是上一帧的 socket 变换。
    const TArray<FTransform>& Pose = Mesh->GetEditableComponentSpaceTransforms();
    const int32 LeftIndex = Mesh->GetBoneIndex(TEXT("ik_foot_l"));
    const int32 RightIndex = Mesh->GetBoneIndex(TEXT("ik_foot_r"));
    
    bPIK_ReferencePoseValid = Pose.IsValidIndex(LeftIndex) && Pose.IsValidIndex(RightIndex);
    if (!bPIK_ReferencePoseValid) return;
    PIK_ReferencePoseCS[0] = Pose[LeftIndex];
    PIK_ReferencePoseCS[1] = Pose[RightIndex];
    
    for (int32 Side = 0; Side < 2; ++Side)
    {
        // 参考脚转成世界空间，并顺带量出该腿的髋→膝→踝总长（用于骨盆 reach limit）。
        PIK_ReferencePoseWS[Side] = PIK_ReferencePoseCS[Side] * Mesh->GetComponentTransform();
        const int32 Hip = Mesh->GetBoneIndex(Side == 0 ? TEXT("thigh_l") : TEXT("thigh_r"));
        const int32 Knee = Mesh->GetBoneIndex(Side == 0 ? TEXT("calf_l") : TEXT("calf_r"));
        const int32 Ankle = Mesh->GetBoneIndex(Side == 0 ? TEXT("foot_l") : TEXT("foot_r"));
        PIK_LegLengthCS[Side] = 0.f;
        
        if (Pose.IsValidIndex(Hip) && Pose.IsValidIndex(Knee) && Pose.IsValidIndex(Ankle))
        {
            // 髋部缓存前先减掉已施加的骨盆位移，避免把 IK 结果再喂回 reach limit 造成自激。
            PIK_UnshiftedHipCS[Side] = Pose[Hip].GetLocation() - PIK_PelvisOffsetCS * PIK_Alpha;
            PIK_LegLengthCS[Side] = FVector::Distance(Pose[Hip].GetLocation(), Pose[Knee].GetLocation()) +
                FVector::Distance(Pose[Knee].GetLocation(), Pose[Ankle].GetLocation());
            if (bPIK_DrawDebug && GetWorld() && GetWorld()->IsGameWorld())
            {
                // 用这份“已经完成”的 Pose 画腿，而不是 pole target 或上一帧 socket 缓冲。
                const FTransform& MeshWS = Mesh->GetComponentTransform();
                const FVector HipWS = MeshWS.TransformPosition(Pose[Hip].GetLocation());
                const FVector KneeWS = MeshWS.TransformPosition(Pose[Knee].GetLocation());
                const FVector AnkleWS = MeshWS.TransformPosition(Pose[Ankle].GetLocation());
                const FColor Color = Side == 0 ? FColor::Cyan : FColor::Orange;
                DrawDebugLine(GetWorld(), HipWS, KneeWS, Color, false, 0.f, 0, 1.1f);
                DrawDebugLine(GetWorld(), KneeWS, AnkleWS, Color, false, 0.f, 0, 1.1f);
                DrawDebugBox(GetWorld(), KneeWS, FVector(2.5f), Color, false, 0.f);
                const int32 PelvisIndex = Mesh->GetBoneIndex(TEXT("pelvis"));
                if (Side == 0 && Pose.IsValidIndex(PelvisIndex))
                {
                    const FVector PelvisWS = MeshWS.TransformPosition(Pose[PelvisIndex].GetLocation());
                    DrawDebugBox(GetWorld(), PelvisWS, FVector(5.f), FColor::Magenta, false, 0.f);
                    DrawDebugString(GetWorld(), PelvisWS, TEXT("PELVIS BONE"), nullptr, FColor::Magenta, 0.f, true);
                }
            }
        }
    }
}
//这个函数主要就是：动画图刚算完这一帧的骨骼 Pose，趁缓冲还没翻转，把"参考脚/腿"的位置和腿长快照下来，给下一帧的 PIK 解算用。


void UPIKAnimInstance::PIK_GetReferenceFoot(bool bLeft, FVector& OutCS, FVector& OutWS) const
{
    // 优先用上一帧评估后缓存好的参考脚；尚未缓存时退回 socket 查询。
    if (bPIK_ReferencePoseValid)
    {
        const int32 Side = bLeft ? 0 : 1;
        OutCS = PIK_ReferencePoseCS[Side].GetLocation();
        OutWS = PIK_ReferencePoseWS[Side].GetLocation();
        return;
    }
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    OutCS = Mesh->GetSocketTransform(bLeft ? TEXT("ik_foot_l") : TEXT("ik_foot_r"), RTS_Component).GetLocation();
    OutWS = Mesh->GetComponentTransform().TransformPosition(OutCS);
}

void UPIKAnimInstance::PIK_ResetPrediction()
{
    // 把双脚状态机、输出、骨盆枢轴全部回到出厂状态。
    PIK_FootState_L = FPIKFootState();
    PIK_FootState_R = FPIKFootState();
    PIK_Alpha = 0.f;
    PIK_PelvisOffsetWorldCm = 0.f;
    PIK_DebugTargetPelvisOffsetCm = 0.f;
    PIK_DebugReachLimitCm = 0.f;
    PIK_PelvisPivotWorldZCm = 0.0;
    bPIK_PelvisPivotInitialized = false;
    PIK_PelvisOffsetCS = FVector::ZeroVector;
    PIK_Status = TEXT("Waiting for forward walk");
}

float UPIKAnimInstance::PIK_ReadALSFloat(FName Name, float Fallback) const
{
    // 用反射在“派生类（AnimBP）”上按名字读属性。派生类里没有就返回默认值。
    if (const FFloatProperty* P = FindFProperty<FFloatProperty>(GetClass(), Name))
        return P->GetPropertyValue_InContainer(this);
    if (const FDoubleProperty* P = FindFProperty<FDoubleProperty>(GetClass(), Name))
        return static_cast<float>(P->GetPropertyValue_InContainer(this));
    return Fallback;
}
//GetClass()：当前 AnimInstance 的真实类——也就是你的派生类/AnimBP；
//FindFProperty<FFloatProperty>：在类上按名字找某个 UPROPERTY 的属性描述符。只有声明成 UPROPERTY 的变量才会被反射系统登记，普通 C++ 成员找不到；
//找到返回 FFloatProperty*（float 属性的"名片"），找不到返回 nullptr。
//第二段 FDoubleProperty 是兼容 double 类型的属性，读到后 static_cast<float> 转成 float。
//最后找不到就返回你传进来的 Fallback（默认值）。
//父类 UPIKAnimInstance 不想硬编码依赖子类具体有哪些属性，所以用"按名字读"的方式去读派生 AnimBP 上的 ALS 变量：

//比如
//PIK_ReadALSFloat(TEXT("StrideBlend"), 1.f)        // 读“步幅混合”，没有就当 1
//PIK_ReadALSFloat(TEXT("StandingPlayRate"), 1.f)   // 读“播放速率”，没有就当 1

//同理它的兄弟函数 PIK_ReadALSEnum 按名字读枚举（MovementState / Gait 那些）。
//一句话：父类不认得子类里的具体变量，就通过反射"按名字要值"。找到就返回当前值，
//找不到返回兜底值——这就是整套 PIK 和 ALS AnimBP 松耦合的关键（也是之前讲的 fail-closed：读不到就按安全默认处理）。

int64 UPIKAnimInstance::PIK_ReadALSEnum(FName Name, int64 Fallback) const
{
    // 同上，但读的是枚举属性（兼容旧式 FByteProperty 和新式 FEnumProperty）。
    if (const FByteProperty* P = FindFProperty<FByteProperty>(GetClass(), Name))
        return P->GetPropertyValue_InContainer(this);
    if (const FEnumProperty* P = FindFProperty<FEnumProperty>(GetClass(), Name))
        return P->GetUnderlyingProperty()->GetSignedIntPropertyValue(P->ContainerPtrToValuePtr<void>(this));
    return Fallback;
}





//决定这个角色这一帧要不要启用 PIK，以及如果不用，告诉你是为什么。

bool UPIKAnimInstance::PIK_IsGroundPoseEligible(FString& Reason) const
{
    // “总资格”：决定整条 PIK 链是激活还是淡出。任一条件不满足就 fail-closed。
    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    if (!bPIK_Enabled)
    {
        Reason = TEXT("Disabled");
        return false;
    }
    if (!Character || !Mesh || !GetWorld() || !GetWorld()->IsGameWorld())
    {
        Reason = TEXT("Waiting for game character");
        return false;
    }
    if (!Character->GetCharacterMovement()->IsMovingOnGround() || Character->bIsCrouched)
    {
        Reason = TEXT("Excluded: air or crouch");
        return false;
    }
    // ALS_Gait.Walking=0, ALS_MovementState.Grounded=1, ALS_MovementAction.None=0。
    // 属性读不到时按失败处理，宁可不求解也不要在别的 AnimBP 上误触发。
    if (PIK_ReadALSEnum(TEXT("MovementState"), -1) != 1 ||
        PIK_ReadALSEnum(TEXT("MovementAction"), -1) != 0)
    {
        Reason = TEXT("Excluded: movement action or not grounded");
        return false;
    }
    if (!Mesh->DoesSocketExist(TEXT("ik_foot_l")) || !Mesh->DoesSocketExist(TEXT("ik_foot_r")))
    {
        Reason = TEXT("Missing reference IK bones");
        return false;
    }
    Reason = TEXT("Ground contact");
    return true;
}

bool UPIKAnimInstance::PIK_AreFootCurvesValid(bool bLeft) const
{
    // 判定该脚的动画曲线是否落在“合法步态”区间内。
    // 若曲线缺失，GetCurveValue 返回 0，绝不能把 0 当成一次“落地事件”。
    const float Height = GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
    const float Time = GetCurveValue(bLeft ? TEXT("FootTimeToLand_L") : TEXT("FootTimeToLand_R"));
    // 缺失曲线会求值为 0；这里把 0 排除，避免被误当成一次落地事件。
    return FMath::IsFinite(Height) && Height >= 10.f && Height <= 40.f &&
        FMath::IsFinite(Time) && Time >= -0.001f && Time <= 1.2f;
}

bool UPIKAnimInstance::PIK_IsTestPoseEligible(FString& Reason) const
{
    // “预测资格”：是否处于“直线前向匀速行走”，是则允许双脚进入 PREDICT（前向预测）分支。
    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    if (!Character || PIK_ReadALSEnum(TEXT("Gait"), -1) != 0) return false; // Gait==0 即 Walking
    const FVector Velocity = PIK::Horizontal(Character->GetVelocity());
    if (Velocity.Size() < PIK_MinSpeedCmPerSec)
    {
        Reason = TEXT("Excluded: idle");
        return false;
    }
    // 前进方向必须足够接近角色面朝方向（直行），斜向/转向不预测。
    if (FVector::DotProduct(Velocity.GetSafeNormal(), PIK::Horizontal(Character->GetActorForwardVector()).GetSafeNormal()) < PIK_MinForwardDot)
    {
        Reason = TEXT("Excluded: not straight forward");
        return false;
    }
    // 高度通道能区分“准备好的行走姿势”和“曲线缺失”；Time==0 是合法的（正好落地的瞬间）。
    for (FName Curve : { FName(TEXT("FootHeight_L")), FName(TEXT("FootHeight_R")) })
    {
        const float H = GetCurveValue(Curve);
        if (!FMath::IsFinite(H) || H < 10.f || H > 40.f)
        {
            Reason = TEXT("Waiting for prepared walk curves");
            return false;
        }
    }
    Reason = TEXT("Forward walk");
    return true;
}

bool UPIKAnimInstance::PIK_IsWalkable(const FHitResult& Hit) const
{
    // 命中面是否可站立：必须是阻挡、未从内部出发、且法线足够向上。
    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    const float MinZ = Character ? Character->GetCharacterMovement()->GetWalkableFloorZ() : 0.71f;
    return Hit.bBlockingHit && !Hit.bStartPenetrating && Hit.ImpactNormal.Z >= MinZ &&
        !Hit.ImpactPoint.ContainsNaN();
}




//做一次直线探测：从 StartWS 到 EndWS，用 LineTraceSingleByChannel，通道是配置好的 PIK_TraceChannel（默认 ECC_Visibility）。
//忽略自己：TryGetPawnOwner() 作为 IgnoreActor——射线不会打到自己身上。
//计数：每次执行 ++PIK_TraceCount，这个计数最后显示在调试面板上，让你看到"每帧打了多少根射线"。
//命中结果通过 Hit（out 参数）带出去；函数返回有没有打中。
//可选调试画线：开了 bPIK_DrawDebug 时画线——打中前段绿色、命中点一个黄色方块、被挡住的剩余段红色。零生命周期，只显示执行那一帧，方便看每次探测发生了什么
bool UPIKAnimInstance::PIK_TraceSegment(const FVector& StartWS, const FVector& EndWS, FHitResult& Hit)
{
    // 两点之间做一次直线探测（忽略自身）。失败/命中的颜色不同，方便调试。
    if (!GetWorld()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PIKPath), false, TryGetPawnOwner());
    ++PIK_TraceCount;
    const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, StartWS, EndWS, PIK_TraceChannel, Params);
    if (bPIK_DrawDebug)
    {
        // 非持久、零生命周期：只显示执行该射线的那一帧。
        const FVector TraceEndWS = bHit ? Hit.ImpactPoint : EndWS;
        DrawDebugLine(GetWorld(), StartWS, TraceEndWS, bHit ? FColor::Green : FColor::Red, false, 0.f, 0, 0.7f);
        if (bHit)
        {
            // 命中点之后剩余那段（本该透过障碍）画红色，直观看出被挡了多少。
            DrawDebugLine(GetWorld(), Hit.ImpactPoint, EndWS, FColor::Red, false, 0.f, 0, 0.5f);
            DrawDebugBox(GetWorld(), Hit.ImpactPoint, FVector(2.5), FColor::Yellow, false, 0.f, 0, 1.f);
        }
    }
    return bHit;
}

bool UPIKAnimInstance::PIK_TraceGround(const FVector& OriginWS, FHitResult& Hit)
{
    // 从指定点向上抬起一段再向下直落，探测它正下方的可行走地面。
    const FVector StartWS = OriginWS + FVector::UpVector * PIK_TraceUpCm;
    const FVector EndWS = OriginWS - FVector::UpVector * PIK_TraceDownCm;
    return PIK_TraceSegment(StartWS, EndWS, Hit) && PIK_IsWalkable(Hit);
}

float UPIKAnimInstance::PIK_ProjectProgress(const FVector& PointWS, const FVector& StartWS, const FVector& EndWS)
{
    // 把一个世界点投影到“起点→终点”的水平线段上，得到 0..1 的进度。
    const FVector Delta = PIK::Horizontal(EndWS - StartWS);
    const double LengthSq = Delta.SizeSquared();
    if (LengthSq < 0.01) return 0.f;
    return FMath::Clamp(static_cast<float>(FVector::DotProduct(PIK::Horizontal(PointWS - StartWS), Delta) / LengthSq), 0.f, 1.f);
}


//沿着一条已建好的摆动路径，按脚的当前位置把"地面高度"插值出来。它是"分段线性采样"——给定一个水平位置，返回该位置应踩的高度
float UPIKAnimInstance::PIK_SamplePathHeight(const TArray<FVector>& PointsWS, const FVector& PositionWS)
{
    // 沿一条有序路径点做分段线性插值：给定水平位置，取回对应高度。
    if (PointsWS.IsEmpty()) return 0.f;
    if (PointsWS.Num() == 1) return PointsWS[0].Z;
    const FVector& Start = PointsWS[0];
    const FVector& End = PointsWS.Last();
    const float U = PIK_ProjectProgress(PositionWS, Start, End);
    for (int32 Index = 1; Index < PointsWS.Num(); ++Index)
    {
        const float A = PIK_ProjectProgress(PointsWS[Index - 1], Start, End);
        const float B = PIK_ProjectProgress(PointsWS[Index], Start, End);
        if (U <= B || Index == PointsWS.Num() - 1)
        {
            const float T = B - A > PIK::ProgressEpsilon ? FMath::Clamp((U - A) / (B - A), 0.f, 1.f) : 1.f;
            return FMath::Lerp(static_cast<float>(PointsWS[Index - 1].Z), static_cast<float>(PointsWS[Index].Z), T);
        }
    }
    return End.Z;
}


//在"脚离地的起点"和"预测出来的落点"之间，沿真实地形搭出一条脚能安全通过的路径
bool UPIKAnimInstance::PIK_BuildFootPath(const FVector& StartWS, const FVector& EndWS, TArray<FVector>& OutPointsWS)
{
    // 核心算法：在“脚离地点 → 未来落点”之间沿地形搭一条安全的摆动路径（含上台阶绕障）。
    OutPointsWS.Reset(); // 构建失败也绝不暴露旧路径或残缺路径。
    if (StartWS.ContainsNaN() || EndWS.ContainsNaN() || !GetWorld()) return false;
    if (FMath::Abs(EndWS.Z - StartWS.Z) > PIK_MaxTerrainDeltaCm)
    {
        // 情况一：起终点落差超过最大台阶高度——但沿着一条缓坡大步走也可能累计这么大落差。
        // 只接受“共面可行走支撑 + 中间无遮挡直连段”的简单情形。
        FHitResult StartGround, EndGround, Obstruction;
        if (!PIK_TraceGround(StartWS, StartGround) || !PIK_TraceGround(EndWS, EndGround)) return false;
        if (!StartGround.ImpactPoint.Equals(StartWS, 2.f) || !EndGround.ImpactPoint.Equals(EndWS, 2.f) ||
            FVector::DotProduct(StartGround.ImpactNormal, EndGround.ImpactNormal) < 0.99f ||
            FMath::Abs(FVector::DotProduct(EndWS - StartWS, StartGround.ImpactNormal)) > 2.f) return false;
        const FVector Lift = FVector::UpVector * PIK_PathSurfaceLiftCm;
        if (PIK_TraceSegment(StartWS + Lift, EndWS + Lift, Obstruction)) return false;
        OutPointsWS = { StartWS, EndWS };
        return true;
    }
    // 情况二（常规）：两端夹逼。Front 从起点向前找、Back 从终点向后找，最终汇合成一条有序路径。
    const FVector Direction = PIK::Horizontal(EndWS - StartWS).GetSafeNormal();
    if (Direction.IsNearlyZero())
    {
        OutPointsWS = { StartWS, EndWS };
        return true;
    }
    TArray<FVector> Front { StartWS }, Back { EndWS };
    FVector FrontWS = StartWS, BackWS = EndWS;
    const FVector Lift = FVector::UpVector * PIK_PathSurfaceLiftCm;

    // 遇到阻挡后，从命中点向推进方向探一个“抬升的落脚点”（处理上台阶）。
    auto Advance = [&](bool bFromFront, const FHitResult& WallHit) -> bool
    {
        if (WallHit.bStartPenetrating) return false;
        const FVector StepDirection = bFromFront ? Direction : -Direction;
        // 探测点放到障碍棱之后一点，高度取两端与障碍点里的最高者，确保能跨过去。
        FVector ProbeWS = WallHit.ImpactPoint + StepDirection * PIK_EdgeProbeInsetCm;
        ProbeWS.Z = FMath::Max3(StartWS.Z, EndWS.Z, WallHit.ImpactPoint.Z);
        FHitResult TopHit;
        if (!PIK_TraceGround(ProbeWS, TopHit)) return false;
        if (TopHit.ImpactPoint.Z > FMath::Max(StartWS.Z, EndWS.Z) + PIK_MaxTerrainDeltaCm) return false;
        FVector NewPointWS = TopHit.ImpactPoint;
        // 在台阶立面之前就开始提前抬升，并一直保持到脚后跟完全越过它的远端边缘。
        const float SpanCm = PIK::Horizontal(BackWS - FrontWS).Size();
        const float Margin = FMath::Min(PIK_FootClearanceMarginCm, SpanCm * 0.15f);
        NewPointWS -= StepDirection * Margin;
        const float U = PIK_ProjectProgress(NewPointWS, StartWS, EndWS);
        const float FrontU = PIK_ProjectProgress(FrontWS, StartWS, EndWS);
        const float BackU = PIK_ProjectProgress(BackWS, StartWS, EndWS);
        // 新点必须仍在前后边界之间、且不是倒退，否则本次推进失败。
        if (U <= FrontU + PIK::ProgressEpsilon || U >= BackU - PIK::ProgressEpsilon) return false;
        if (bFromFront)
        {
            Front.Add(NewPointWS);
            FrontWS = NewPointWS;
        }
        else
        {
            Back.Add(NewPointWS);
            BackWS = NewPointWS;
        }
        return true;
    };

    bool bClear = false;
    // 反复在两端之间搭桥：先从前向后、再从后向前，直到中间彻底贯通（或迭代耗尽）。
    for (int32 Iteration = 0; Iteration < FMath::Clamp(PIK_MaxPathIterations, 1, 12); ++Iteration)
    {
        FHitResult FrontHit;
        if (!PIK_TraceSegment(FrontWS + Lift, BackWS + Lift, FrontHit))
        {
            bClear = true;
            break;
        }
        if (!Advance(true, FrontHit)) return false;
        FHitResult BackHit;
        if (!PIK_TraceSegment(BackWS + Lift, FrontWS + Lift, BackHit))
        {
            bClear = true;
            break;
        }
        if (!Advance(false, BackHit)) return false;
    }
    // 迭代结束后最后一次尝试探测剩余段是否已贯通。
    if (!bClear)
    {
        FHitResult RemainingHit;
        bClear = !PIK_TraceSegment(FrontWS + Lift, BackWS + Lift, RemainingHit);
    }
    if (!bClear) return false;
    // 汇合：Front 正序 + Back 倒序，去重后组成单调有序的整条路径。
    OutPointsWS = MoveTemp(Front);
    for (int32 Index = Back.Num() - 1; Index >= 0; --Index)
        if (!OutPointsWS.Last().Equals(Back[Index], PIK::PositionEpsilonCm)) OutPointsWS.Add(Back[Index]);
    return OutPointsWS.Num() >= 2;
}

bool UPIKAnimInstance::PIK_UpdateGroundFoot(FPIKFootState& State, bool bLeft, bool bCurvesValid, float DeltaSeconds)
{
    // GROUND 分支：脚直接“钉”在脚下探测到的地面上（支撑/静止/走动时的接触相位）。
    const FTransform MeshToWorld = GetSkelMeshComponent()->GetComponentTransform();
    FVector RawCS, RawWS;
    PIK_GetReferenceFoot(bLeft, RawCS, RawWS);
    FHitResult Ground;
    if (!PIK_TraceGround(RawWS, Ground)) return false;
    const float ScaleZ = FMath::Abs(MeshToWorld.GetScale3D().Z);
    const bool bMoving = PIK::Horizontal(TryGetPawnOwner()->GetVelocity()).Size() >= PIK_MinSpeedCmPerSec;
    // 站姿的参考脚踝高度与走路 clip 不同，不能把站姿里那点竖向偏差当成脚底与坡面的永久缝隙。
    // 因此走动时保留“动画里的原始抬脚量”，静止时则把它插值收敛回 0。
    State.GroundLiftCm = bMoving ? FMath::Max(0.f, static_cast<float>(RawCS.Z) - PIK_AnkleHeightCm) * ScaleZ :
        FMath::FInterpTo(State.GroundLiftCm, 0.f, DeltaSeconds, PIK_ContactInterpSpeed);
    // 脚踝相对脚底的高度要随地面法线一起旋转：纯竖向偏移在坡面上会把脚尖沉进土里。
    State.TargetAnkleWS = Ground.ImpactPoint + Ground.ImpactNormal * PIK_AnkleHeightCm * ScaleZ +
        FVector::UpVector * State.GroundLiftCm;
    State.PlantContactWS = Ground.ImpactPoint;
    State.PathStartWS = Ground.ImpactPoint;
    State.ContactNormalWS = Ground.ImpactNormal;
    // 支撑相位由 FootHeight 曲线判定（带落地/离地迟滞，见状态机入口处解释）。
    const float Height = bCurvesValid ? GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R")) : RawCS.Z;
    State.bPlanted = Height <= (State.bPlanted ? PIK_ReleaseThresholdCm : PIK_PlantThresholdCm);
    State.bInitialized = true;
    State.bPathValid = false;
    State.bPredictionValid = false;
    State.PathPointsWS.Reset();
    State.TimeToLandSec = 0.f;
    State.PathProgress = 0.f;
    return true;
}

bool UPIKAnimInstance::PIK_UpdateFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds,
    const FVector& VelocityWS, float PlayRate, bool bRequestPrediction)
{
    // 单脚状态机总入口：维护 GROUND(接触) ⇄ PREDICT(前向预测) 的切换与平滑收尾。
    const bool bWasInitialized = State.bInitialized;
    const bool bWasPredicting = State.bUsingPrediction;
    const bool bWasPlanted = State.bPlanted;
    FVector ReferenceCS, ReferenceWS;
    PIK_GetReferenceFoot(bLeft, ReferenceCS, ReferenceWS);
    const FVector ReferenceStepWS = State.bHasLastReference
        ? PIK::Horizontal(ReferenceWS - State.LastReferenceWS) : FVector::ZeroVector;
    const FVector PreviousTargetWS = State.TargetAnkleWS;
    const bool bCurvesValid = PIK_AreFootCurvesValid(bLeft);
    const float Height = GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
    if (!State.bUsingPrediction)
    {
        // 每只脚在“自己离地”的那一帧各自进入预测，而不是两只脚一起按速度阈值切换。
        if (bRequestPrediction && bCurvesValid && State.bInitialized && State.bPlanted &&
            Height > PIK_ReleaseThresholdCm)
            State.bUsingPrediction = true;
    }
    // 请求停止（bRequestPrediction 变 false）时先进入“待退出”，保持一小段时间再真正退出，
    // 让脚先安全落下；这是停步/静止时平滑收尾的关键。
    State.bExitPredictionPending = State.bUsingPrediction && !bRequestPrediction;
    State.ExitPredictionTimeSec = State.bExitPredictionPending ? State.ExitPredictionTimeSec + DeltaSeconds : 0.f;
    if (State.bExitPredictionPending && (!bCurvesValid || Height <= PIK_PlantThresholdCm ||
        State.ExitPredictionTimeSec >= PIK_MaxStopHandoffSec))
        State.bUsingPrediction = false;

    bool bValid = false;
    if (State.bUsingPrediction)
        bValid = PIK_UpdatePredictiveFoot(State, bLeft, DeltaSeconds, VelocityWS, PlayRate);
    // 预测失败（比如探测不到地面）就回退到普通地面接触，保证脚至少有处可去。
    if (!bValid)
    {
        State.bUsingPrediction = false;
        State.bExitPredictionPending = false;
        bValid = PIK_UpdateGroundFoot(State, bLeft, bCurvesValid && bRequestPrediction, DeltaSeconds);
    }
    if (!bValid) return false;
    // 支撑锚点与动画脚位存在积累误差。离地时保留动画本帧位移，仅弥合水平接缝；
    // 随实际摆动距离消除差量，不延迟路径抬脚高度，也不冻结整只脚一帧。
    if (bWasPredicting && State.bUsingPrediction && bWasPlanted && !State.bPlanted && State.bHasLastReference)
    {
        State.LiftoffCorrectionWS = PIK::Horizontal(PreviousTargetWS + ReferenceStepWS - State.TargetAnkleWS);
        State.LiftoffTravelCm = 0.f;
    }
    else
        State.LiftoffTravelCm += ReferenceStepWS.Size();
    if (State.bUsingPrediction && !State.bPlanted)
        State.TargetAnkleWS += State.LiftoffCorrectionWS * (1.f - FMath::SmoothStep(
            0.f, FMath::Max(PIK_LiftoffBlendDistanceCm, 0.1f), State.LiftoffTravelCm));
    else
        State.LiftoffCorrectionWS = FVector::ZeroVector;
    State.LastReferenceWS = ReferenceWS;
    State.bHasLastReference = true;
    // 子状态切换的那一帧，新旧目标之间往往有一截位置跳变，记录这个差量用于平滑弥合。
    if (bWasInitialized && bWasPredicting != State.bUsingPrediction)
        State.HandoffCorrectionWS = PreviousTargetWS - State.TargetAnkleWS;
    // 在切换帧先把上一次的输出保留（加回差量），再让差量按指数衰减自然消失，避免脚瞬移。
    State.TargetAnkleWS += State.HandoffCorrectionWS;
    const float Decay = FMath::Exp(-FMath::Max(PIK_ContactInterpSpeed, 0.1f) * DeltaSeconds);
    State.HandoffCorrectionWS *= Decay;
    State.SmoothedNormalWS = bWasInitialized
        ? FMath::Lerp(State.SmoothedNormalWS, State.ContactNormalWS, 1.f - Decay).GetSafeNormal()
        : State.ContactNormalWS;
    return !State.TargetAnkleWS.ContainsNaN();
}

bool UPIKAnimInstance::PIK_UpdatePredictiveFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds,
    const FVector& VelocityWS, float PlayRate)
{
    // PREDICT 分支：脚的摆动路径由“脚踝参考骨 + 前向外推落点 + 地面路径”共同决定。
    USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    const FTransform MeshToWorld = Mesh->GetComponentTransform();
    // 这些参考骨骼被刻意保证永不被 PredictIK 动画层改动，因此可以作为真实“地面脚踝位置”来读取。
    FVector RawFootCS, RawFootWS;
    PIK_GetReferenceFoot(bLeft, RawFootCS, RawFootWS);
    const float HeightCurve = GetCurveValue(bLeft ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
    const float RawTimeSec = GetCurveValue(bLeft ? TEXT("FootTimeToLand_L") : TEXT("FootTimeToLand_R"));
    if (!FMath::IsFinite(RawTimeSec) || RawTimeSec < -0.001f || RawTimeSec > 1.2f) return false;
    // FootTimeToLand 是动画时间（按 1.0 播放速率），除以实际播放速率换算成真实秒数。
    State.TimeToLandSec = FMath::Clamp(RawTimeSec / FMath::Max(PlayRate, 0.05f), 0.f, 1.5f);
    State.TimeSinceBuildSec += DeltaSeconds;
    const bool bWasPlanted = State.bPlanted;
    const float Threshold = bWasPlanted ? FMath::Max(PIK_ReleaseThresholdCm, PIK_PlantThresholdCm) : PIK_PlantThresholdCm;
    const bool bPlantedNow = HeightCurve <= Threshold;

    // 落地 / 首次初始化的瞬间，把“本次支撑的接触点”固定下来作为后续路径的起点。
    FHitResult CurrentGround;
    if (!PIK_TraceGround(RawFootWS, CurrentGround)) return false;
    const bool bJustInitialized = !State.bInitialized;
    if (bJustInitialized || (bPlantedNow && !bWasPlanted))
    {
        State.PlantContactWS = CurrentGround.ImpactPoint;
        State.ContactNormalWS = CurrentGround.ImpactNormal;
        State.PathStartWS = State.PlantContactWS;
        State.bInitialized = true;
        State.bPathValid = false;
        State.PathPointsWS.Reset();
    }
    // 先判断完“本次是否落地”的边界，再更新上一帧状态缓存，顺序不能颠倒。
    State.bPlanted = bPlantedNow;
    // 刚离地的那一帧：把路径起点固定在刚离开的接触点上，路径随之作废待重建。
    if (bWasPlanted && !bPlantedNow)
    {
        State.PathStartWS = State.PlantContactWS;
        State.bPathValid = false;
    }

    // 预测落点 = 以参考骨骼为基准，沿组件前方（+Y）外推一步，再叠加速度在这段时间内的位移。
    // 参考骨骼的 Y 还会乘上步幅混合值，步幅大时预测得更远。
    FVector ReferenceCS = bLeft ? PIK_LandingReferenceCS_L : PIK_LandingReferenceCS_R;
    ReferenceCS.Y *= FMath::Clamp(PIK_ReadALSFloat(TEXT("StrideBlend"), 1.f), 0.25f, 2.f);
    ReferenceCS.Z = 0.f;
    const FVector PredictionWS = MeshToWorld.TransformPosition(ReferenceCS) + VelocityWS * State.TimeToLandSec;
    // 支撑时可更新下一步预测；离地使路径失效，因此离地帧仍会重新预测并建路。
    // 本步路径有效后保持落点，避免台阶边缘使摆动中的路径反复换层。
    // 退出预测的收尾期也保持旧路径。
    FHitResult PredictedGround;
    if (!State.bExitPredictionPending && (!bPIK_LockSwingPrediction ||
        State.bPlanted || !State.bPathValid || !State.bPredictionValid))
    {
        State.bPredictionValid = PIK_TraceGround(PredictionWS, PredictedGround);
        if (State.bPredictionValid)
        {
            State.PredictedContactWS = PredictedGround.ImpactPoint;
            // 只有“同坡度延续”或“落差没超过可跨台阶”的落点才视为有效，防止凭空乱跳。
            const bool bSameSlope =
                FVector::DotProduct(State.ContactNormalWS, PredictedGround.ImpactNormal) > 0.99f &&
                FMath::Abs(FVector::DotProduct(State.PredictedContactWS - State.PathStartWS, PredictedGround.ImpactNormal)) < 2.f;
            State.bPredictionValid = bSameSlope ||
                FMath::Abs(State.PredictedContactWS.Z - State.PathStartWS.Z) <= PIK_MaxTerrainDeltaCm;
        }
    }

    const float WorldAnkleHeightCm = PIK_AnkleHeightCm * FMath::Abs(MeshToWorld.GetScale3D().Z);
    // 已着地的脚保持在本周期落点，不跟下一周期（下一只脚摆动）的预测乱跑。
    if (State.bPlanted)
    {
        State.TargetAnkleWS = State.PlantContactWS + State.ContactNormalWS * WorldAnkleHeightCm;
        State.PathProgress = 0.f;
        return true; // 一只着地的脚不跟随“下一个周期”的预测。
    }
    if (!State.bPredictionValid) return false;
    // 只有当预测点移动超过阈值、且过了重建间隔时才重建路径，控制射线开销。
    const bool bTargetMoved = FVector::DistSquared(State.LastBuildTargetWS, State.PredictedContactWS) >
        FMath::Square(PIK_RebuildDistanceCm);
    if (!State.bPathValid || (State.TimeSinceBuildSec >= PIK_RebuildIntervalSec && bTargetMoved))
    {
        State.bPathValid = PIK_BuildFootPath(State.PathStartWS, State.PredictedContactWS, State.PathPointsWS);
        State.LastBuildTargetWS = State.PredictedContactWS;
        State.TimeSinceBuildSec = 0.f;
        ++State.PathBuildCount;
    }
    if (!State.bPathValid) return false;
    // 摆动相位用脚当前水平位置在整条路径上的投影进度来表示。
    State.PathProgress = PIK_ProjectProgress(RawFootWS, State.PathPointsWS[0], State.PathPointsWS.Last());
    const float PathHeightCm = PIK_SamplePathHeight(State.PathPointsWS, RawFootWS);
    // “动画自己抬起的量” = 参考骨骼相对脚踝基准的多余高度，用来叠加到目标上保持动画感。
    const float AnimatedLiftCm = FMath::Max(0.f, static_cast<float>(RawFootCS.Z) - PIK_AnkleHeightCm) *
        FMath::Abs(MeshToWorld.GetScale3D().Z);
    // 在真正落地的离散事件之前就弥合“摆动/接触”高度差：当 FootHeight 降到落地阈值时，
    // 这里的插值正好退化为“地面点 + 法线方向抬高”，与支撑分支给出的目标完全一致。
    // 全部由姿势驱动，没有持帧缓存或对移动脚目标的时间滞后滤波。
    const float SwingWeight = FMath::SmoothStep(PIK_PlantThresholdCm,
        PIK_PlantThresholdCm + FMath::Max(PIK_ContactBlendHeightCm, 0.1f), HeightCurve);
    const FVector ContactAnkleWS = CurrentGround.ImpactPoint + CurrentGround.ImpactNormal * WorldAnkleHeightCm;
    const FVector SwingAnkleWS(RawFootWS.X, RawFootWS.Y,
        FMath::Max(PathHeightCm, static_cast<float>(CurrentGround.ImpactPoint.Z)) + WorldAnkleHeightCm + AnimatedLiftCm);
    // 水平位置取“当前脚下接触”与“抬起的摆动脚踝”之间按高度权重插值：高度高时抬脚，接近落地时贴地。
    State.TargetAnkleWS = FMath::Lerp(ContactAnkleWS, SwingAnkleWS, SwingWeight);
    // 仅独立诊断进程记录原始求解项，不修改运行时目标或混合权重。
    if (FParse::Param(FCommandLine::Get(), TEXT("PIKHeightProbe")))
    {
        UE_LOG(LogTemp, Display, TEXT("PIK_HEIGHT,%llu,%s,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%.6f,%.6f"),
            static_cast<unsigned long long>(GFrameCounter), *GetOwningActor()->GetName(), bLeft ? TEXT("L") : TEXT("R"),
            DeltaSeconds, PathHeightCm, CurrentGround.ImpactPoint.Z, SwingWeight, AnimatedLiftCm,
            ContactAnkleWS.Z, State.TargetAnkleWS.Z, State.PathBuildCount, State.PathProgress, RawFootWS.Z);
    }
    State.ContactNormalWS = CurrentGround.ImpactNormal;
    return !State.TargetAnkleWS.ContainsNaN();
}

void UPIKAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    // 每帧主循环：资格判断 → 双脚求解 → 骨盆支撑高度 → 转成动画图需要的输出。
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f) return;
    PIK_UpdateDebugOverlay();
    PIK_TraceCount = 0;
    FString Reason;
    const bool bEligible = PIK_IsGroundPoseEligible(Reason);
    const float Dt = FMath::Min(DeltaSeconds, 0.1f); // 帧时间封顶，防大跳变
    bool bValid = false;
    if (bEligible)
    {
        const FVector Velocity = PIK::Horizontal(TryGetPawnOwner()->GetVelocity());
        const float PlayRate = PIK_ReadALSFloat(TEXT("StandingPlayRate"), 1.f);
        FString PredictionReason;
        const bool bRequestPrediction = PIK_IsTestPoseEligible(PredictionReason);
        const bool bLeftValid = PIK_UpdateFoot(PIK_FootState_L, true, Dt, Velocity, PlayRate, bRequestPrediction);
        const bool bRightValid = PIK_UpdateFoot(PIK_FootState_R, false, Dt, Velocity, PlayRate, bRequestPrediction);
        bValid = bLeftValid && bRightValid;
        Reason = bValid
            ? ((PIK_FootState_L.bUsingPrediction || PIK_FootState_R.bUsingPrediction)
                ? TEXT("Active: forward prediction/contact")
                : TEXT("Active: ground contact"))
            : TEXT("No valid ground; fading out");
    }
    PIK_Status = Reason;
    // PIK_Alpha 用恒定速率淡入淡出，避免启用/失效时脚部目标突然切入切出。

    const float BlendSec = bValid ? PIK_BlendInSec : PIK_BlendOutSec;
    PIK_Alpha = FMath::FInterpConstantTo(PIK_Alpha, bValid ? 1.f : 0.f, Dt, 1.f / FMath::Max(BlendSec, 0.01f));
    if (!bValid)
    {
        // 完全淡出后把状态清掉，等下一次合法地面姿态再重新初始化。
        if (PIK_Alpha <= KINDA_SMALL_NUMBER)
        {
            PIK_FootState_L = FPIKFootState();
            PIK_FootState_R = FPIKFootState();
            PIK_PelvisOffsetWorldCm = 0.f;
            bPIK_PelvisPivotInitialized = false;
            PIK_PelvisOffsetCS = FVector::ZeroVector;
        }
        PIK_RecordDebugSample(Dt);
        return;
    }

    const FTransform MeshToWorld = GetSkelMeshComponent()->GetComponentTransform();
    const float BaseFloorZ = MeshToWorld.GetLocation().Z; // 胶囊底部 = 网格根的高度
    // 支撑高度优先取支撑脚接触面；若这只脚正在摆动且预测有效，则提前下探（预判下坡），
    // 上坡则不提前——上抬要等真正接触后再做。
    auto SupportZ = [&](const FPIKFootState& State)
    {
        float Z = State.PlantContactWS.Z;
        // 仅当摆动脚在下行、预测有效时，才按路径进度把支撑高度向更低的预测落点过渡。
        if (!State.bPlanted && State.bPredictionValid && State.PredictedContactWS.Z < Z)
        {
            const float T = FMath::SmoothStep(0.2f, 0.9f, State.PathProgress);
            Z = FMath::Lerp(Z, static_cast<float>(State.PredictedContactWS.Z), T);
        }
        return Z;
    };
    float SupportHeightCm = FMath::Min(SupportZ(PIK_FootState_L), SupportZ(PIK_FootState_R));
    FHitResult BodyGround, LeftGround, RightGround;
    // 在连续平坦面上，用“身体正下方 + 两脚目标”三点的一致性判断地面是平面还是台阶：
    // 若三者法线接近且共面，就以身体正下方的地面为准——支撑脚是一个脚部约束，
    // 不能让一只后锚定的脚决定整条步幅期间身体必须保持的高度。
    if (PIK_TraceGround(MeshToWorld.GetLocation(), BodyGround) &&
        PIK_TraceGround(PIK_FootState_L.TargetAnkleWS, LeftGround) &&
        PIK_TraceGround(PIK_FootState_R.TargetAnkleWS, RightGround))
    {
        // 平面置信度 = 法线一致度 × 共面误差惩罚，两脚取最小值。
        auto PlaneConfidence = [&](const FHitResult& FootGround)
        {
            const float NormalDot = FVector::DotProduct(BodyGround.ImpactNormal, FootGround.ImpactNormal);
            const float PlaneErrorCm = FMath::Abs(FVector::DotProduct(
                FootGround.ImpactPoint - BodyGround.ImpactPoint, BodyGround.ImpactNormal));
            return FMath::SmoothStep(0.97f, 0.995f, NormalDot) * (1.f - FMath::SmoothStep(0.5f, 2.f, PlaneErrorCm));
        };
        const float PlaneWeight = FMath::Min(PlaneConfidence(LeftGround), PlaneConfidence(RightGround));
        SupportHeightCm = FMath::Lerp(SupportHeightCm, static_cast<float>(BodyGround.ImpactPoint.Z), PlaneWeight);
    }
    // 期望偏移 = 支撑高度 − 胶囊底高度，再夹到允许范围。
    float DesiredOffset = FMath::Clamp(SupportHeightCm - BaseFloorZ, -PIK_MaxPelvisOffsetCm, PIK_MaxPelvisOffsetCm);

    // 腿长 reach limit：骨盆不能低到把腿“顶直/拉直”。对两腿各算一次可及下限，取更紧的那个。
    float MaxReachOffsetCm = PIK_MaxPelvisOffsetCm;
    for (int32 Side = 0; Side < 2; ++Side)
    {
        if (!bPIK_ReferencePoseValid || PIK_LegLengthCS[Side] <= 0.f) continue;
        const FVector HipWS = MeshToWorld.TransformPosition(PIK_UnshiftedHipCS[Side]);
        const FVector TargetWS = Side == 0 ? PIK_FootState_L.TargetAnkleWS : PIK_FootState_R.TargetAnkleWS;
        const float ReachCm = PIK_LegLengthCS[Side] * MeshToWorld.GetScale3D().GetAbsMin() * 0.995f;
        // 已知腿长与水平距离，反推“还能再往下放多少”——勾股差，取 max 防根号负数。
        const float VerticalReachCm = FMath::Sqrt(FMath::Max(0.f,
            FMath::Square(ReachCm) - static_cast<float>(FVector::DistSquaredXY(HipWS, TargetWS))));
        MaxReachOffsetCm = FMath::Min(MaxReachOffsetCm, static_cast<float>(TargetWS.Z - HipWS.Z) + VerticalReachCm);
    }
    MaxReachOffsetCm = FMath::Max(MaxReachOffsetCm, -PIK_MaxPelvisOffsetCm);
    DesiredOffset = FMath::Min(DesiredOffset, MaxReachOffsetCm);
    PIK_DebugTargetPelvisOffsetCm = DesiredOffset;
    PIK_DebugReachLimitCm = MaxReachOffsetCm;
    if (!bPIK_PelvisPivotInitialized)
    {
        // 第一次进入时用当前偏移初始化枢轴，避免从 0 突然起跳。
        PIK_PelvisPivotWorldZCm = BaseFloorZ + PIK_PelvisOffsetWorldCm;
        bPIK_PelvisPivotInitialized = true;
    }
    // 平滑的是“绝对支撑高度”，而不是“相对偏移”。胶囊踏步上/下时，
    // 立即减去它的新高度；若对这部分补偿再做平滑，反而会引起身体上浮/下陷的跳变。
    const double TargetPivotWorldZCm = BaseFloorZ + DesiredOffset;
    const double PivotBlend = 1.0 - FMath::Exp(-FMath::Max(PIK_PelvisInterpSpeed, 0.f) * Dt);
    PIK_PelvisPivotWorldZCm = FMath::Lerp(PIK_PelvisPivotWorldZCm, TargetPivotWorldZCm, PivotBlend);
    PIK_PelvisOffsetWorldCm = FMath::Clamp(static_cast<float>(PIK_PelvisPivotWorldZCm - BaseFloorZ),
        -PIK_MaxPelvisOffsetCm, PIK_MaxPelvisOffsetCm);
    // reach limit 约束的是上面的“目的地”。这里不要再把已插值的枢轴硬夹到新算出的 reach limit 上：
    // 落地瞬间该限制可能在一帧内松/紧几厘米，硬夹会重新引入可见的骨盆跳动。
    // 枢轴会在随后几帧自行收敛到那个有界的终点。
    PIK_PelvisPivotWorldZCm = BaseFloorZ + PIK_PelvisOffsetWorldCm;
    PIK_PelvisOffsetCS = MeshToWorld.InverseTransformVector(FVector::UpVector * PIK_PelvisOffsetWorldCm);
    // 脚踝目标是绝对 CS 目标，骨盆位移已被立即补偿掉——这里不再做第二次插值，避免给脚引入延迟。
    PIK_FootTargetCS_L = MeshToWorld.InverseTransformPosition(PIK_FootState_L.TargetAnkleWS);
    PIK_FootTargetCS_R = MeshToWorld.InverseTransformPosition(PIK_FootState_R.TargetAnkleWS);
    // 膝盖 pole target：放在脚前方 + 上方一点，让两段 IK 自然向前弯膝盖。注意叠加骨盆位移。
    const FVector ForwardCS = MeshToWorld.InverseTransformVectorNoScale(TryGetPawnOwner()->GetActorForwardVector());
    PIK_KneeTargetCS_L = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_foot_l"), RTS_Component).GetLocation()
        + ForwardCS * 70.f + FVector(0, 0, 50) + PIK_PelvisOffsetCS;
    PIK_KneeTargetCS_R = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_foot_r"), RTS_Component).GetLocation()
        + ForwardCS * 70.f + FVector(0, 0, 50) + PIK_PelvisOffsetCS;
    // 由平滑后的接触法线算出脚踝“贴坡”旋转；摆动中则按接触权重/路径进度渐变到完全贴地。
    auto FootRotationCS = [&](FPIKFootState& State, FName Bone)
    {
        FVector Normal = State.SmoothedNormalWS.GetSafeNormal();
        FQuat Slope = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
        FVector Axis;
        float Angle;
        Slope.ToAxisAndAngle(Axis, Angle);
        Slope = FQuat(Axis, FMath::Min(Angle, FMath::DegreesToRadians(35.f))); // 倾斜角封顶 35°，防过度扭转脚踝
        if (State.bUsingPrediction && !State.bPlanted)
        {
            const float FootHeight = GetCurveValue(Bone == TEXT("ik_foot_l") ? TEXT("FootHeight_L") : TEXT("FootHeight_R"));
            const float ContactWeight = 1.f - FMath::SmoothStep(PIK_PlantThresholdCm,
                PIK_PlantThresholdCm + FMath::Max(PIK_ContactBlendHeightCm, 0.1f), FootHeight);
            // 物理接触时路径进度可能还不到 1；用高度来补足这最后一点旋转收敛。
            Slope = FQuat::Slerp(FQuat::Identity, Slope,
                FMath::Max(ContactWeight, FMath::SmoothStep(0.7f, 1.f, State.PathProgress)));
        }
        State.SmoothedSlopeWS = FQuat::Slerp(State.SmoothedSlopeWS, Slope,
            1.f - FMath::Exp(-FMath::Max(PIK_ContactInterpSpeed, 0.1f) * Dt)).GetNormalized();
        // 基准旋转取自参考骨骼的原始朝向；最终的组件空间旋转 = 逆网格旋转 * 坡度 * 原始朝向。
        const FQuat RawWS = bPIK_ReferencePoseValid
            ? PIK_ReferencePoseWS[Bone == TEXT("ik_foot_l") ? 0 : 1].GetRotation()
            : GetSkelMeshComponent()->GetSocketQuaternion(Bone);
        return (MeshToWorld.GetRotation().Inverse() * State.SmoothedSlopeWS * RawWS).Rotator();
    };
    PIK_FootRotationCS_L = FootRotationCS(PIK_FootState_L, TEXT("ik_foot_l"));
    PIK_FootRotationCS_R = FootRotationCS(PIK_FootState_R, TEXT("ik_foot_r"));

    if (bPIK_DrawDebug)
    {
        // 世界内可视化：骨盆枢轴、双脚路径/落点、pole target 等。
        const FVector MeshOriginWS = MeshToWorld.GetLocation();
        const FVector PelvisWS = MeshOriginWS + FVector::UpVector * PIK_PelvisOffsetWorldCm;
        DrawDebugBox(GetWorld(), PelvisWS, FVector(7.f, 7.f, 4.f), FColor::Magenta, false, 0.f, 0, 1.2f);
        DrawDebugLine(GetWorld(), MeshOriginWS, PelvisWS, FColor::Magenta, false, 0.f, 0, 1.1f);
        DrawDebugString(GetWorld(), PelvisWS + FVector(0.f, 0.f, 12.f),
            FString::Printf(TEXT("PIVOT offset %+0.1f  target %+0.1f  cap %+0.1f"),
                PIK_PelvisOffsetWorldCm, PIK_DebugTargetPelvisOffsetCm, PIK_DebugReachLimitCm), nullptr, FColor::Magenta, 0.f, true);
        PIK_DrawFoot(PIK_FootState_L, true, FColor::Cyan);
        PIK_DrawFoot(PIK_FootState_R, false, FColor::Orange);
    }
    PIK_RecordDebugSample(Dt);
}

void UPIKAnimInstance::PIK_DrawFoot(const FPIKFootState& State, bool bLeft, const FColor& Color) const
{
    // 单脚的全部调试图元：接触点、预测点、路径、参考脚、目标、法线、旋转坐标系、pole。
    if (!State.bInitialized) return;
    const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
    if (!Mesh || !GetWorld()) return;

    const int32 Side = bLeft ? 0 : 1;
    const FTransform MeshToWorld = Mesh->GetComponentTransform();
    const FVector RawFootWS = bPIK_ReferencePoseValid
        ? PIK_ReferencePoseWS[Side].GetLocation()
        : Mesh->GetSocketLocation(bLeft ? TEXT("ik_foot_l") : TEXT("ik_foot_r"));
    const FVector KneeWS = MeshToWorld.TransformPosition(bLeft ? PIK_KneeTargetCS_L : PIK_KneeTargetCS_R);

    DrawDebugBox(GetWorld(), State.PlantContactWS, FVector(3), FColor::Green, false, 0.f, 0, 0.9f);
    if (State.bPredictionValid)
    {
        DrawDebugBox(GetWorld(), State.PredictedContactWS, FVector(4), FColor::Red, false, 0.f, 0, 1.f);
        DrawDebugDirectionalArrow(GetWorld(), State.PredictedContactWS,
            State.PredictedContactWS + FVector::UpVector * 16.f, 5.f, FColor::Red, false, 0.f, 0, 1.2f);
    }
    for (int32 Index = 0; Index < State.PathPointsWS.Num(); ++Index)
    {
        DrawDebugBox(GetWorld(), State.PathPointsWS[Index], FVector(3), Color, false, 0.f, 0, 1.2f);
        if (Index) DrawDebugLine(GetWorld(), State.PathPointsWS[Index - 1], State.PathPointsWS[Index], Color, false, 0.f, 0, 1.4f);
    }
    DrawDebugBox(GetWorld(), RawFootWS, FVector(2.5f), FColor(190, 190, 190), false, 0.f, 0, 0.8f);
    DrawDebugBox(GetWorld(), State.TargetAnkleWS, FVector(4), FColor::White, false, 0.f, 0, 1.2f);
    DrawDebugDirectionalArrow(GetWorld(), State.TargetAnkleWS,
        State.TargetAnkleWS + State.ContactNormalWS.GetSafeNormal() * 24.f, 6.f, Color, false, 0.f, 0, 1.4f);
    const FRotator FootRotationCS = bLeft ? PIK_FootRotationCS_L : PIK_FootRotationCS_R;
    const FQuat FootRotationWS = MeshToWorld.GetRotation() * FootRotationCS.Quaternion();
    DrawDebugCoordinateSystem(GetWorld(), State.TargetAnkleWS, FootRotationWS.Rotator(), 16.f, false, 0.f, 0, 0.8f);
    DrawDebugLine(GetWorld(), RawFootWS, State.TargetAnkleWS, Color, false, 0.f, 0, 1.f);
    DrawDebugBox(GetWorld(), KneeWS, FVector(3.f), FColor::Yellow, false, 0.f);
    DrawDebugString(GetWorld(), KneeWS, TEXT("POLE TARGET"), nullptr, FColor::Yellow, 0.f, true);
    DrawDebugString(GetWorld(), State.TargetAnkleWS + FVector(0.f, 0.f, 10.f),
        FString::Printf(TEXT("%s %s  P:%0.2f  correction:%0.1fcm"), bLeft ? TEXT("L") : TEXT("R"),
            State.bUsingPrediction ? TEXT("PRED") : TEXT("GROUND"), State.PathProgress,
            FVector::Dist(RawFootWS, State.TargetAnkleWS)), nullptr, Color, 0.f, true);
}
