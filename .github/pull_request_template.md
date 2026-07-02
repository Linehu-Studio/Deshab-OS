## 变更说明

<!-- 简要描述本 PR 做了什么、为什么 -->

## 变更类型

- [ ] feat — 新功能
- [ ] fix — 修复
- [ ] docs — 文档
- [ ] build — 构建系统
- [ ] refactor — 重构
- [ ] test — 测试
- [ ] chore — 杂项

## 影响范围

<!-- 勾选受影响的子系统 -->
- [ ] UTSM 首阶段内核
- [ ] DSK 主内核
- [ ] DKM 驱动（请列出具体驱动： ）
- [ ] DKM ABI / kernel_api
- [ ] 文件系统 / VFS
- [ ] 网络
- [ ] FirstInit / 用户空间
- [ ] 构建 / 镜像打包
- [ ] 文档

## 提交前检查清单

- [ ] `.\build.bat` 构建成功，产出 `ISO/deshab.img`
- [ ] `.\ISO\run_qemu.bat` 启动到 `[DSK] SELFTEST PASS`
- [ ] 无新增编译 warning（`-Wall -Wextra`）
- [ ] ABI 变更已同步所有驱动本地定义（未在结构体中间插入字段）
- [ ] 新驱动已在 `manifest.json` + `limine.conf` 注册
- [ ] 设计文档（`RE/`）与模块 README 已同步
- [ ] 提交信息符合 Conventional Commits
- [ ] `LESSONS_LEARNED.md` 已追加经验记录（如适用）

## 测试方式

<!-- 描述如何验证本变更，如 QEMU 参数、预期串口输出 -->

```text
预期输出：
```

## ABI / 兼容性影响

<!-- 如果修改了 dkm_kernel_api、driver_desc、dsk_boot_context 等 ABI，请说明：
1. 新增字段是否追加到结构尾部
2. 是否影响已有驱动
3. min_kernel_abi / abi_version 是否需要变更 -->
