/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * 我们需要下面这些内嵌语句 - 从内核空间创建进程(forking)将导致没有写时复制
 * (COPY ON WRITE)(!!!), 直到一个执行 execve 调用。这对堆栈可能带来问题。
 * 处理的方法是在fork()调用之后不让main()使用任何堆栈。因此就不能有函数调用
 * - 这意味着fork也要使用内嵌的代码，否则我们在从fork()退出时就要使用堆栈了.
 *
 * 实际上只有 pause 和 fork 需要使用内嵌方式，以保证从 main()中不会弄乱堆栈，
 * 但是我们同时还定义了其它一些函数.
 */

/**
 * 是unistd.h中的内嵌宏代码。以嵌入汇编的形式调用Linux的系统调用中断0x80。
 * 该中断是所有系统调用的入口。该条语句实际上是int fork()创建进程系统调用.
 * syscall0名称中最后的0表示无参数，1表示1个参数.
 */
static inline _syscall0(int, fork)
    /* nt pause()系统调用：暂停进程的执行，直到收到一个信号. */
    static inline _syscall0(int, pause)
    /* int setup(void * BIOS)系统调用，仅用于linux初始化(仅在这个程序中被调用). */
    static inline _syscall1(int, setup, void *, BIOS)
    /* int sync()系统调用:更新文件系统. */
    static inline _syscall0(int, sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

    /* 静态字符串数组. */
    static char printbuf[1024];

/* 送格式化输出到一字符串中(kernel/vsprintf.c). */
extern int vsprintf();
/* 函数原形，初始化. */
extern void init(void);
/* 块设备初始化子程序(kernel/blk_drv/ll_rw_blk.c). */
extern void blk_dev_init(void);
/* 字符设备初始化(kernel/chr_drv/tty_io.c). */
extern void chr_dev_init(void);
/* 硬盘初始化程序(kernel/blk_drv/hd.c). */
extern void hd_init(void);
/* 软驱初始化程序(kernel/blk_drv/floppy.c). */
extern void floppy_init(void);
/* 内存管理初始化(mm/memory.c). */
extern void mem_init(long start, long end);
/* 虚拟盘初始化(kernel/blk_drv/ramdisk.c). */
extern long rd_init(long mem_start, int length);
/* 建立内核时间(秒). */
extern long kernel_mktime(struct tm *tm);
/* 内核启动时间(开机时间)(秒). */
extern long startup_time;

/*
 * 以下这些数据是由setup.s程序在引导时间设置的.
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

/**
 * 这段宏读取CMOS实时时钟信息.
 * 0x70 是写端口号，0x80|addr是要读取的CMOS内存地址,
 * 0x71 是读端口号.
 */
#define CMOS_READ(addr) ({     \
    outb_p(0x80 | addr, 0x70); \
    inb_p(0x71);               \
})

/* 将BCD码转换成数字. */
#define BCD_TO_BIN(val) ((val) = ((val)&15) + ((val) >> 4) * 10)

/**
 * 该子程序取CMOS时钟，并设置开机时间??startup_time(秒).
 */
static void time_init(void)
{
    struct tm time;

    do
    {
        time.tm_sec = CMOS_READ(0);
        time.tm_min = CMOS_READ(2);
        time.tm_hour = CMOS_READ(4);
        time.tm_mday = CMOS_READ(7);
        time.tm_mon = CMOS_READ(8);
        time.tm_year = CMOS_READ(9);
    } while (time.tm_sec != CMOS_READ(0));

    BCD_TO_BIN(time.tm_sec);
    BCD_TO_BIN(time.tm_min);
    BCD_TO_BIN(time.tm_hour);
    BCD_TO_BIN(time.tm_mday);
    BCD_TO_BIN(time.tm_mon);
    BCD_TO_BIN(time.tm_year);

    time.tm_mon--;
    startup_time = kernel_mktime(&time);
}

/* 机器具有的内存(字节数). */
static long memory_end = 0;
/* 高速缓冲区末端地址. */
static long buffer_memory_end = 0;
/* 主内存(将用于分页)开始的位置. */
static long main_memory_start = 0;

struct drive_info
{
    char dummy[32];
} drive_info;

/**
 * 这里确实是void，并没错。在startup程序(head.s)中就是这样假设的.
 */
void main(void)
{
    /**
     * 此时中断仍被禁止着，做完必要的设置后就将其开启。
     * 下面这段代码用于保存：
     * 根设备号 ??ROOT_DEV； 高速缓存末端地址??buffer_memory_end；
     * 机器内存数??memory_end；主内存开始地址 ??main_memory_start；
     */
    ROOT_DEV = ORIG_ROOT_DEV;
    drive_info = DRIVE_INFO;

    /* 内存大小=1Mb字节 + 扩展内存(k)*1024字节. */
    memory_end = (1 << 20) + (EXT_MEM_K << 10);

    /* 忽略不到4Kb（1 页）的内存数. */
    memory_end &= 0xfffff000;

    /* 如果内存超过16Mb，则按16Mb计. */
    if (memory_end > 16 * 1024 * 1024)
        memory_end = 16 * 1024 * 1024;

    /* 如果内存>12Mb，则设置缓冲区末端=4Mb. */
    if (memory_end > 12 * 1024 * 1024)
        buffer_memory_end = 4 * 1024 * 1024;
    /* 否则如果内存>6Mb，则设置缓冲区末端=2Mb. */
    else if (memory_end > 6 * 1024 * 1024)
        buffer_memory_end = 2 * 1024 * 1024;
    /* 否则则设置缓冲区末端=1Mb. */
    else
        buffer_memory_end = 1 * 1024 * 1024;

    /* 主内存起始位置=缓冲区末端. */
    main_memory_start = buffer_memory_end;

#ifdef RAMDISK /* 如果定义了虚拟盘，则主内存将减少. */
    main_memory_start += rd_init(main_memory_start, RAMDISK * 1024);
#endif

    /* 以下是内核进行所有方面的初始化工作. */
    mem_init(main_memory_start, memory_end);

    /* 陷阱门(硬件中断向量)初始化。(kernel/traps.c). */
    trap_init();

    /* 块设备初始化。(kernel/blk_dev/ll_rw_blk.c). */
    blk_dev_init();

    /* 字符设备初始化。(kernel/chr_dev/tty_io.c). */
    chr_dev_init();

    /* tty初始化。(kernel/chr_dev/tty_io.c). */
    tty_init();

    /* 设置开机启动时间??startup_time. */
    time_init();

    /* 调度程序初始化(加载了任务0的tr, ldtr)。(kernel/sched.c). */
    sched_init();

    /* 缓冲管理初始化，建内存链表等。(fs/buffer.c). */
    buffer_init(buffer_memory_end);

    /* 硬盘初始化。(kernel/blk_dev/hd.c). */
    hd_init();

    /* 软驱初始化。(kernel/blk_dev/floppy.c). */
    floppy_init();

    /* 所有初始化工作都做完了，开启中断. */
    sti();

    /* 下面过程通过在堆栈中设置的参数，利用中断返回指令切换到任务0. */
    /* 移到用户模式。(include/asm/system.h). */
    move_to_user_mode();

    /* we count on this going ok. */
    if (!fork())
    {
        init();
    }

    /*
     * 注意!! 对于任何其它的任务，'pause()'将意味着我们必须等待收到一个信号才会返
     * 回就绪运行态，但任务 0（task0）是唯一的意外情况（参见'schedule()'），因为
     * 任务 0 在任何空闲时间里都会被激活（当没有其它任务在运行时），因此对于任务0
     * 'pause()'仅意味着我们返回来查看是否有其它任务可以运行，如果没有的话我们就回
     * 到这里，一直循环执行'pause()'.
     */
    for (;;)
        pause();
}

/**
 * 产生格式化信息并输出到标准输出设备stdout(1)，这里是指屏幕上显示。参数'*fmt'指定输出将
 * 采用的格式，参见各种标准 C 语言书籍。该子程序正好是 vsprintf 如何使用的一个例子。
 * 该程序使用 vsprintf()将格式化的字符串放入 printbuf 缓冲区，然后用 write()将缓冲区的内容
 * 输出到标准设备(1--stdout).
 */
static int printf(const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    write(1, printbuf, i = vsprintf(printbuf, fmt, args));
    va_end(args);

    return i;
}

/* 调用执行程序时参数的字符串数组. */
static char *argv_rc[] = {"/bin/sh", NULL};
/* 调用执行程序时的环境字符串数组. */
static char *envp_rc[] = {"HOME=/", NULL};

/* 同上. */
static char *argv[] = {"-/bin/sh", NULL};
static char *envp[] = {"HOME=/usr/root", NULL};

void init(void)
{
    int pid, i;

    /**
     * 读取硬盘参数包括分区表信息并建立虚拟盘和安装根文件系统设备.
     * 该函数是宏定义，对应函数是sys_setup()，在 kernel/blk_drv/hd.c.
     */
    setup((void *)&drive_info);

    /**
     * 用读写访问方式打开设备"/dev/tty0", 这里对应终端控制台.
     * 返回的句柄号0--stdin标准输入设备.
     */
    (void)open("/dev/tty0", O_RDWR, 0);

    /* 复制句柄，产生句柄1号--stdout标准输出设备. */
    (void)dup(0);

    /* 复制句柄，产生句柄2号--stderr标准出错输出设备. */
    (void)dup(0);

    /* 打印缓冲区块数和总字节数，每块1024字节. */
    printf("%d buffers = %d bytes buffer space\n\r",
           NR_BUFFERS, NR_BUFFERS * BLOCK_SIZE);

    /* 空闲内存字节数. */
    printf("Free mem: %d bytes\n\r", memory_end - main_memory_start);

    /**
     * 下面fork()用于创建一个子进程(子任务)。对于被创建的子进程，fork()将返回0值，
     * 对于原(父进程)将返回子进程的进程号。所以此处是子进程执行的内容。
     * 该子进程关闭了句柄0(stdin)，以只读方式打开/etc/rc文件，并执行/bin/sh程序，
     * 所带参数和环境变量分别由argv_rc和envp_rc数组给出.
     */
    if (!(pid = fork()))
    {
        close(0);

        if (open("/etc/rc", O_RDONLY, 0))
            _exit(1); // 如果打开文件失败，则退出(/lib/_exit.c)

        /* 装入/bin/sh 程序并执行. */
        execve("/bin/sh", argv_rc, envp_rc);

        /* 若execve()执行失败则退出(出错码2,"文件或目录不存在"). */
        _exit(2);
    }

    /**
     * 下面是父进程执行的语句。wait()是等待子进程停止或终止，其返回值应是子进程的进程号(pid)
     * 这三句的作用是父进程等待子进程的结束。&i是存放返回状态信息的位置。如果wait()返回值不
     * 等于子进程号，则继续等待.
     */
    if (pid > 0)
        while (pid != wait(&i))
            /* nothing */;

    /**
     * 如果执行到这里，说明刚创建的子进程的执行已停止或终止了。下面循环中首先再创建一个子进程，
     * 如果出错，则显示“初始化程序创建子进程失败”的信息并继续执行。对于所创建的子进程关闭所有
     * 以前还遗留的句柄(stdin, stdout, stderr)，新创建一个会话并设置进程组号，然后重新打开
     * /dev/tty0作为stdin，并复制成stdout和stderr。再次执行系统解释程序/bin/sh。但这次执行所选
     * 用的参数和环境数组另选了一套。然后父进程再次运行wait()等待。如果子进程又停止了执行，则在
     * 标准输出上显示出错信息"子进程pid停止了运行，返回码是i"，然后继续重试下去…，形成"大"死循环.
     */
    while (1)
    {
        if ((pid = fork()) < 0)
        {
            printf("Fork failed in init\r\n");
            continue;
        }

        if (!pid)
        {
            close(0);
            close(1);
            close(2);

            setsid();

            (void)open("/dev/tty0", O_RDWR, 0);
            (void)dup(0);
            (void)dup(0);

            _exit(execve("/bin/sh", argv, envp));
        }

        while (1)
            if (pid == wait(&i))
                break;

        printf("\n\rchild %d died with code %04x\n\r", pid, i);
        sync();
    }

    /* NOTE! _exit, not exit() */
    _exit(0);
}
