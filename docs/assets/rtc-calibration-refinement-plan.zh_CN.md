<p align="right">
  <strong>简体中文</strong> · <a href="rtc-calibration-refinement-plan.md">English</a>
</p>

# RTC 频偏校准精化方案 (Estimator v3 / RECENT_READY)

## 1. 背景与协作分工
本方案旨在改进 ESP32-C3 RTC 控制器的残差频偏自适应能力，并严格区分生效中的校准证据与估计器生成的候选频偏。当前基线提交为 `0745cf8`，完整门禁已通过，尚无新增硬件实测数据。本增量限于残差频偏精调与校准诊断。

## 2. 核心架构与选择规则

### 2.1 生效校准证据与估计候选解耦
系统在内存与存储层严格区分“当前生效且受信任的校准证据”与“估计器生成的微调候选”。生效校准质量新增四种 RTC 留存枚举：
- `NONE` (0): 无有效校准证据。
- `LEGACY` (1): 已启用但缺少支持元数据的系数。
- `STABLE` (2): 多窗口验证的一致性校准。
- `RECOVERY` (3): 带经验不确定度的恢复补偿，物理精度仍待实测。

Estimator v3 引入评估阶段的独立质量枚举：`RECENT_READY` (4)。`RECENT_READY` 表示已通过独立保留集验证的微调候选，绝不可作为首次激活证据，亦不能使未校准设备直接转为已校准。

### 2.2 Estimator v3 三窗口选择与交叉验证规则
仅在全局严格一致性判定为 `INCONSISTENT` 时，触发如下确定性评估：
1. **窗口选取**：严格选取最近且 `<= 7 天`、同源、无重叠的 3 个有效窗口。严禁跳过更新的排除行，严禁回溯挑选更有利的历史三元组。
2. **候选拟合**：前 2 个窗口在满足 `max(200 ppm, sum(reference allowances))` 容差一致性约束下，通过加权线性拟合计算四舍五入的整型候选值 `C_cand` (ppm)。
3. **独立保留集回放**：第 3 个窗口（最新）作为独立测试集，严禁参与拟合计算。直接在该保留窗口上回放候选值 `C_cand`。
4. **准入判定**：回放结果必须同时满足：
   - 基线残差误差改善 `>= 20%`；
   - 绝对误差改善 `>= 1 ppm` 等效值；
   - 绝对回放误差的改善必须严格超过不确定度边界：`Margin_us = 2 * holdout.combined_uncertainty_us + uncertainty_ppm * bridge_us / (1000000 + candidate_ppm)`。
   - 严禁使用 `sqrt(N)` 人为缩小不确定度。
5. **仅限精化已有生效频偏**：`RECENT_READY` 仅允许在当前频偏已经合格启用（包括缺少元数据的 `LEGACY`） 时进行微调。严禁绕过 pending/gap/source 门控，严禁相同三元组重复漂移，严禁引起 UTC 或安全时钟发生阶跃。

### 2.3 状态留存、闪存持久化与接口隔离
- **RTC 内存留存**：安全升级 RTC 留存内存布局版本号。在深度睡眠与影子重算期间，完整保留生效质量 (0..3)、估计器版本、最新支持序号与 UTC 时间戳、毫 ppm 不确定度及有效性标志。
- **NVS 闪存参数**：仅保存整型 ppm 频偏。冷启动读取的频偏标记为 `LEGACY` 未知状态，直到获得全新有效证据。记录持久化与频偏持久化逻辑严格解耦。
- **导出与 API**：历史 API 提供白名单校准对象，包含独立字段：`coefficient_saved`（生效值等于已存有效 ppm）、`coefficient_save_blocked`、`saved_ppm` 及 `coefficient_save_budget_us`。缺失元数据按未知处理，格式损坏数据直接拒绝。
- **底层保持不变**：24小时/6小时写预算、8MB 分区表、账户与 PIN 格式、硬件时钟源、120s/900s 唤醒周期及温补模型均保持不变。

## 3. 方案对比与权衡
- **方案 A（已采纳 - 2 拟合 + 1 保留集硬门限）**：保留独立的最新验证窗口，限制仅采用一致证据进行调整，且不依赖虚高的 `sqrt(N)` 统计缩减。
- **方案 B（放弃 - 3 窗口联合回归）**：缺乏独立外样本测试，易将瞬态波动固化为系统误差。
- **方案 C（放弃 - 允许近期窗口直接初次校准）**：缺乏长周期基准校验，极易误锁不可靠频偏。

## 4. 实施任务清单
- [x] 1. 在校准核心类型中新增 `RTC_HISTORY_ESTIMATE_RECENT_READY = 4`，并安全递增 RTC 留存结构版本。
- [x] 2. 实现严格的三窗口选择器（最新 3 个 `<= 7 天`、同源且无重叠窗口，禁止行跳跃）。
- [x] 3. 实现前 2 窗口拟合及第 3 窗口独立保留集回放，严格校验 20% 改善、1 ppm 改善及不缩小的不确定度门限。
- [x] 4. 限制 `RECENT_READY` 仅精化既有合格频偏，并增加防重入机制避免同三元组重复漂移。
- [x] 5. 解耦记录持久化与参数闪存持久化，独立暴露保存状态、受限状态与预算耗时。
- [x] 6. 更新序列化与历史导出字段，完成白名单机制与旧版本兼容校验。

## 5. 验证命令与交付门禁
先编写针对性回归并观察预期失败，再实现行为。在已激活的 ESP-IDF 5.5.6 环境运行以下检查；将 `PASSPORT_RTC_ARCHIVE` 设置为成功门禁输出的准确归档目录：
```bash
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_history_estimator.c main/rtc_history.c main/rtc_drift.c -lm -o /tmp/passport-rtc-estimator
/tmp/passport-rtc-estimator
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_clock.c main/rtc_trace.c main/rtc_history.c main/rtc_history_store.c main/rtc_drift.c -lm -o /tmp/passport-rtc-clock
/tmp/passport-rtc-clock
python3 tests/test_rtc_history_http.py
node tests/test_web_companion.js
./tools/validate.sh
python3 tools/archive_firmware.py verify "$PASSPORT_RTC_ARCHIVE"
```

交付验证状态：
```text
Build: PASS
Host tests: PASS
Device tests: PASS（仅限固件哈希校验、启动和现有历史记录加载：自归档 5dbe4d770819244220c56e81a259b2611360d499ffde50f11cea1cdfa24b9912 写入 0x10000 的应用 SHA bf4a7d37f747292feb76184a98043626c19281d87986db496a8a4131efef92cf 校验通过；启动成功；成功加载 3 条历史记录且 ready=1、当前生效 -7165ppm，未观察到崩溃）
Unverified: 待验证 Web 界面新增元数据与导出功能，以及隔夜电池漂移测试（未声明 30 天 ±3 秒精度保证或账户渲染与数量校验）。
```
*固件烧录需先征得用户明确授权；禁止执行任何 git push 操作。*

## 6. 物理电池隔夜测试规程（未验证说明）
1. 准确同步基准时间。
2. 拔出 USB 数据线，允许设备在电池供电下自动息屏并进入睡眠。
3. 次日清晨启动并比对参考时间差值，采集原始漂移数据（在下一次显式同步前完成比对，保留正常睡眠补偿）。
4. 收集全新独立窗口进行后续验证，不得复用已参与拟合的数据。

## 7. 后续工作（本期范围外）
持久化全生命周期溯源链、浅睡/深睡分段建模以及动态温度/电量耦合模型均留待后续迭代，不计入本次交付。

## 8. 复核后的验收契约

实际部署的整数候选须与最新验证窗口满足 `max(200 ppm, training_uncertainty_ppm + holdout_uncertainty_ppm)` 一致性约束。训练不确定度取两个训练窗口各自参考误差与相对整数候选的绝对偏差之和的最大值；最终经验误差还覆盖最新验证窗口的参考误差与偏差，不能当作置信区间或未来物理误差界。RTC 保存的毫 ppm 向上取整。候选回放保留 Q19 小数并调用生产整数比例算法；1 ppm 等效阈值为 `holdout_bridge_us / 1000000`，20% 与 1 ppm 判定含等号，不确定度门限严格大于。两个训练窗口各自恶化不得超过其两倍参考误差加 1 ppm 等效值。

所有历史发布路径 READY、RECENT_READY、RECOVERY 都须使用比活动元组更新的证据序号，且支持证据的 UTC 终点不得倒退。原双窗口发布在可用时也记录已接纳终点，其未知不确定度仍标记无效。READY 和 RECENT_READY 对应活动 STABLE，RECOVERY 对应活动 RECOVERY。候选重算、学习重置与保存失败均不能替换活动元组。本轮不做同系数质量升级，因此冷启动读入的未变化系数可能长期保持 LEGACY。证据与水位在深睡中保留，完全掉电会丢失；仅保存 ppm 的 NVS 无法保证终身证据去重。

没有有效已存系数时 API 输出 `saved_ppm: null`；`coefficient_saved` 必须同时满足活动系数已合格、已存值有效且两者相等。旧固件缺少元数据可兼容，出现 null、不完整、小数整数、越界或标志矛盾的元数据则由网页拒绝。用真实 HTTP 格式化函数测试字段最大宽度，保留 768 字节分块与 1024 字节邮箱上限。测试覆盖环形回绕、七天边界、小数与舍入、矛盾验证窗口、重复刷新及无新终点的老化、待存及持久化缺口、两个发布路径、校验和正确但元数据无效的留存、冷启动与保存失败。
