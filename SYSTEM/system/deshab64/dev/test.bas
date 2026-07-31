# Deshab 自动测试脚本 v1
# dev_mode=1 时由 shell 自动加载执行
# 手动触发: 输入 test 命令

$section 文件系统基础
ls
$assert_file FIRSTINTXT

$section 写入测试
echo Deshab test > TEST.TXT
cat TEST.TXT
$assert_file TEST.TXT

$section 复制测试
cp TEST.TXT COPY.TXT
$assert_file COPY.TXT

$section 移动测试
mv COPY.TXT MOVED.TXT
$assert_nofile COPY.TXT
$assert_file MOVED.TXT

$section 清理
rm TEST.TXT
rm MOVED.TXT
$assert_nofile TEST.TXT
$assert_nofile MOVED.TXT

$section 配置验证
$check_config dsk.show_logo
$check_config boot.dev_mode
$check_config drivers.e1000

$section 设备与版本
pci
version
uname

$section 内核插桩
$probe kernel_version
$probe block_device
$probe memory_map
$probe irq_status

$end
