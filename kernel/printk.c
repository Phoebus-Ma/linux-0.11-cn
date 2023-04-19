/*
 *  linux/kernel/printk.c
 *
 *  (C) 1991  Linus Torvalds
 */

/**
 * 当处于内核模式时，我们不能使用printf，因为寄存器fs指向其它
 * 不感兴趣的地方。
 * 自己编制一个printf并在使用前保存fs，一切就解决了.
 */
#include <stdarg.h>
#include <stddef.h>

#include <linux/kernel.h>

static char buf[1024];

/* 下面该函数vsprintf()在linux/kernel/vsprintf.c. */
extern int vsprintf(char *buf, const char *fmt, va_list args);

/**
 * 内核使用的显示函数.
 */
int printk(const char *fmt, ...)
{
    /* va_list实际上是一个字符指针类型. */
    va_list args;
    int i;

    /* 参数处理开始函数。在(include/stdarg.h). */
    va_start(args, fmt);

    /* 使用格式串fmt将参数列表args输出到buf中。返回值i等于输出字符串的长度. */
    i = vsprintf(buf, fmt, args);

    /* 参数处理结束函数. */
    va_end(args);

    __asm__("push %%fs\n\t"         /* 保存fs. */
            "push %%ds\n\t"         /* 令fs = ds. */
            "pop %%fs\n\t"
            "pushl %0\n\t"          /* 将字符串长度压入堆栈(这三个入栈是调用参数). */
            "pushl $_buf\n\t"       /* 将buf的地址压入堆栈. */
            "pushl $0\n\t"          /* 将数值0压入堆栈。是通道号channel. */
            "call _tty_write\n\t"   /* 调用tty_write函数。(kernel/chr_drv/tty_io.c) */
            "addl $8,%%esp\n\t"     /* 跳过(丢弃)两个入栈参数(buf,channel). */
            "popl %0\n\t"           /* 弹出字符串长度值，作为返回值. */
            "pop %%fs"              /* 恢复原fs寄存器. */
            ::"r"(i) : "ax", "cx", "dx"); /* 通知编译器，寄存器ax,cx,dx值可能已经改变. */

    /* 返回字符串长度. */
    return i;
}
