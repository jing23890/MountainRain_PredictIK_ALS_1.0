# PredictIK_ALS_MouR
基于ALS框架的预测ik制作尝试

## 当前版本

UE 5.7 项目，使用 `ALS_AnimBP` 的 `PredictIK` 层，核心计算位于 `UPIKAnimInstance`。

- 前进预测仅用于 `ALS_N_Walk_F`；站立和其他受支持的地面姿势使用当前脚下检测，不增加其他方向的预测曲线。
- 左右脚独立维护支撑锚点、预测落点和双向障碍检测路径。
- 保留 14 cm 落地容差，离地阈值为 14.5 cm。
- 盆骨采用世界高度 pivot 插值，双脚使用组件空间目标解算。
- 已开启单帧 Trace、路径线和 Debug Box；可在动画蓝图类默认值中关闭 `bPIK_DrawDebug`。
- `bPIK_DrawDebug` 开启世界空间调试绘制；`bPIK_DrawScreenDebug` 独立开启左上角实时调试面板，显示 IK 状态、Alpha、Trace 数量、骨盆偏移、脚步路径进度，以及 `FootHeight_L/R` 和 `FootTimeToLand_L/R` 曲线。`PIK_DebugHistorySamples` 控制曲线历史采样数。
- `PIK_PelvisInterpSpeed` 控制高度跟随速度，C++ 默认值为 10，当前 `ALS_AnimBP` 覆盖为 15。

地面贴合与预测共用一套盆骨和双脚解算：每只脚离地时进入预测；停步后优先在落地时交回地面贴合。曲线失效、路径失败或等待超过 `PIK_MaxStopHandoffSec`（默认 0.3 秒）时平滑交接。缺失曲线不作为落地事件。空中、蹲伏、攀爬/翻滚等运动动作不启用地面解算。

坡面根据命中法线调整脚掌旋转和脚踝到脚底的偏移。`PIK_ContactInterpSpeed`（默认 15）控制交接修正的释放速度和坡面旋转平滑；盆骨仍使用独立的世界高度 pivot 插值。

连续坡面上，骨盆目标使用身体下方的地面高度，避免后脚旧支撑点拖住身体抬升。通过身体和双脚的三次地面检测，以法线一致性及共面误差混合该目标；台阶等不连续地面保留原支撑高度策略。使用移除已施加骨盆偏移的髋部姿势和腿长限制抬升，插值后的结果也受该限制约束。极端横向距离仍可能超出腿部可达范围，本改动不移动支撑脚来处理这种情况。

落地接合由动画脚高驱动：`PIK_ContactBlendHeightCm` 默认 2 cm，在落地阈值上方这段高度内逐渐将摆动目标接到脚下接触目标。到达 14 cm 时两套位置计算一致，脚掌旋转也提前接合；不在接触帧冻结目标。

参考脚在 `NativePostEvaluateAnimation` 中从本次求值的组件空间姿势缓存，并同时保存对应的世界变换；下一次更新使用这份一致的输入，避免将旧骨骼姿势与新 Mesh 位置拼接。该方案仍使用上一份完成的参考姿势，未改成自定义动画节点内的同帧姿势求解。

## 获取与打开

安装 Git LFS 后克隆仓库，并执行 `git lfs pull` 获取动画及地图资产。
使用 UE 5.7 打开 `AdvancedLocomotionV4.uproject`。首次打开需要本机 C++ 构建环境，并编译项目模块和随项目提供的 MCP 插件源码。
编译产物、缓存及本机配置不纳入版本控制。

## 验证

- C++ 与动画蓝图编译通过。
- `ALS.PredictIK.PathGeometry` 自动测试覆盖平地、上下台阶、障碍路径及无效路径。
- 完成平地前进走路与临时 20 cm 台阶运行采样。
- `Scripts/PIK_IsolatedProbe.py` 仅用于带 `-PIKIsolatedTest` 标记的独立测试进程，不保存测试场景。
- `Scripts/PIK_SlopeProbe.py` 验证 20° 坡面的站立、横向站立、起步、停步及空中淡出，同样仅在独立测试进程运行。
