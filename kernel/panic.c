/*
 *  linux/kernel/panic.c
 *
 *  (C) 1991  Linus Torvalds
 */

/**
 * 该函数在整个内核中使用(包括在头文件*.h, 内存管理程序mm和
 * 文件系统fs中)，用以指出主要的出错问题.
 */
#include <linux/kernel.h>
#include <linux/sched.h>

/* 实际上是整型int(fs/buffer.c). */
void sys_sync(void);

/**
 * 该函数用来显示内核中出现的重大错误信息，并运行文件系统同步函数，
 * 然后进入死循环 -- 死机。
 * 如果当前进程是任务0的话，还说明是交换任务出错，并且还没有运行
 * 文件系统同步函数。
 */
volatile void panic(const char *s)
{
    printk("Kernel panic: %s\n\r", s);

    if (current == task[0])
        printk("In swapper task - not syncing\n\r");
    else
        sys_sync();

    for (;;)
        ;
}
