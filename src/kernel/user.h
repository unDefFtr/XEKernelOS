#pragma once
#include "lib/types.h"

class PagingManager;
void user_init(void);
void enter_user_mode(u32 entry, u32 stack_top, PagingManager *pd,
                     int argc, const char *args);

extern PagingManager *g_user_pd;
extern u32 g_entry_esp;
/* User program arguments — stored by loader, consumed by enter_user_mode */
extern char g_user_args[256];
void user_tss_set_esp0(u32 esp0);  /* 更新 TSS ESP0 */

/* 用户返回 EFLAGS 的 IF，默认 true，启用 IRQ 与时间片抢占。
   false 仅用于隔离诊断；不影响 int 0x80 进入统一的段保存/恢复路径。 */
extern bool g_ring3_irq_on;

/* Ring0 诊断/演示模式，默认 false。正常程序以 CS=0x2B 的 Ring3 运行。
   true 使用 CS=0x18 的远跳转，失去用户内存保护且不支持 fork。
   段上下文修复不依赖此开关，也不以兼容模式启动作为硬件验收。 */
extern bool g_ring0_mode;
