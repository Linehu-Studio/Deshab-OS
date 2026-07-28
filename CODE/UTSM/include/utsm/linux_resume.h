#ifndef UTSM_LINUX_RESUME_H
#define UTSM_LINUX_RESUME_H

#include <utsm/types.h>

/* linux_resume.h — Linux guest park-and-resume 唤醒接口。
 *
 * linux_launch() 启动 Linux guest 后，guest daemon 完成 init 并 HLT 驻留（park）。
 * linux_resume() 通过 vmresume 唤醒 guest，daemon 处理 IPC 请求后再次 HLT park，
 * 控制权返回 host。
 *
 * 典型流程：
 *   1. shell 调用 linux_compat_service->exec(path, argc, argv)
 *   2. exec() 写 IPC 请求到 utsm_to_linux ring
 *   3. exec() 调用 linux_resume() → vmresume → guest daemon 读取请求 → fork+exec
 *   4. daemon 写输出到 linux_to_utsm ring → HLT park → linux_resume() 返回
 *   5. exec() 从 ring 读取输出，返回给 shell
 */

/* 唤醒已 park 的 Linux guest。
 * 前置条件：g_guest_parked == 1（linux_launch 已完成且 guest 已 HLT）。
 * 返回 0 成功（guest 再次 park），负值失败。 */
int linux_resume(void);

/* 查询 Linux guest 是否已 park 就绪。 */
int linux_is_parked(void);

#endif /* UTSM_LINUX_RESUME_H */
