# Learning: mcopy -o 热补镜像会剥离 LFN 长名链

**日期**: 2026-09-12
**分类**: best_practice
**优先级**: high

## Summary

用 `mcopy -o` 覆盖 GPT 镜像 ESP 内**已存在的带 LFN 文件**时，mtools 会删除旧条目并写入新条目，但**新条目不带 LFN 长名链**（只有 8.3 短名，如 `SIMHEI~1.DBF`）。裸机 fat32_io.h 的 `f32_find_in_dir_lfn` 先做 LFN 匹配再回退 8.3 短名匹配——对 `simhei_16.dbf` 这类长名文件，短名 `SIMHEI~1.DBF` 无法匹配 → 查找返回 -3。

## Details

- 现象：zhfont 加载 `system/font/simhei_16.dbf` 返回 -3（文件未找到），但同目录 simhei.ttf（LFN 完好）能被 mdir 正常列出
- 诊断：Python 直读镜像目录簇（offset = (2048 + data_lba + (clus-2)*spc)*512）对比发现 dbf 条目前无 0x0F LFN 项
- 修复：`mdel ::SYSTEM/FONT/SIMHEI~1.DBF` 后重新 `mcopy`（新建条目会带 LFN）
- 同日另一坑：q35 下启动盘挂 `-device ide-hd,bus=ide.0` 会落在 AHCI 端口 2 且签名异常（identified ports=0）；应使用显式 `-device ahci,id=ahci0 ... bus=ahci0.0`（run_qemu_kvm.sh 的挂法）

## Suggested Action

热补 ESP 内长名文件的正确流程：`mdel` + `mcopy`，**不要** `mcopy -o`。验证：补丁后 `mdir -i img@@1048576 ::路径` 确认 LFN 列还在（行尾显示长名）。boot 日志 `[zhfont] dbf loaded (16px ok)` 为加载成功标记。

## Metadata

- Source: M1 zhfont 集成调试（zhfont_test PASS 但 QEMU 内 -3）
- Related Files: CODE/desktop/zhfont.c, CODE/desktop/f32io.c, CODE/tools/fat32_io.h
- Tags: mtools, fat32, lfn, hotpatch, qemu
