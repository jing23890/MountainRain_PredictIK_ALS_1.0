#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "PIKAnimInstance.generated.h"

class SPIKDebugOverlay;
class UGameViewportClient;

/** 路径里所有点都是“世界空间支撑高度”，不含脚踝高度（即都是地面接触点那一层）。 */
USTRUCT(BlueprintType)
struct FPIKFootState
{
    GENERATED_BODY()

    // ---- 一只脚在 GROUND / PREDICT 两个子状态之间切换的核心状态机变量 ----

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    bool bInitialized = false;       // 是否已至少成功落地过一次（首次落地前不产生有效目标）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    bool bPlanted = false;           // 当前是否处于“脚已着地（支撑）”相位
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    bool bPredictionValid = false;   // 预测落点（PredictedContactWS）在本帧是否有效
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    bool bPathValid = false;         // 当前摆动路径 PathPointsWS 是否有效
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    FVector PlantContactWS = FVector::ZeroVector;        // 本次支撑期的真实地面接触点（世界空间）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    FVector PathStartWS = FVector::ZeroVector;          // 摆动路径的起点（离开地面时的 PlantContactWS 快照）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    FVector PredictedContactWS = FVector::ZeroVector;   // 外推得到的未来落点（摆动路径终点）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    FVector TargetAnkleWS = FVector::ZeroVector;        // 本帧要交给 IK 的脚踝最终目标（世界空间，含脚踝抬高）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    FVector ContactNormalWS = FVector::ZeroVector;      // 当前接触面的法线
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    TArray<FVector> PathPointsWS;                       // 本次摆动沿地面/台阶构建的有序路径点（世界空间）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    float TimeToLandSec = 0.f;                          // 距落地的预计秒数（由动画曲线换算）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    float PathProgress = 0.f;                           // 脚在路径上的推进进度 0→1
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    int32 PathBuildCount = 0;                           // 累计重建路径的次数（调试用）
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    bool bUsingPrediction = false;   // 当前是否处于 PREDICT（摆动预测）状态
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Foot") 
    bool bExitPredictionPending = false; // 已请求退出预测但仍在平滑收尾（停步/落下收尾阶段）
    

    FVector LastBuildTargetWS = FVector::ZeroVector;   // 上次构建路径用的终点，用于判断预测点是否移动了足够距离
    float TimeSinceBuildSec = 0.f;                     // 距上次构建路径的时间，配合重建间隔做按帧节流
    float ExitPredictionTimeSec = 0.f;                 // 退出预测的计时器累计
    FVector HandoffCorrectionWS = FVector::ZeroVector; // 子状态切换帧的目标差量，用于指数衰减平滑，防止切换瞬间脚跳
    FVector SmoothedNormalWS = FVector::UpVector;      // 平滑后的接触法线（世界空间），用于脚部贴地旋转
    FVector LastReferenceWS = FVector::ZeroVector;
    FVector LiftoffCorrectionWS = FVector::ZeroVector; // 离地时支撑锚点与动画脚位的水平差量
    float LiftoffTravelCm = 0.f;
    bool bHasLastReference = false;
    FQuat SmoothedSlopeWS = FQuat::Identity;           // 平滑后的坡度四元数，用于把脚踝旋转向地面
    float GroundLiftCm = 0.f;                          // 静止/走动时脚底相对支撑面多抬起的量，避免脚踝初始高度在坡面上下陷
};

/** 单帧采样点，用于游戏内 IK / 动画曲线的叠加绘制（调试浮层的曲线图）。 */
struct FPIKDebugSample
{
    float TimeSec = 0.f;         // 采样时钟
    float Alpha = 0.f;           // PIK 整体混合权重
    float PelvisOffsetCm = 0.f;  // 骨盆世界偏移（厘米）
    float FootHeightL = 0.f;     // 左脚 FootHeight_L 曲线值
    float FootHeightR = 0.f;     // 右脚 FootHeight_R 曲线值
    float TimeToLandL = 0.f;     // 左脚 FootTimeToLand_L 曲线值
    float TimeToLandR = 0.f;     // 右脚 FootTimeToLand_R 曲线值
    float PathProgressL = 0.f;   // 左脚路径进度
    float PathProgressR = 0.f;   // 右脚路径进度
    float TargetHeightL = 0.f;   // 左脚目标脚踝世界 Z
    float TargetHeightR = 0.f;   // 右脚目标脚踝世界 Z
};


//PIK 求解器本体

UCLASS(Blueprintable, BlueprintType)
class ADVANCEDLOCOMOTIONV4_API UPIKAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    virtual void NativeInitializeAnimation() override;
    virtual void NativePostEvaluateAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void BeginDestroy() override;
    virtual void NativeUninitializeAnimation() override;

    // ---- 总开关 / 调试 ----
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Enable") 
    bool bPIK_Enabled = true;
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Debug") 
    bool bPIK_DrawDebug = false;       // 世界内 DrawDebug 追踪线
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Debug") 
    bool bPIK_DrawScreenDebug = true;  // 屏幕左上 Slate 调试面板
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Debug", meta=(ClampMin="30", ClampMax="600")) 
    int32 PIK_DebugHistorySamples = 240; // 曲线图最多保留的历史采样数

    // ---- 落地接触判定（由动画曲线 FootHeight 驱动） ----
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0", Units="cm")) 
    float PIK_PlantThresholdCm = 14.f;      // 脚“落地”阈值：FootHeight 降到该值以下视为支撑开始
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0", Units="cm")) 
    float PIK_ReleaseThresholdCm = 14.5f;  // 脚“离地”阈值：带迟滞（略高于落地阈值）
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0", Units="cm")) 
    float PIK_AnkleHeightCm = 13.46f;      // 参考骨骼脚踝相对地面的高度（动画校准得到）
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0.1", Units="cm")) 
    float PIK_ContactBlendHeightCm = 2.f; // 摆动/接触间高度混合带宽度

    // ---- 地面探测（射线） ----
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", Units="cm")) 
    float PIK_TraceUpCm = 80.f;      // 向上探测长度
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", Units="cm")) 
    float PIK_TraceDownCm = 120.f;    // 向下探测长度
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", Units="cm")) 
    float PIK_MaxTerrainDeltaCm = 45.f;  // 允许的最大地形高度落差（超过则走“缓坡直连”分支）
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="0.1", Units="cm")) 
    float PIK_PathSurfaceLiftCm = 2.f;  // 路径探测沿支撑面抬升的偏移，避免贴合面浮点误差
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="0.1", Units="cm")) 
    float PIK_EdgeProbeInsetCm = 3.f;   // 绕过高台/台阶边时的探测内缩量
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="0", Units="cm")) 
    float PIK_FootClearanceMarginCm = 8.f; // 脚从台阶前缘提前抬升的安全余量
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace", meta=(ClampMin="1", ClampMax="12")) 
    int32 PIK_MaxPathIterations = 6;    // 路径构建最多往返迭代次数
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Trace") 
    TEnumAsByte<ECollisionChannel> PIK_TraceChannel = ECC_Visibility; //检测通道，可视

    // ---- 预测（前向摆动） ----
    // 前进走路测试：离地时确定本步落点，摆动途中不因预测跨过台阶边缘而换路。
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction")
    bool bPIK_LockSwingPrediction = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.1", Units="cm"))
    float PIK_LiftoffBlendDistanceCm = 12.f; // 脚向前摆过该距离后完全接回动画水平位置

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.01", Units="s")) 
    float PIK_RebuildIntervalSec = 0.10f; // 重建路径的最小时间间隔（节流）
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.1", Units="cm")) 
    float PIK_RebuildDistanceCm = 6.f;    // 预测点移动超过该距离才重建路径
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0.5", ClampMax="1"))
    float PIK_MinForwardDot = 0.98f;    // 判为“直线前向行走”的最小前进点积

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Prediction", meta=(ClampMin="0", Units="cm/s"))
    float PIK_MinSpeedCmPerSec = 10.f;   // 进入预测所需的最小水平速度

    // ---- 混合 / 淡入淡出 ----
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Blend", meta=(ClampMin="0.01", Units="s"))
    float PIK_BlendInSec = 0.15f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Blend", meta=(ClampMin="0.01", Units="s"))
    float PIK_BlendOutSec = 0.12f;

    // ---- 骨盆支撑 ----
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Pelvis", meta=(ClampMin="0", Units="cm"))
    float PIK_MaxPelvisOffsetCm = 40.f; // 骨盆最大允许偏移

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Pelvis", meta=(ClampMin="0.1"))
    float PIK_PelvisInterpSpeed = 10.f;           // 骨盆支撑高度的平滑速度

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Contact", meta=(ClampMin="0.1"))
    float PIK_ContactInterpSpeed = 15.f;        // 法线/切换量的平滑速度

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Blend", meta=(ClampMin="0.05", Units="s"))
    float PIK_MaxStopHandoffSec = 0.3f; // 停步时由预测平滑退回地面接触的最长时间

    //从 ALS_N_Walk_F 在 FootTimeToLand_L/R == 0 时刻采样的脚踝位置
    //（该时刻脚正好落地，采样值为 L(0.2) / R(0.7666667) 秒）。
    // 注意：ALS 人形骨骼的组件空间 +Y 才是“前方”，别用解算后的脚去推导这两个值。
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Walk Calibration")
    FVector PIK_LandingReferenceCS_L = FVector(6.076324, 10.247291, 13.465718);

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="PIK|Walk Calibration")
    FVector PIK_LandingReferenceCS_R = FVector(-6.076340, 10.247272, 13.465560);

    // ---- 输出：供派生 AnimBP 的动画图（PredictIK 层）读取并应用 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    float PIK_Alpha = 0.f;                    // 整体混合权重 0→1

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FVector PIK_PelvisOffsetCS = FVector::ZeroVector;     // 骨盆位移（组件空间）

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FVector PIK_FootTargetCS_L = FVector::ZeroVector;    // 左脚踝 IK 目标

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FVector PIK_FootTargetCS_R = FVector::ZeroVector;    // 右脚踝 IK 目标

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FVector PIK_KneeTargetCS_L = FVector::ZeroVector;    // 左膝 pole vector 目标

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FVector PIK_KneeTargetCS_R = FVector::ZeroVector;    // 右膝 pole vector 目标

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FRotator PIK_FootRotationCS_L = FRotator::ZeroRotator; // 左脚踝旋转（组件空间）

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Output")
    FRotator PIK_FootRotationCS_R = FRotator::ZeroRotator; // 右脚踝旋转（组件空间）

    // ---- 逐脚状态机 / 调试输出 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|State")
    FPIKFootState PIK_FootState_L;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|State")
    FPIKFootState PIK_FootState_R;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Debug")
    FString PIK_Status = TEXT("Not initialized"); // 供浮层显示的人类可读状态

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Debug")
    int32 PIK_TraceCount = 0;                       // 本帧射线数（调试）

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Debug")
    float PIK_DebugTargetPelvisOffsetCm = 0.f;     // 骨盆偏移目标值（未夹取前，调试）

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PIK|Debug")
    float PIK_DebugReachLimitCm = 0.f;             // 当前腿长可及的骨盆上限（调试）

    UFUNCTION(BlueprintCallable, Category="PIK")
    void PIK_ResetPrediction();

    // ---- 供调试浮层读取的采样数据 ----
    const TArray<FPIKDebugSample>& PIK_GetDebugSamples() const { return PIK_DebugSamples; }
    float PIK_GetDebugSampleClock() const { return PIK_DebugSampleClock; }
    float PIK_GetPelvisOffsetWorldCm() const { return PIK_PelvisOffsetWorldCm; }

    // 被运行时与编辑器自动化测试共用；构建一条有界、有序的安全路径包络。
    bool PIK_BuildFootPath(const FVector& StartWS, const FVector& EndWS, TArray<FVector>& OutPointsWS);
    static float PIK_ProjectProgress(const FVector& PointWS, const FVector& StartWS, const FVector& EndWS);
    static float PIK_SamplePathHeight(const TArray<FVector>& PointsWS, const FVector& PositionWS);

private:
    void PIK_GetReferenceFoot(bool bLeft, FVector& OutCS, FVector& OutWS) const; // 取当前“参考脚踝”位置（组件/世界）
    FTransform PIK_ReferencePoseCS[2];   // 上一帧评估完成的参考脚（ik_foot_*）组件空间变换
    FTransform PIK_ReferencePoseWS[2];   // 参考脚的“世界空间”缓存
    bool bPIK_ReferencePoseValid = false; // 上一帧是否成功从评估后的 Pose 里读到参考骨骼
    FVector PIK_UnshiftedHipCS[2];   // 去除骨盆偏移后的髋部位置（避免把 IK 反馈进 reach limit）
    float PIK_LegLengthCS[2] = {0.f,0.f}; // 髋→膝→踝三段总长，用于骨盆 reach limit
    bool PIK_UpdateFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds, const FVector& VelocityWS, float PlayRate, bool bRequestPrediction); // 单脚总入口：决定 GROUND / PREDICT 并分发
    bool PIK_UpdatePredictiveFoot(FPIKFootState& State, bool bLeft, float DeltaSeconds, const FVector& VelocityWS, float PlayRate); // PREDICT 分支：外推落点 + 建路径
    bool PIK_UpdateGroundFoot(FPIKFootState& State, bool bLeft, bool bCurvesValid, float DeltaSeconds); // GROUND 分支：直接贴当前地面
    bool PIK_AreFootCurvesValid(bool bLeft) const;  // 曲线是否在合法区间（区间硬编码见实现）
    bool PIK_TraceGround(const FVector& OriginWS, FHitResult& Hit); // 从 Origin 向下探测可行走地面
    bool PIK_TraceSegment(const FVector& StartWS, const FVector& EndWS, FHitResult& Hit); // 两点间直线探测
    bool PIK_IsWalkable(const FHitResult& Hit) const; // 命中的法线是否可站立
    bool PIK_IsTestPoseEligible(FString& Reason) const;   // 是否为“直线前向行走”姿势（可进入预测）
    bool PIK_IsGroundPoseEligible(FString& Reason) const; // 是否为可启用 PIK 的地面姿态（总资格）
    float PIK_ReadALSFloat(FName Name, float Fallback) const; // 反射：按名字读派生类上的 float 属性（如 StrideBlend）
    int64 PIK_ReadALSEnum(FName Name, int64 Fallback) const;  // 反射：按名字读派生类上的枚举（如 MovementState/Gait）
    void PIK_DrawFoot(const FPIKFootState& State, bool bLeft, const FColor& Color) const; // 世界内调试绘制
    void PIK_RecordDebugSample(float DeltaSeconds);   // 往采样缓冲里记一帧
    void PIK_UpdateDebugOverlay();                    // 按条件创建/移除屏幕浮层
    void PIK_RemoveDebugOverlay();
    float PIK_PelvisOffsetWorldCm = 0.f; // 骨盆世界偏移（厘米），作为插值目标
    // 在“世界空间”里插值支撑枢轴高度：这样胶囊踏步瞬间不会让枢轴跳变。
    double PIK_PelvisPivotWorldZCm = 0.0;       // 世界空间骨盆支撑枢轴高度（厘米）
    bool bPIK_PelvisPivotInitialized = false;
    float PIK_DebugSampleClock = 0.f;                     // 采样时钟（秒）
    TArray<FPIKDebugSample> PIK_DebugSamples;             // 环形采样缓冲
    TSharedPtr<SPIKDebugOverlay> PIK_DebugOverlay;        // 屏幕调试浮层
    TWeakObjectPtr<UGameViewportClient> PIK_DebugViewport; // 浮层所在的视口
};
