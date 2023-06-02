/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/**
 * 本程序是底层硬盘中断辅助程序。主要用于扫描请求列表，使用中断在函数之间跳转。
 * 由于所有的函数都是在中断里调用的，所以这些函数不可以睡眠。请特别注意。
 *
 * 由Drew Eckhardt修改，利用CMOS信息检测硬盘数.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

/* 硬盘主设备号是3. */
#define MAJOR_NR                3

#include "blk.h"

/* 读CMOS参数宏函数. */
#define CMOS_READ(addr) ({     \
    outb_p(0x80 | addr, 0x70); \
    inb_p(0x71);               \
})

/* Max read/write errors/sector */
#define MAX_ERRORS              7       /* 读/写一个扇区时允许的最多出错次数. */
#define MAX_HD                  2       /* 系统支持的最多硬盘数. */

/* 硬盘中断程序在复位操作时会调用的重新校正函数. */
static void recal_intr(void);

static int recalibrate = 1;             /* 重新校正标志. */
static int reset = 1;                   /* 复位标志. */

/*
 * 下面结构定义了硬盘参数及类型.
 */

/* 各字段分别是磁头数、每磁道扇区数、柱面数、写前预补偿柱面号、磁头着陆区柱面号、控制字节. */
struct hd_i_struct
{
    int head, sect, cyl, wpcom, lzone, ctl;
};

/* 如果已经在include/linux/config.h中定义了HD_TYPE… */
#ifdef HD_TYPE
/* 取定义好的参数作为hd_info[]的数据. */
struct hd_i_struct hd_info[] = {HD_TYPE};
/* 计算硬盘数. */
#define NR_HD ((sizeof(hd_info)) / (sizeof(struct hd_i_struct)))
#else   /* 否则，都设为0值. */
struct hd_i_struct hd_info[] = {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}};
static int NR_HD = 0;
#endif

/**
 * 定义硬盘分区结构。给出每个分区的物理起始扇区号、分区扇区总数.
 * 其中5的倍数处的项(例如hd[0]和 hd[5]等)代表整个硬盘中的参数.
 */
static struct hd_struct
{
    long start_sect;
    long nr_sects;
} hd[5 * MAX_HD] = {
    {0, 0},
};

/* 读端口port，共读nr字，保存在buf中. */
#define port_read(port, buf, nr)                          \
    __asm__("cld;rep;insw" ::"d"(port), "D"(buf), "c"(nr) \
            : "cx", "di")

/* 写端口port，共写nr字，从buf中取数据. */
#define port_write(port, buf, nr)                          \
    __asm__("cld;rep;outsw" ::"d"(port), "S"(buf), "c"(nr) \
            : "cx", "si")

extern void hd_interrupt(void);
extern void rd_load(void);

/* 下面该函数只在初始化时被调用一次。用静态变量callable作为可调用标志. */
/**
 * 该函数的参数由初始化程序init/main.c的init子程序设置为指向0x90080处，
 * 此处存放着setup.s程序从BIOS取得的2个硬盘的基本参数表(32字节)。硬盘参数
 * 表信息参见下面列表后的说明.
 * 本函数主要功能是读取CMOS和硬盘参数表信息，用于设置硬盘分区结构hd，并加载
 * RAM虚拟盘和根文件系统.
 */
int sys_setup(void *BIOS)
{
    static int callable = 1;
    int i, drive;
    unsigned char cmos_disks;
    struct partition *p;
    struct buffer_head *bh;

    /* 初始化时callable=1，当运行该函数时将其设置为0，使本函数只能执行一次. */
    if (!callable)
        return -1;

    callable = 0;

    /* 如果没有在config.h中定义硬盘参数，就从0x90080处读入. */
#ifndef HD_TYPE
    for (drive = 0; drive < 2; drive++)
    {
        hd_info[drive].cyl  = *(unsigned short *)BIOS;          /* 柱面数. */
        hd_info[drive].head = *(unsigned char *)(2 + BIOS);     /* 磁头数. */
        hd_info[drive].wpcom= *(unsigned short *)(5 + BIOS);    /* 写前预补偿柱面号. */
        hd_info[drive].ctl  = *(unsigned char *)(8 + BIOS);     /* 控制字节. */
        hd_info[drive].lzone= *(unsigned short *)(12 + BIOS);   /* 磁头着陆区柱面号. */
        hd_info[drive].sect = *(unsigned char *)(14 + BIOS);    /* 每磁道扇区数. */
        BIOS += 16;         /* 每个硬盘的参数表长 16 字节，这里 BIOS 指向下一个表. */
    }

    /**
     * setup.s程序在取BIOS中的硬盘参数表信息时，如果只有1个硬盘，就会将对应
     * 第2个硬盘的16字节全部清零。因此这里只要判断第2个硬盘柱面数是否为0就可以
     * 知道有没有第2个硬盘了.
     */
    if (hd_info[1].cyl)
        NR_HD = 2;      /* 硬盘数置为2. */
    else
        NR_HD = 1;
#endif

    /* 设置每个硬盘的起始扇区号和扇区总数。其中编号i*5含义参见本程序后的有关说明. */
    for (i = 0; i < NR_HD; i++)
    {
        /* 硬盘起始扇区号. */
        hd[i * 5].start_sect = 0;

        /* 硬盘总扇区数. */
        hd[i * 5].nr_sects = hd_info[i].head *
                             hd_info[i].sect * hd_info[i].cyl;
    }

    /**
     * 我们对CMOS有关硬盘的信息有些怀疑：可能会出现这样的情况，我们有一块SCSI/ESDI/
     * 等的控制器，它是以ST-506方式与BIOS兼容的，因而会出现在我们的BIOS参数表中，
     * 但却又不是寄存器兼容的，因此这些参数在CMOS中又不存在.
     * 
     * 另外，我们假设ST-506驱动器(如果有的话)是系统中的基本驱动器，也即以驱动器1或2
     * 出现的驱动器。
     * 
     * 第1个驱动器参数存放在CMOS字节0x12的高半字节中，第2个存放在低半字节中。
     * 该4位字节信息可以是驱动器类型，也可能仅是0xf。0xf表示使用CMOS中0x19字节作为
     * 驱动器1的8位类型字节，使用CMOS中0x1A字节作为驱动器2的类型字节。
     * 
     * 总之，一个非零值意味着我们有一个AT控制器硬盘兼容的驱动器.
     */

    /* 这里根据上述原理来检测硬盘到底是否是AT控制器兼容的. */
    if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
        if (cmos_disks & 0x0f)
            NR_HD = 2;
        else
            NR_HD = 1;
    else
        NR_HD = 0;

    /**
     * 若NR_HD=0，则两个硬盘都不是AT控制器兼容的，硬盘数据结构清零。
     * 若NR_HD=1，则将第2个硬盘的参数清零.
     */
    for (i = NR_HD; i < 2; i++)
    {
        hd[i * 5].start_sect = 0;
        hd[i * 5].nr_sects = 0;
    }

    /**
     * 读取每一个硬盘上第1块数据(第1个扇区有用)，获取其中的分区表信息。
     * 首先利用函数 bread()读硬盘第1块数据(fs/buffer.c,267)，参数中的0x300是
     * 硬盘的主设备号然后根据硬盘头1个扇区位置 0x1fe 处的两个字节是否为'55AA'来
     * 判断该扇区中位于0x1BE开始的分区表是否有效。最后将分区表信息放入硬盘分区数据
     * 结构hd中.
     */
    for (drive = 0; drive < NR_HD; drive++)
    {
        /* 0x300, 0x305逻辑设备号. */
        if (!(bh = bread(0x300 + drive * 5, 0)))
        {
            printk("Unable to read partition table of drive %d\n\r",
                   drive);
            panic("");
        }

        /* 判断硬盘信息有效标志'55AA'. */
        if (bh->b_data[510] != 0x55 || 
                (unsigned char)bh->b_data[511] != 0xAA)
        {
            printk("Bad partition table on drive %d\n\r", drive);
            panic("");
        }

        /* 分区表位于硬盘第1扇区的0x1BE处. */
        p = 0x1BE + (void *)bh->b_data;

        for (i = 1; i < 5; i++, p++)
        {
            hd[i + 5 * drive].start_sect = p->start_sect;
            hd[i + 5 * drive].nr_sects = p->nr_sects;
        }

        /* 释放为存放硬盘块而申请的内存缓冲区页. */
        brelse(bh);
    }

    /* 如果有硬盘存在并且已读入分区表，则打印分区表正常信息. */
    if (NR_HD)
        printk("Partition table%s ok.\n\r", (NR_HD > 1) ? "s" : "");

    /* 加载(创建)RAMDISK(kernel/blk_drv/ramdisk.c). */
    rd_load();

    /* 安装根文件系统(fs/super.c). */
    mount_root();

    return (0);
}

/**
 * 判断并循环等待驱动器就绪。
 * 读硬盘控制器状态寄存器端口HD_STATUS(0x1f7)，并循环检测驱动器就绪比特位和控制器忙位。
 */
static int controller_ready(void)
{
    int retries = 10000;

    while (--retries && (inb_p(HD_STATUS) & 0xc0) != 0x40)
        ;

    /* 返回等待循环的次数. */
    return (retries);
}

/**
 * 检测硬盘执行命令后的状态。(win_表示温切斯特硬盘的缩写).
 * 读取状态寄存器中的命令执行结果状态。返回0表示正常，1出错。
 * 如果执行命令错，则再读错误寄存器HD_ERROR(0x1f1).
 */
static int win_result(void)
{
    /* 取状态信息. */
    int i = inb_p(HD_STATUS);

    if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT)) == (READY_STAT | SEEK_STAT))
        return (0); /* ok */

    if (i & 1)
        i = inb(HD_ERROR);  /* 若ERR_STAT置位，则读取错误寄存器. */

    return (1);
}

/**
 * 向硬盘控制器发送命令块.
 * 调用参数：
 * drive - 硬盘号(0-1)；
 * nsect - 读写扇区数；
 * sect - 起始扇区；
 * head - 磁头号；
 * cyl - 柱面号；
 * cmd - 命令码；
 * *intr_addr() - 硬盘中断处理程序中将调用的C处理函数.
 */
static void hd_out(unsigned int drive, unsigned int nsect, unsigned int sect,
                   unsigned int head, unsigned int cyl, unsigned int cmd,
                   void (*intr_addr)(void))
{
    /* port变量对应寄存器dx. */
    register int port asm("dx");

    /* 如果驱动器号(0,1)>1或磁头号>15，则程序不支持. */
    if (drive > 1 || head > 15)
        panic("Trying to write bad sector");

    /* 如果等待一段时间后仍未就绪则出错，死机. */
    if (!controller_ready())
        panic("HD controller not ready");

    /* do_hd 函数指针将在硬盘中断程序中被调用. */
    do_hd = intr_addr;

    /* 向控制寄存器(0x3f6)输出控制字节. */
    outb_p(hd_info[drive].ctl, HD_CMD);

    /* 置dx为数据寄存器端口(0x1f0). */
    port = HD_DATA;

    /* 参数：写预补偿柱面号(需除 4). */
    outb_p(hd_info[drive].wpcom >> 2, ++port);

    /* 参数：读/写扇区总数. */
    outb_p(nsect, ++port);

    /* 参数：起始扇区. */
    outb_p(sect, ++port);

    /* 参数：柱面号低8位. */
    outb_p(cyl, ++port);

    /* 参数：柱面号高8位. */
    outb_p(cyl >> 8, ++port);

    /* 参数：驱动器号+磁头号. */
    outb_p(0xA0 | (drive << 4) | head, ++port);

    /* 命令：硬盘控制命令. */
    outb(cmd, ++port);
}

/**
 * 等待硬盘就绪。也即循环等待主状态控制器忙标志位复位。若仅有就绪或寻道
 * 结束标志置位，则成功，返回0。若经过一段时间仍为忙，则返回1.
 */
static int drive_busy(void)
{
    unsigned int i;

    /* 循环等待就绪标志位置位. */
    for (i = 0; i < 10000; i++)
        if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT | READY_STAT)))
            break;

    /* 再取主控制器状态字节. */
    i = inb(HD_STATUS);

    /* 检测忙位、就绪位和寻道结束位. */
    i &= BUSY_STAT | READY_STAT | SEEK_STAT;

    /* 若仅有就绪或寻道结束标志，则返回0. */
    if (i == READY_STAT | SEEK_STAT)
        return (0);

    /* 否则等待超时，显示信息。并返回1. */
    printk("HD controller times out\n\r");

    return (1);
}

/**
 * 诊断复位(重新校正)硬盘控制器.
 */
static void reset_controller(void)
{
    int i;

    /* 向控制寄存器端口发送控制字节(4-复位). */
    outb(4, HD_CMD);

    for (i = 0; i < 100; i++)
        nop();      /* 等待一段时间(循环空操作). */

    /* 再发送正常的控制字节(不禁止重试、重读). */
    outb(hd_info[0].ctl & 0x0f, HD_CMD);

    /* 若等待硬盘就绪超时，则显示出错信息. */
    if (drive_busy())
        printk("HD-controller still busy\n\r");

    /* 取错误寄存器，若不等于1(无错误)则出错. */
    if ((i = inb(HD_ERROR)) != 1)
        printk("HD-controller reset failed: %02x\n\r", i);
}

/**
 * 复位硬盘nr。首先复位(重新校正)硬盘控制器。然后发送硬盘控制器命令"建立驱动
 * 器参数"，其中recal_intr()是在硬盘中断处理程序中调用的重新校正处理函数.
 */
static void reset_hd(int nr)
{
    reset_controller();
    hd_out(nr, hd_info[nr].sect, hd_info[nr].sect, hd_info[nr].head - 1,
           hd_info[nr].cyl, WIN_SPECIFY, &recal_intr);
}

/**
 * 意外硬盘中断调用函数。
 * 发生意外硬盘中断时，硬盘中断处理程序中调用的默认C处理函数。在被调用函数指针
 * 为空时调用该函数。参见(kernel/system_call.s).
 */
void unexpected_hd_interrupt(void)
{
    printk("Unexpected HD interrupt\n\r");
}

/**
 * 读写硬盘失败处理调用函数.
 */
static void bad_rw_intr(void)
{
    /**
     * 如果读扇区时的出错次数大于或等于7次时，则结束请求并唤醒等待该请求的进程，
     * 而且对应缓冲区更新标志复位(没有更新).
     */
    if (++CURRENT->errors >= MAX_ERRORS)
        end_request(0);

    /**
     * 如果读一扇区时的出错次数已经大于3次，则要求执行复位硬盘控制器操作.
     */
    if (CURRENT->errors > MAX_ERRORS / 2)
        reset = 1;
}

/**
 * 读操作中断调用函数。将在执行硬盘中断处理程序中被调用.
 */
static void read_intr(void)
{
    /* 若控制器忙、读写错或命令执行错. */
    if (win_result())
    {
        /* 则进行读写硬盘失败处理. */
        bad_rw_intr();
        /* 然后再次请求硬盘作相应(复位)处理. */
        do_hd_request();

        return;
    }

    /* 将数据从数据寄存器口读到请求结构缓冲区. */
    port_read(HD_DATA, CURRENT->buffer, 256);

    CURRENT->errors = 0;            /* 清出错次数. */
    CURRENT->buffer += 512;         /* 调整缓冲区指针，指向新的空区. */
    CURRENT->sector++;              /* 起始扇区号加1. */

    /**
     * 如果所需读出的扇区数还没有读完，则再次置硬盘调用C函数指针为
     * read_intr()因为硬盘中断处理程序每次调用do_hd时都会将该函数指针置空。
     * 参见system_call.s.
     */
    if (--CURRENT->nr_sectors)
    {
        do_hd = &read_intr;
        return;
    }

    /* 若全部扇区数据已经读完，则处理请求结束事宜. */
    end_request(1);

    /* 执行其它硬盘请求操作. */
    do_hd_request();
}

/**
 * 写扇区中断调用函数。在硬盘中断处理程序中被调用。
 * 在写命令执行后，会产生硬盘中断信号，执行硬盘中断处理程序，此时在硬盘中断处理
 * 程序中调用的C函数指针do_hd()已经指向write_intr()，因此会在写操作完成(或出错)后，
 * 执行该函.数
 */
static void write_intr(void)
{
    /* 如果硬盘控制器返回错误信息. */
    if (win_result())
    {
        /* 则首先进行硬盘读写失败处理. */
        bad_rw_intr();

        /* 然后再次请求硬盘作相应(复位)处理. */
        do_hd_request();

        /* 然后返回(也退出了此次硬盘中断). */
        return;
    }

    /* 否则将欲写扇区数减1，若还有扇区要写，则: */
    if (--CURRENT->nr_sectors)
    {
        CURRENT->sector++;          /* 当前请求起始扇区号+1. */
        CURRENT->buffer += 512;     /* 调整请求缓冲区指针. */
        do_hd = &write_intr;        /* 置硬盘中断程序调用函数指针为write_intr(). */

        /* 再向数据寄存器端口写256字节. */
        port_write(HD_DATA, CURRENT->buffer, 256);

        /* 返回等待硬盘再次完成写操作后的中断处理. */
        return;
    }

    /* 若全部扇区数据已经写完，则处理请求结束事宜. */
    end_request(1);

    /* 执行其它硬盘请求操作. */
    do_hd_request();
}

/**
 * 硬盘重新校正(复位)中断调用函数。在硬盘中断处理程序中被调用。
 * 如果硬盘控制器返回错误信息，则首先进行硬盘读写失败处理，然后
 * 请求硬盘作相应(复位)处理.
 */
static void recal_intr(void)
{
    if (win_result())
        bad_rw_intr();

    do_hd_request();
}

/**
 * 执行硬盘读写请求操作.
 */
void do_hd_request(void)
{
    int i, r;
    unsigned int block, dev;
    unsigned int sec, head, cyl;
    unsigned int nsect;

    /* 检测请求项的合法性(参见kernel/blk_drv/blk.h). */
    INIT_REQUEST;

    /* 取设备号中的子设备号。子设备号即是硬盘上的分区号. */
    dev = MINOR(CURRENT->dev);
    /* 请求的起始扇区. */
    block = CURRENT->sector;

    /**
     * 如果子设备号不存在或者起始扇区大于该分区扇区数-2，则结束该请求，
     * 并跳转到标号repeat处(定义在INIT_REQUEST开始处)。因为一次要求读写2个
     * 扇区(512*2字节)，所以请求的扇区号不能大于分区中最后倒数第二个扇区号.
     */
    if (dev >= 5 * NR_HD || block + 2 > hd[dev].nr_sects)
    {
        end_request(0);
        goto repeat;                /* 该标号在blk.h最后面. */
    }

    /* 将所需读的块对应到整个硬盘上的绝对扇区号. */
    block += hd[dev].start_sect;
    /* 此时dev代表硬盘号(0或1). */
    dev /= 5;

    /**
     * 下面嵌入汇编代码用来从硬盘信息结构中根据起始扇区号和每磁道扇区数计算在磁道中的
     * 扇区号(sec)、所在柱面号(cyl)和磁头号(head).
     */
    __asm__("divl %4"
            : "=a"(block), "=d"(sec)
            : "0"(block), "1"(0),
              "r"(hd_info[dev].sect));

    __asm__("divl %4"
            : "=a"(cyl), "=d"(head)
            : "0"(block), "1"(0),
              "r"(hd_info[dev].head));

    sec++;
    nsect = CURRENT->nr_sectors;    /* 欲读/写的扇区数. */

    /* 如果reset置1，则执行复位操作。复位硬盘和控制器，并置需要重新校正标志，返回. */
    if (reset)
    {
        reset = 0;
        recalibrate = 1;
        reset_hd(CURRENT_DEV);
        return;
    }

    /* 如果重新校正标志(recalibrate)置位，则首先复位该标志，然后向硬盘控制器发送重新校正命令. */
    if (recalibrate)
    {
        recalibrate = 0;
        hd_out(dev, hd_info[CURRENT_DEV].sect, 0, 0, 0,
               WIN_RESTORE, &recal_intr);
        return;
    }

    /**
     * 如果当前请求是写扇区操作，则发送写命令，循环读取状态寄存器信息并判断请求服务标志
     * DRQ_STAT是否置位。DRQ_STAT是硬盘状态寄存器的请求服务位(include/linux/hdreg.h).
     */
    if (CURRENT->cmd == WRITE)
    {
        hd_out(dev, nsect, sec, head, cyl, WIN_WRITE, &write_intr);

        for (i = 0; i < 3000 && !(r = inb_p(HD_STATUS) & DRQ_STAT); i++)
            /* nothing */;

        /**
         * 如果请求服务位置位则退出循环。若等到循环结束也没有置位，则此次写硬盘操作失败，
         * 去处理下一个硬盘请求。否则向硬盘控制器数据寄存器端口HD_DATA写入1个扇区的数据.
         */
        if (!r)
        {
            bad_rw_intr();
            goto repeat;            /* 该标号在blk.h最后面. */
        }

        port_write(HD_DATA, CURRENT->buffer, 256);
    }
    /* 如果当前请求是读硬盘扇区，则向硬盘控制器发送读扇区命令. */
    else if (CURRENT->cmd == READ)
    {
        hd_out(dev, nsect, sec, head, cyl, WIN_READ, &read_intr);
    }
    else
        panic("unknown hd-command");
}

/**
 * 硬盘系统初始化.
 */
void hd_init(void)
{
    /* do_hd_request(). */
    blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;

    /* 设置硬盘中断门向量int 0x2E(46),hd_interrupt 在(kernel/system_call.s). */
    set_intr_gate(0x2E, &hd_interrupt);

    /* 复位接联的主8259A int2的屏蔽位，允许从片发出中断请求信号. */
    outb_p(inb_p(0x21) & 0xfb, 0x21);

    /* 复位硬盘的中断请求屏蔽位(在从片上)，允许硬盘控制器发送中断请求信号. */
    outb(inb_p(0xA1) & 0xbf, 0xA1);
}
