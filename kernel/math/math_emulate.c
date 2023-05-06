/*
 * linux/kernel/math/math_emulate.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * 该目录里应该包含数学仿真代码。目前仅产生一个信号.
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

/**
 * 协处理器仿真函数。
 * 中断处理程序调用的C函数，参见(kernel/math/system_call.s).
 */
void math_emulate(long edi, long esi, long ebp, long sys_call_ret,
                  long eax, long ebx, long ecx, long edx,
                  unsigned short fs, unsigned short es, unsigned short ds,
                  unsigned long eip, unsigned short cs, unsigned long eflags,
                  unsigned short ss, unsigned long esp)
{
    unsigned char first, second;

    /* 0x0007表示用户代码空间. */
    /**
     * 选择符0x000F表示在局部描述符表中描述符索引值=1，即代码空间。
     * 如果段寄存器cs不等于0x000F则表示cs一定是内核代码选择符，是在
     * 内核代码空间，则出错，显示此时的 cs:eip 值，并显示信息"内核中
     * 需要数学仿真"，然后进入死机状态.
     */
    if (cs != 0x000F)
    {
        printk("math_emulate: %04x:%08x\n\r", cs, eip);
        panic("Math emulation needed in kernel");
    }

    /**
     * 取用户数据区堆栈数据first和second，显示这些数据，并给进程设置浮点
     * 异常信号SIGFPE.
     */
    first = get_fs_byte((char *)((*&eip)++));
    second = get_fs_byte((char *)((*&eip)++));

    printk("%04x:%08x %02x %02x\n\r", cs, eip - 2, first, second);
    current->signal |= 1 << (SIGFPE - 1);
}

/**
 * 协处理器出错处理函数。
 * 中断处理程序调用的C函数，参见(kernel/math/system_call.s).
 */
void math_error(void)
{
    /* 协处理器指令。(以非等待形式)清除所有异常标志、忙标志和状态字位7. */
    __asm__("fnclex");

    /* 如果上个任务使用过协处理器，则向上个任务发送协处理器异常信号. */
    if (last_task_used_math)
        last_task_used_math->signal |= 1 << (SIGFPE - 1);
}
