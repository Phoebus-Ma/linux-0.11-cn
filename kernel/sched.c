/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/**
 * 'sched.c'是主要的内核文件。其中包括有关调度的基本函数
 * (sleep_on、wakeup、schedule 等)以及一些简单的系统调用函数
 * (比如 getpid()，仅从当前任务中获取一个字段).
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

/**
 * 取信号nr在信号位图中对应位的二进制数值。信号编号1-32.
 * 比如信号5的位图数值 = 1<<(5-1) = 16 = 00010000b.
 */
#define _S(nr)                  (1 << ((nr)-1))
/* 除了SIGKILL和SIGSTOP信号以外其它都是可阻塞的(…10111111111011111111b). */
#define _BLOCKABLE              (~(_S(SIGKILL) | _S(SIGSTOP)))

/**
 * 显示任务号 nr 的进程号、进程状态和内核堆栈空闲字节数(大约).
 */
void show_task(int nr, struct task_struct *p)
{
    int i, j = 4096 - sizeof(struct task_struct);

    printk("%d: pid=%d, state=%d, ", nr, p->pid, p->state);
    i = 0;

    /* 检测指定任务数据结构以后等于0的字节数. */
    while (i < j && !((char *)(p + 1))[i])
        i++;

    printk("%d (of %d) chars free in kernel stack\n\r", i, j);
}

/**
 * 显示所有任务的任务号、进程号、进程状态和内核堆栈空闲字节数(大约).
 */
void show_stat(void)
{
    int i;

    /* NR_TASKS 是系统能容纳的最大进程(任务)数量(64个). */
    for (i = 0; i < NR_TASKS; i++)
        if (task[i])    /* 定义在include/kernel/sched.h. */
            show_task(i, task[i]);
}

/* 定义每个时间片的滴答数? */
#define LATCH                   (1193180 / HZ)

/* 没有任何地方定义和引用该函数. */
extern void mem_use(void);

/* 时钟中断处理程序(kernel/system_call.s). */
extern int timer_interrupt(void);
/* 系统调用中断处理程序(kernel/system_call.s). */
extern int system_call(void);

/* 定义任务联合(任务结构成员和stack字符数组程序成员). */
union task_union
{
    /**
     * 因为一个任务数据结构与其堆栈放在同一内存页中，所以
     * 从堆栈段寄存器ss可以获得其数据段选择符.
     */
    struct task_struct task;
    char stack[PAGE_SIZE];
};

/* 定义初始任务的数据(sched.h中). */
static union task_union init_task = {
    INIT_TASK,
};

/* 从开机开始算起的滴答数时间值(10ms/滴答). */
long volatile jiffies = 0;
/* 开机时间。从 1970:0:0:0 开始计时的秒数. */
long startup_time = 0;
/* 当前任务指针(初始化为初始任务). */
struct task_struct *current = &(init_task.task);
/* 使用过协处理器任务的指针. */
struct task_struct *last_task_used_math = NULL;

/* 定义任务指针数组. */
struct task_struct *task[NR_TASKS] = {
    &(init_task.task),
};

/* 定义系统堆栈指针，4K。指针指在最后一项. */
long user_stack[PAGE_SIZE >> 2];

/* 该结构用于设置堆栈ss:esp(数据段选择符，指针)，见 head.s */
struct
{
    long *a;
    short b;
} stack_start = {&user_stack[PAGE_SIZE >> 2], 0x10};

/**
 * 将当前协处理器内容保存到老协处理器状态数组中，并将当前任务的协处理器
 * 内容加载进协处理器.
 */

/**
 * 当任务被调度交换过以后，该函数用以保存原任务的协处理器状态(上下文)并恢复
 * 新调度进来的当前任务的协处理器执行状态.
 */
void math_state_restore()
{
    /* 如果任务没变则返回(上一个任务就是当前任务). */
    if (last_task_used_math == current)
        return;

    /* 在发送协处理器命令之前要先发WAIT指令. */
    __asm__("fwait");

    /* 如果上个任务使用了协处理器，则保存其状态. */
    if (last_task_used_math)
    {
        __asm__("fnsave %0" ::"m"(last_task_used_math->tss.i387));
    }

    /* 现在，last_task_used_math指向当前任务以备当前任务被交换出去时使用. */
    last_task_used_math = current;

    /* 如果当前任务用过协处理器，则恢复其状态. */
    if (current->used_math)
    {
        __asm__("frstor %0" ::"m"(current->tss.i387));
    }
    /* 否则的话说明是第一次使用，于是就向协处理器发初始化命令并设置使用了协处理器标志. */
    else
    {
        __asm__("fninit" ::);
        current->used_math = 1;
    }
}

/**
 * 'schedule()'是调度函数。这是个很好的代码！没有任何理由对它进行修改，因为它可以在
 * 所有的环境下工作(比如能够对IO-边界处理很好的响应等)。只有一件事值得留意，那就是
 * 这里的信号处理代码。
 *
 * 注意！！任务0是个闲置('idle')任务，只有当没有其它任务可以运行时才调用它。它不能被
 * 杀死，也不能睡眠。任务0中的状态信息'state'是从来不用的.
 */
void schedule(void)
{
    int i, next, c;
    /* 任务结构指针的指针. */
    struct task_struct **p;

    /* 检测alarm(进程的报警定时值)，唤醒任何已得到信号的可中断任务. */

    /* 从任务数组中最后一个任务开始检测alarm. */
    for (p = &LAST_TASK; p > &FIRST_TASK; --p)
        /* 如果任务的alarm时间已经过期(alarm<jiffies),则在信号位图中置SIGALRM信号,然后清alarm. */
        if (*p)
        {
            /* jiffies是系统从开机开始算起的滴答数(10ms/滴答)。定义在 sched.h. */
            if ((*p)->alarm && (*p)->alarm < jiffies)
            {
                (*p)->signal |= (1 << (SIGALRM - 1));
                (*p)->alarm = 0;
            }

            /**
             * 如果信号位图中除被阻塞的信号外还有其它信号，并且任务处于可中断状态，则置任务
             * 为就绪状态其中'~(_BLOCKABLE & (*p)->blocked)'用于忽略被阻塞的信号，但
             * SIGKILL和SIGSTOP不能被阻塞.
             */
            if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
                (*p)->state == TASK_INTERRUPTIBLE)
                /* 置为就绪(可执行)状态. */
                (*p)->state = TASK_RUNNING;
        }

    /* 这里是调度程序的主要部分: */

    while (1)
    {
        c = -1;
        next = 0;
        i = NR_TASKS;
        p = &task[NR_TASKS];

        /**
         * 这段代码也是从任务数组的最后一个任务开始循环处理，并跳过不含任务的数组槽。
         * 比较每个就绪状态任务的counter(任务运行时间的递减滴答计数)值，哪一个值大，
         * 运行时间还不长，next就指向哪个的任务号.
         */
        while (--i)
        {
            if (!*--p)
                continue;

            if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
                c = (*p)->counter, next = i;
        }

        /* 如果比较得出有counter值大于0的结果，则退出开始的循环，执行任务切换. */
        if (c)
            break;

        /**
         * 否则就根据每个任务的优先权值，更新每一个任务的counter值，然后回到重新比较。
         * counter值的计算方式为counter = counter / 2 + priority.
         */
        for (p = &LAST_TASK; p > &FIRST_TASK; --p)
            if (*p)
                (*p)->counter = ((*p)->counter >> 1) +
                                (*p)->priority;
    }

    /* 切换到任务号为next的任务，并运行. */
    switch_to(next);
}

/**
 * pause()系统调用。转换当前任务的状态为可中断的等待状态，并重新调度。
 * 该系统调用将导致进程进入睡眠状态，直到收到一个信号。该信号用于终止进程或者使
 * 进程调用一个信号捕获函数。只有当捕获了一个信号，并且信号捕获处理函数返回，
 * pause()才会返回。此时pause()返回值应该是-1，并且errno被置为 EINTR。
 * 这里还没有完全实现(直到0.95版).
 */
int sys_pause(void)
{
    current->state = TASK_INTERRUPTIBLE;
    schedule();

    return 0;
}

/**
 * 把当前任务置为不可中断的等待状态，并让睡眠队列头的指针指向当前任务。
 * 只有明确地唤醒时才会返回。该函数提供了进程与中断处理程序之间的同步机制。
 * 函数参数*p是放置等待任务的队列头指针。
 */
void sleep_on(struct task_struct **p)
{
    struct task_struct *tmp;

    /* 若指针无效，则退出. */
    if (!p)
        return;

    /* 如果当前任务是任务0，则死机(impossible!). */
    if (current == &(init_task.task))
        panic("task[0] trying to sleep");

    /* 让tmp指向已经在等待队列上的任务(如果有的话). */
    tmp = *p;

    /* 将睡眠队列头的等待指针指向当前任务. */
    *p = current;

    /* 将当前任务置为不可中断的等待状态. */
    current->state = TASK_UNINTERRUPTIBLE;

    /* 重新调度. */
    schedule();

    /**
     * 只有当这个等待任务被唤醒时，调度程序才又返回到这里，则表示进程已被明确地唤醒。
     * 既然大家都在等待同样的资源，那么在资源可用时，就有必要唤醒所有等待该资源的进程。
     * 该函数嵌套调用，也会嵌套唤醒所有等待该资源的进程。然后系统会根据这些进程的优先
     * 条件，重新调度应该由哪个进程首先使用资源。也即让这些进程竞争上岗.
     * 若还存在等待的任务，则也将其置为就绪状态(唤醒).
     */
    if (tmp)
        tmp->state = 0;
}

/**
 * 将当前任务置为可中断的等待状态，并放入*p指定的等待队列中.
 */
void interruptible_sleep_on(struct task_struct **p)
{
    struct task_struct *tmp;

    if (!p)
        return;

    if (current == &(init_task.task))
        panic("task[0] trying to sleep");

    tmp = *p;
    *p = current;

repeat:
    current->state = TASK_INTERRUPTIBLE;
    schedule();

    /**
     * 如果等待队列中还有等待任务，并且队列头指针所指向的任务不是当前任务时，
     * 则将该等待任务置为可运行的就绪状态，并重新执行调度程序。当指针*p 所指向
     * 的不是当前任务时，表示在当前任务被放入队列后，又有新的任务被插入等待队列中，
     * 因此，既然本任务是可中断的，就应该首先执行所有其它的等待任务.
     */
    if (*p && *p != current)
    {
        (**p).state = 0;
        goto repeat;
    }

    /**
     * 下面一句代码有误，应该是*p = tmp，让队列头指针指向其余等待任务，否则在当前
     * 任务之前插入等待队列的任务均被抹掉了.
     */
    *p = NULL;

    if (tmp)
        tmp->state = 0;
}

/**
 * 唤醒指定任务*p.
 */
void wake_up(struct task_struct **p)
{
    if (p && *p)
    {
        /* 置为就绪(可运行)状态. */
        (**p).state = 0;
        *p = NULL;
    }
}

/**
 * 好了，从这里开始是一些有关软盘的子程序，本不应该放在内核的主要部分中的。
 * 将它们放在这里是因为软驱需要一个时钟，而放在这里是最方便的办法.
 */
static struct task_struct *wait_motor[4] = {NULL, NULL, NULL, NULL};
static int mon_timer[4] = {0, 0, 0, 0};
static int moff_timer[4] = {0, 0, 0, 0};

/* 数字输出寄存器(初值：允许DMA和请求中断、启动FDC). */
unsigned char current_DOR = 0x0C;

/**
 * 指定软盘到正常运转状态所需延迟滴答数(时间).
 * nr -- 软驱号(0-3)，返回值为滴答数.
 */
int ticks_to_floppy_on(unsigned int nr)
{
    /* 当前选中的软盘号(kernel/blk_drv/floppy.c). */
    extern unsigned char selected;

    /* 所选软驱对应数字输出寄存器中启动马达比特位. */
    unsigned char mask = 0x10 << nr;

    /* 最多4个软驱. */
    if (nr > 3)
        panic("floppy_on: nr>3");

    moff_timer[nr] = 10000; /* 100 s = very big :-) */

    cli();                  /* use floppy_off to turn it off */

    mask |= current_DOR;

    /* 如果不是当前软驱，则首先复位其它软驱的选择位，然后置对应软驱选择位. */
    if (!selected)
    {
        mask &= 0xFC;
        mask |= nr;
    }

    /**
     * 如果数字输出寄存器的当前值与要求的值不同，则向FDC数字输出端口输出新值(mask)。
     * 并且如果要求启动的马达还没有启动，则置相应软驱的马达启动定时器值(HZ/2 = 0.5秒
     * 或 50 个滴答)。此后更新当前数字输出寄存器值current_DOR.
     */
    if (mask != current_DOR)
    {
        outb(mask, FD_DOR);

        if ((mask ^ current_DOR) & 0xf0)
            mon_timer[nr] = HZ / 2;
        else if (mon_timer[nr] < 2)
            mon_timer[nr] = 2;

        current_DOR = mask;
    }

    sti();

    return mon_timer[nr];
}

/**
 * 等待指定软驱马达启动所需时间.
 */
void floppy_on(unsigned int nr)
{
    /* 关中断. */
    cli();

    /**
     * 如果马达启动定时还没到，就一直把当前进程置为不可中断睡眠状态
     * 并放入等待马达运行的队列中.
     */
    while (ticks_to_floppy_on(nr))
        sleep_on(nr + wait_motor);

    /* 开中断. */
    sti();
}

/**
 * 置关闭相应软驱马达停转定时器(3秒).
 */
void floppy_off(unsigned int nr)
{
    moff_timer[nr] = 3 * HZ;
}

/**
 * 软盘定时处理子程序。更新马达启动定时值和马达关闭停转计时值。该子程序是
 * 在时钟定时中断中被调用，因此每一个滴答(10ms)被调用一次，更新马达开启或
 * 停转定时器的值。如果某一个马达停转定时到，则将数字输出寄存器马达启动位复位.
 */
void do_floppy_timer(void)
{
    int i;
    unsigned char mask = 0x10;

    for (i = 0; i < 4; i++, mask <<= 1)
    {
        /* 如果不是DOR指定的马达则跳过. */
        if (!(mask & current_DOR))
            continue;

        if (mon_timer[i])
        {
            /* 如果马达启动定时到则唤醒进程. */
            if (!--mon_timer[i])
                wake_up(i + wait_motor);
        }
        /* 如果马达停转定时到则,复位相应马达启动位，并更新数字输出寄存器. */
        else if (!moff_timer[i])
        {
            current_DOR &= ~mask;
            outb(current_DOR, FD_DOR);
        }
        else
            moff_timer[i]--;    /* 马达停转计时递减. */
    }
}

/* 最多可有64个定时器链表(64个任务). */
#define TIME_REQUESTS               64

/* 定时器链表结构和定时器数组. */
static struct timer_list
{
    long jiffies;               /* 定时滴答数. */
    void (*fn)();               /* 定时处理程序. */
    struct timer_list *next;    /* 下一个定时器. */
} timer_list[TIME_REQUESTS], *next_timer = NULL;

/**
 * 添加定时器。输入参数为指定的定时值(滴答数)和相应的处理程序指针.
 * jiffies – 以10毫秒计的滴答数；*fn() - 定时时间到时执行的函数.
 */
void add_timer(long jiffies, void (*fn)(void))
{
    struct timer_list *p;

    /* 如果定时处理程序指针为空，则退出. */
    if (!fn)
        return;

    cli();

    /* 如果定时值<=0，则立刻调用其处理程序。并且该定时器不加入链表中. */
    if (jiffies <= 0)
        (fn)();
    else
    {
        /* 从定时器数组中，找一个空闲项. */
        for (p = timer_list; p < timer_list + TIME_REQUESTS; p++)
            if (!p->fn)
                break;

        /* 如果已经用完了定时器数组，则系统崩溃. */
        if (p >= timer_list + TIME_REQUESTS)
            panic("No more time requests free");

        /* 向定时器数据结构填入相应信息。并链入链表头. */
        p->fn = fn;
        p->jiffies = jiffies;
        p->next = next_timer;
        next_timer = p;

        /**
         * 链表项按定时值从小到大排序。在排序时减去排在前面需要的滴答数，这样在
         * 处理定时器时只要查看链表头的第一项的定时是否到期即可。[[?? 这段程序
         * 好象没有考虑周全。如果新插入的定时器值 < 原来头一个定时器值时，也应该
         * 将所有后面的定时值均减去新的第 1 个的定时值。]]
         */
        while (p->next && p->next->jiffies < p->jiffies)
        {
            p->jiffies -= p->next->jiffies;
            fn = p->fn;
            p->fn = p->next->fn;
            p->next->fn = fn;
            jiffies = p->jiffies;
            p->jiffies = p->next->jiffies;
            p->next->jiffies = jiffies;
            p = p->next;
        }
    }

    sti();
}

/**
 * 时钟中断C函数处理程序，在kernel/system_call.s中的_timer_interrupt被调用。
 * 参数cpl是当前特权级0或3，0表示内核代码在执行。
 * 对于一个进程由于执行时间片用完时，则进行任务切换。并执行一个计时更新工作.
 */
void do_timer(long cpl)
{
    /* 扬声器发声时间滴答数(kernel/chr_drv/console.c). */
    extern int beepcount;
    /* 关闭扬声器(kernel/chr_drv/console.c). */
    extern void sysbeepstop(void);

    /**
     * 如果发声计数次数到，则关闭发声。(向0x61口发送命令，复位位0和1。位0控制
     * 8253计数器2的工作，位1控制扬声器).
     */
    if (beepcount)
        if (!--beepcount)
            sysbeepstop();

    /**
     * 如果当前特权级(cpl)为0(最高，表示是内核程序在工作)，则将超级用户运行时间
     * stime递增；如果cpl > 0，则表示是一般用户程序在工作，增加utime.
     */
    if (cpl)
        current->utime++;
    else
        current->stime++;

    /**
     * 如果有用户的定时器存在，则将链表第1个定时器的值减1。如果已等于0，则调用相
     * 应的处理程序，并将该处理程序指针置为空。然后去掉该项定时器.
     */
    /* next_timer是定时器链表的头指针. */
    if (next_timer)
    {
        next_timer->jiffies--;
        while (next_timer && next_timer->jiffies <= 0)
        {
            /* 这里插入了一个函数指针定义. */
            void (*fn)(void);

            fn = next_timer->fn;
            next_timer->fn = NULL;
            next_timer = next_timer->next;

            /* 调用处理函数. */
            (fn)();
        }
    }

    /* 如果当前软盘控制器FDC的数字输出寄存器中马达启动位有置位的，则执行软盘定时程序. */
    if (current_DOR & 0xf0)
        do_floppy_timer();

    /* 如果进程运行时间还没完，则退出. */
    if ((--current->counter) > 0)
        return;

    current->counter = 0;

    /* 对于超级用户程序，不依赖counter值进行调度. */
    if (!cpl)
        return;

    schedule();
}

/**
 * 系统调用功能 - 设置报警定时时间值(秒).
 * 如果已经设置过alarm值，则返回旧值，否则返回0.
 */
int sys_alarm(long seconds)
{
    int old = current->alarm;

    if (old)
        old = (old - jiffies) / HZ;
    current->alarm = (seconds > 0) ? (jiffies + HZ * seconds) : 0;

    return (old);
}

/**
 * 取当前进程号pid.
 */
int sys_getpid(void)
{
    return current->pid;
}

/**
 * 取父进程号ppid.
 */
int sys_getppid(void)
{
    return current->father;
}

/**
 * 取用户号uid.
 */
int sys_getuid(void)
{
    return current->uid;
}

/**
 * 取euid.
 */
int sys_geteuid(void)
{
    return current->euid;
}

/**
 * 取组号gid.
 */
int sys_getgid(void)
{
    return current->gid;
}

/**
 * 取egid.
 */
int sys_getegid(void)
{
    return current->egid;
}

/**
 * 系统调用功能 -- 降低对 CPU 的使用优先权.
 * 应该限制increment大于0，否则的话,可使优先权增大！！
 */
int sys_nice(long increment)
{
    if (current->priority - increment > 0)
        current->priority -= increment;

    return 0;
}

/**
 * 调度程序的初始化子程序.
 */
void sched_init(void)
{
    int i;
    /* 描述符表结构指针. */
    struct desc_struct *p;

    /* sigaction是存放有关信号状态的结构. */
    if (sizeof(struct sigaction) != 16)
        panic("Struct sigaction MUST be 16 bytes");

    /* 设置初始任务(任务0)的任务状态段描述符和局部数据表描述符(include/asm/system.h). */
    set_tss_desc(gdt + FIRST_TSS_ENTRY, &(init_task.task.tss));
    set_ldt_desc(gdt + FIRST_LDT_ENTRY, &(init_task.task.ldt));

    /* 清任务数组和描述符表项(注意i=1开始，所以初始任务的描述符还在). */
    p = gdt + 2 + FIRST_TSS_ENTRY;

    for (i = 1; i < NR_TASKS; i++)
    {
        task[i] = NULL;
        p->a = p->b = 0;
        p++;
        p->a = p->b = 0;
        p++;
    }

    /* 清除标志寄存器中的位NT，这样以后就不会有麻烦. */
    /**
     * NT标志用于控制程序的递归调用(Nested Task)。当NT置位时，那么当前中断任务执行
     * iret指令时就会引起任务切换。NT指出TSS中的back_link字段是否有效.
     */
    __asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");    /* 复位NT标志. */

    /* 将任务0的TSS加载到任务寄存器t. */
    ltr(0);

    /* 将局部描述符表加载到局部描述符表寄存器. */
    lldt(0);

    /**
     * 注意!!是将GDT中相应LDT描述符的选择符加载到ldtr。只明确加载这一次，以后新任务
     * LDT的加载，是CPU根据TSS中的LDT项自动加载.
     */

    /* 下面代码用于初始化8253定时器. */
    outb_p(0x36, 0x43);         /* binary, mode 3, LSB/MSB, ch 0 */
    outb_p(LATCH & 0xff, 0x40); /* LSB,定时值低字节. */
    outb(LATCH >> 8, 0x40);     /* MSB,定时值高字节. */

    /* 设置时钟中断处理程序句柄(设置时钟中断门). */
    set_intr_gate(0x20, &timer_interrupt);

    /* 修改中断控制器屏蔽码，允许时钟中断. */
    outb(inb_p(0x21) & ~0x01, 0x21);

    /* 设置系统调用中断门. */
    set_system_gate(0x80, &system_call);
}
