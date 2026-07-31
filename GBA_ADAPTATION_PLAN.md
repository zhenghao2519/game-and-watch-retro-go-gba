# GBA（gpSP）适配计划报告

日期：2026-07-30  
范围：`GNW_TARGET=zelda`、Flash 构建、STM32H7B0（AXI SRAM 1 MiB + DTCM 128 KiB + AHB SRAM 128 KiB）。

## 结论

当前 gpSP 不能仅通过移除链接器 `ASSERT` 或改变一个 Make 参数适配。它的 GBA 覆盖层为 **978,920 bytes**，而现有 `RAM_EMU` 为 **741,376 bytes（724 KiB）**，最少缺 **237,544 bytes**。此外，链接脚本将 MD 与 GBA 的空间检查顺序相加，产生了额外的 944,272-byte 报错；此检查应修正，但并不解决 GBA 自身超限。

推荐路线是：先完成 **静态 gpSP 内存精简与多 SRAM bank 放置** 的可启动原型；只有在拥有 SD-card 改装硬件并需要大规模维护时，才迁入 SD 版的按需核心加载架构。

## 已测量基线

| 项目 | 大小 | 证据 |
| --- | ---: | --- |
| `RAM_EMU` | 0xB5000 = 741,376 B | `STM32H7B0VBTx_FLASH.ld` |
| GBA 可加载代码/只读数据 | 0x48C70 = 298,096 B | `build/gw_retro_go.map` |
| GBA BSS | 0xA6378 = 680,824 B | `build/gw_retro_go.map` |
| GBA 覆盖层合计 | 0xEEFE8 = 978,920 B | `build/gw_retro_go.map` |
| 纯 GBA 最小缺口 | 237,544 B | 上述两项相减 |
| ITCM 中 GBA CPU 代码 | 0xC000 = 48 KiB | `build/gw_retro_go.map` |

最大已识别静态分配：`ewram` 256 KiB、`gamepak_backup` 128 KiB、`vram` 96 KiB、`iwram` 32 KiB、`memory_map_read` 32 KiB、BIOS 16 KiB、`gamepak_page0_shadow` 32 KiB。它们位于 `cpu.o`/ `gba_memory.o`，而非 ROM 文件本身。

## 外部参考：sylverb SD 版

`sylverb/game-and-watch-retro-go-sd` 将 GBA 标为“experimental, SD-card only”，其 SD 链接脚本通过 `CORES` 区和运行时加载覆盖层来组织核心，而不是复用当前 Flash-only 固件的构建路径。当前公开 `main` 的 Makefile 和 `STM32H7B0VBTx_SDCARD.ld` 没有 `GBA_C_SOURCES` 或 `.overlay_gba`，因此不能直接复制一段 GBA linker rule 来解决本项目问题。

参考：

- https://github.com/sylverb/game-and-watch-retro-go-sd#game-boy-advance
- https://raw.githubusercontent.com/sylverb/game-and-watch-retro-go-sd/main/STM32H7B0VBTx_SDCARD.ld

## 方案对比

| 方案 | 适用条件 | 优点 | 风险/代价 | 建议 |
| --- | --- | --- | --- | --- |
| A. 精简静态 gpSP | 保持现有 Flash-only 架构 | 不依赖 SD 改装；可逐步验证 | 需要修改 gpSP 全局数组和链接脚本 | 先做 |
| B. SD 按需加载核心 | 已有 SD-card 硬改并接受较大架构迁移 | ROM 与核心资源不必都内置；方便更新 | 不能凭空增加运行 RAM；需移植加载器、文件系统和 core 格式 | 备选 |
| C. 只修链接检查 | 仅为诊断 map | 可消除误导性的 MD+GBA 累加错误 | GBA 本体仍超 237 KiB，运行会失败 | 不可单独采用 |
| D. 扩大 `RAM_EMU` | 不减少 UI/其他常驻内存 | 修改最少 | 会重叠 framebuffer/常驻数据，必然不稳定 | 禁止 |

## 阶段计划

### 阶段 0：建立可重复的内存验收

1. 保留当前失败链接生成的 map，并新增 `size-gba` 目标：输出 GBA overlay、BSS、ITCM 和各 SRAM 区使用量。
2. 修正 `._ram_space_check_gba`：它必须从 `__RAM_EMU_START__` 独立开始计算，而不是接在 MD check 后。
3. 保留 `.overlay_gba_bss` 的真实 `ASSERT`；任何方案都必须使其小于 `__RAM_EMU_END__`。
4. 验收：map 显示 GBA overlay 单独大小与总 SRAM 区使用量；不以链接通过作为唯一成功标准。

### 阶段 1：减少不必要的 gpSP 常驻 BSS

1. 将 `gamepak_backup[128 KiB]` 改为按存档类型分配：EEPROM/SRAM/Flash 各自采用实际所需容量；默认不得固定占 128 KiB。
2. 审查 `gamepak_page0_shadow[32 KiB]`：仅在页缓存/XIP 路径需要时分配，加载 ROM 后不再需要时释放或复用。
3. 审查 `bios_rom[16 KiB]` 与内置 open BIOS 是否存在双份驻留；仅保留实际执行路径所需的一份 RAM 副本。
4. 确认 Cortex-M interpreter 构建未意外启用 dynarec 兼容缓冲；禁止定义 `HAVE_DYNAREC`、`GNW_KEEP_EWRAM`、`GNW_KEEP_IWRAM`。
5. 验收：单靠该阶段至少释放 96 KiB；保存加载、Flash 128K 存档游戏与 EEPROM 游戏均通过。

### 阶段 2：利用 DTCM/AHB SRAM（目标：再获得至少 145 KiB）

1. 在链接脚本新增 `.gba_dtcm_bss` 与 `.gba_ahb_bss` 的 `NOLOAD` section，分别放置到 DTCMRAM/AHBRAM；不得与 `.audio` 保留区重叠。
2. 先将延迟容忍的 GBA 数据放到 AHB：存档/页缓存优先，避免把 CPU 高频读写的 `ewram` 直接放到较慢区域。
3. 在 DTCM 仅放置经基准测试确认的热点数据，并始终为启动栈、普通 `.data/.bss` 和运行时 heap 留出余量。
4. 使用显式 section attribute 包装变量，禁止通过全局 wildcard 把整个 `cpu.o` 移到其他 bank。
5. 验收：GBA overlay 本体小于 724 KiB；DTCM/AHB linker ASSERT 均通过；启动菜单、退出 GBA、连续启动不同核心不 HardFault。

### 阶段 3：代码和只读数据重分布

1. ITCM 当前 gpSP CPU 使用 48 KiB，剩余约 16 KiB；使用 profile 将真正热点函数放入剩余 ITCM，而不是盲目增加 ITCM section。
2. 评估将非热点只读表放到外部 Flash/XIP 或现有 `.extflash_emu_data`；只有读取频率低的数据可这样处理。
3. 评估关闭/裁剪功能：RFU、GBP、串口 netplay、作弊、诊断字符串、可选滤镜。每项以功能开关管理，不能静默删除。
4. 验收：至少测试三类游戏（SRAM、Flash、EEPROM），音频连续运行 15 分钟，无越界与存档损坏。

### 阶段 4：运行时与性能验证

1. 在 GBA 启动/退出路径加入所有专用 SRAM 段的清零和恢复流程；避免一个模拟器遗留指针给另一个核心。
2. 验证 D-cache/MPU 属性；AHB DMA audio 区必须继续保持项目既定 non-cacheable 配置。
3. 基准测试帧率、音频 underrun、存档写入和睡眠唤醒。
4. 增加 CI 或本地检查：解析 map，当 GBA overlay 超预算即失败。

## 关键风险

- 所有 128 KiB DTCM/AHB 都不是免费空间：DTCM 承载常驻数据、stack 和 heap；AHB 已预留音频 DMA。阶段 2 必须先从 map 计算剩余容量。
- 移动 GBA EWRAM/VRAM 可能导致性能降低；因此先移动存档和缓存，再移动高频内存。
- 删除链接器 ASSERT 只会将错误从编译期推迟到运行时内存破坏。
- SD 版并不自动解决 RAM 不足；它主要改变核心与资源的存储/加载方式。

## 决策门槛

- 若阶段 1+2 可释放/迁移至少 238 KiB，并保留各 SRAM 区安全余量：继续 Flash-only gpSP 适配。
- 若无法在不牺牲稳定性下获得该空间：停止静态移植，选择 SD 运行时核心架构或不提供 GBA。

## 建议的下一步

执行阶段 0，然后做一个只移动 `gamepak_backup`、页缓存和 BIOS 的最小实验补丁；每次重链后比较 map，不同时改动链接脚本、缓存策略和功能裁剪。
