/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <signal.h>

/* 前面的限定符volatile要求编译器不要对其进行优化. */
volatile void do_exit(int error_code);

/**
 * 获取当前任务信号屏蔽位图(屏蔽码).
 */
int sys_sgetmask()
{
    return current->blocked;
}

/**
 * 设置新的信号屏蔽位图。SIGKILL不能被屏蔽。返回值是原信号屏蔽位图.
 */
int sys_ssetmask(int newmask)
{
    int old = current->blocked;

    current->blocked = newmask & ~(1 << (SIGKILL - 1));

    return old;
}

/**
 * 复制sigaction数据到fs数据段to处.
 */
static inline void save_old(char *from, char *to)
{
    int i;

    /* 验证to处的内存是否足够. */
    verify_area(to, sizeof(struct sigaction));

    for (i = 0; i < sizeof(struct sigaction); i++)
    {
        /* 复制到fs段。一般是用户数据段. */
        put_fs_byte(*from, to);
        from++;
        to++;
    }
}

/**
 * 把sigaction数据从fs数据段from位置复制到to处.
 */
static inline void get_new(char *from, char *to)
{
    int i;

    for (i = 0; i < sizeof(struct sigaction); i++)
        *(to++) = get_fs_byte(from++);
}

/**
 * signal()系统调用。类似于sigaction()。为指定的信号安装新的信号句柄
 * (信号处理程序)。信号句柄可以是用户指定的函数，也可以是SIG_DFL(默认
 * 句柄)或SIG_IGN(忽略)。参数signum - 指定的信号；handler - 指定的句柄；
 * restorer – 原程序当前执行的地址位置。函数返回原信号句柄.
 */
int sys_signal(int signum, long handler, long restorer)
{
    struct sigaction tmp;

    /* 信号值要在(1-32)范围内，并且不得是SIGKILL. */
    if (signum < 1 || signum > 32 || signum == SIGKILL)
        return -1;

    /* 指定的信号处理句柄. */
    tmp.sa_handler = (void (*)(int))handler;
    /* 执行时的信号屏蔽码. */
    tmp.sa_mask = 0;
    /* 该句柄只使用1次后就恢复到默认值，并允许信号在自己的处理句柄中收到. */
    tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
    /* 保存返回地址. */
    tmp.sa_restorer = (void (*)(void))restorer;
    handler = (long)current->sigaction[signum - 1].sa_handler;
    current->sigaction[signum - 1] = tmp;

    return handler;
}

/**
 * sigaction()系统调用。改变进程在收到一个信号时的操作。signum是除了SIGKILL
 * 以外的任何信号。[如果新操作(action)不为空]则新操作被安装。如果oldaction
 * 指针不为空，则原操作被保留到oldaction。成功则返回0，否则为-1.
 */
int sys_sigaction(int signum, const struct sigaction *action,
                  struct sigaction *oldaction)
{
    struct sigaction tmp;

    /* 信号值要在(1-32)范围内，并且信号SIGKILL的处理句柄不能被改变. */
    if (signum < 1 || signum > 32 || signum == SIGKILL)
        return -1;

    /* 在信号的sigaction结构中设置新的操作(动作). */
    tmp = current->sigaction[signum - 1];

    get_new((char *)action,
            (char *)(signum - 1 + current->sigaction));

    /* 如果oldaction指针不为空的话，则将原操作指针保存到oldaction所指的位置. */
    if (oldaction)
        save_old((char *)&tmp, (char *)oldaction);

    /* 如果允许信号在自己的信号句柄中收到，则令屏蔽码为0，否则设置屏蔽本信号. */
    if (current->sigaction[signum - 1].sa_flags & SA_NOMASK)
        current->sigaction[signum - 1].sa_mask = 0;
    else
        current->sigaction[signum - 1].sa_mask |= (1 << (signum - 1));

    return 0;
}

/**
 * 系统调用中断处理程序中真正的信号处理程序(在kernel/system_call.s).
 * 该段代码的主要作用是将信号的处理句柄插入到用户程序堆栈中，并在本系统
 * 调用结束返回后立刻执行信号句柄程序，然后继续执行用户的程序.
 */
void do_signal(long signr, long eax, long ebx, long ecx, long edx,
               long fs, long es, long ds,
               long eip, long cs, long eflags,
               unsigned long *esp, long ss)
{
    unsigned long sa_handler;
    long old_eip = eip;
    struct sigaction *sa = current->sigaction + signr - 1;
    int longs;
    unsigned long *tmp_esp;

    sa_handler = (unsigned long)sa->sa_handler;

    /**
     * 如果信号句柄为SIG_IGN(忽略)，则返回；如果句柄为SIG_DFL(默认处理)，
     * 则如果信号是SIGCHLD则返回，否则终止进程的执行.
     */
    if (sa_handler == 1)
        return;

    if (!sa_handler)
    {
        if (signr == SIGCHLD)
            return;
        else
            /**
             * [?? 为什么以信号位图为参数?不为什么!??]
             * 这里应该是do_exit(1 << signr)).
             */
            do_exit(1 << (signr - 1));
    }

    /* 如果该信号句柄只需使用一次，则将该句柄置空(该信号句柄已经保存在sa_handler指针中). */
    if (sa->sa_flags & SA_ONESHOT)
        sa->sa_handler = NULL;

    /**
     * 下面这段代码将信号处理句柄插入到用户堆栈中，同时也将sa_restorer,signr,
     * 进程屏蔽码(如果SA_NOMASK没置位),eax,ecx,edx 作为参数以及原调用系统调用
     * 的程序返回指针及标志寄存器值压入堆栈。因此在本次系统调用中断(0x80)返回用户
     * 程序时会首先执行用户的信号句柄程序，然后再继续执行用户程序。
     * 将用户调用系统调用的代码指针eip指向该信号处理句柄.
     */
    *(&eip) = sa_handler;

    /* 如果允许信号自己的处理句柄收到信号自己，则也需要将进程的阻塞码压入堆栈. */
    longs = (sa->sa_flags & SA_NOMASK) ? 7 : 8;

    /**
     * 将原调用程序的用户的堆栈指针向下扩展7(或8)个长字(用来存放调用信号句柄的参数等)，
     * 并检查内存使用情况(例如如果内存超界则分配新页等).
     */
    *(&esp) -= longs;
    verify_area(esp, longs * 4);

    /**
     * 在用户堆栈中从下到上存放sa_restorer, 信号signr, 屏蔽码blocked(如果SA_NOMASK置位),
     * eax, ecx, edx, eflags 和用户程序原代码指针.
     */
    tmp_esp = esp;

    put_fs_long((long)sa->sa_restorer, tmp_esp++);
    put_fs_long(signr, tmp_esp++);

    if (!(sa->sa_flags & SA_NOMASK))
        put_fs_long(current->blocked, tmp_esp++);

    put_fs_long(eax, tmp_esp++);
    put_fs_long(ecx, tmp_esp++);
    put_fs_long(edx, tmp_esp++);
    put_fs_long(eflags, tmp_esp++);
    put_fs_long(old_eip, tmp_esp++);

    /* 进程阻塞码(屏蔽码)添上sa_mask中的码位. */
    current->blocked |= sa->sa_mask;
}
