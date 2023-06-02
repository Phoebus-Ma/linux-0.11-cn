/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * 该程序处理块设备的所有读/写操作.
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * 请求结构中含有加载nr扇区数据到内存的所有必须的信息.
 */
struct request request[NR_REQUEST];

/*
 * 是用于请求数组没有空闲项时的临时等待处.
 */
struct task_struct *wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
/* 该数组使用主设备号作为索引(下标). */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
    {NULL, NULL}, /* no_dev */
    {NULL, NULL}, /* dev mem */
    {NULL, NULL}, /* dev fd */
    {NULL, NULL}, /* dev hd */
    {NULL, NULL}, /* dev ttyx */
    {NULL, NULL}, /* dev tty */
    {NULL, NULL}  /* dev lp */
};

/**
 * 锁定指定的缓冲区 bh。如果指定的缓冲区已经被其它任务锁定，则使自己睡眠
 * (不可中断地等待)，直到被执行解锁缓冲区的任务明确地唤醒.
 */
static inline void lock_buffer(struct buffer_head *bh)
{
    /* 清中断. */
    cli();

    /* 如果缓冲区已被锁定，则睡眠，直到缓冲区解锁. */
    while (bh->b_lock)
        sleep_on(&bh->b_wait);

    /* 立刻锁定该缓冲区. */
    bh->b_lock = 1;

    /* 开中断. */
    sti();
}

/**
 * 释放(解锁)锁定的缓冲区.
 */
static inline void unlock_buffer(struct buffer_head *bh)
{
    /* 如果该缓冲区并没有被锁定，则打印出错信息. */
    if (!bh->b_lock)
        printk("ll_rw_block.c: buffer not locked\n\r");

    /* 清锁定标志. */
    bh->b_lock = 0;

    /* 唤醒等待该缓冲区的任务. */
    wake_up(&bh->b_wait);
}

/*
 * add-request()向连表中加入一项请求。它关闭中断，这样就能安全地处理请求连表了.
 */
/* 向链表中加入请求项。参数dev指定块设备，req是请求的结构信息. */
static void add_request(struct blk_dev_struct *dev, struct request *req)
{
    struct request *tmp;

    req->next = NULL;

    /* 关中断. */
    cli();

    /* 清缓冲区"脏"标志. */
    if (req->bh)
        req->bh->b_dirt = 0;

    /**
     * 如果dev的当前请求(current_request)子段为空，则表示目前该设备没有请求项，
     * 本次是第1个请求项，因此可将块设备当前请求指针直接指向请求项，并立刻执行
     * 相应设备的请求函数.
     */
    if (!(tmp = dev->current_request))
    {
        dev->current_request = req;

        /* 开中断. */
        sti();

        /* 执行设备请求函数，对于硬盘(3)是do_hd_request(). */
        (dev->request_fn)();
        return;
    }

    /**
     * 如果目前该设备已经有请求项在等待，则首先利用电梯算法搜索最佳位置，然后将
     * 当前请求插入请求链表中.
     */
    for (; tmp->next; tmp = tmp->next)
        if ((IN_ORDER(tmp, req) ||
             !IN_ORDER(tmp, tmp->next)) &&
            IN_ORDER(req, tmp->next))
            break;

    req->next = tmp->next;
    tmp->next = req;

    /* 开中断. */
    sti();
}

/**
 * 创建请求项并插入请求队列。参数是：主设备号major，命令rw，存放数据的缓冲区
 * 头指针bh.
 */
static void make_request(int major, int rw, struct buffer_head *bh)
{
    struct request *req;
    int rw_ahead;

    /**
     * WRITEA/READA是特殊的情况 - 它们并不是必要的，所以如果缓冲区已经上锁，
     * 我们就不管它而退出，否则的话就执行一般的读/写操作.
     * 
     * 这里'READ'和'WRITE'后面的'A'字符代表英文单词Ahead，表示提前预读/写数据块的意思。
     * 当指定的缓冲区正在使用，已被上锁时，就放弃预读/写请求.
     */
    if (rw_ahead = (rw == READA || rw == WRITEA))
    {
        if (bh->b_lock)
            return;
        if (rw == READA)
            rw = READ;
        else
            rw = WRITE;
    }

    /* 如果命令不是READ或WRITE则表示内核程序有错，显示出错信息并死机. */
    if (rw != READ && rw != WRITE)
        panic("Bad block dev command, must be R/W/RA/WA");

    /* 锁定缓冲区，如果缓冲区已经上锁，则当前任务(进程)就会睡眠，直到被明确地唤醒. */
    lock_buffer(bh);

    /**
     * 如果命令是写并且缓冲区数据不脏，或者命令是读并且缓冲区数据是更新过的，
     * 则不用添加这个请求。将缓冲区解锁并退出.
     */
    if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate))
    {
        unlock_buffer(bh);
        return;
    }

repeat:
    /**
     * 我们不能让队列中全都是写请求项：我们需要为读请求保留一些空间：读操作
     * 是优先的。请求队列的后三分之一空间是为读准备的.
     * 
     * 请求项是从请求数组末尾开始搜索空项填入的。根据上述要求，对于读命令请求，
     * 可以直接从队列末尾开始操作，而写请求则只能从队列的2/3处向头上搜索空项填入.
     */
    if (rw == READ)
        /* 对于读请求，将队列指针指向队列尾部. */
        req = request + NR_REQUEST;
    else
        /* 对于写请求，队列指针指向队列2/3处. */
        req = request + ((NR_REQUEST * 2) / 3);

    /* 搜索一个空请求项. */
    /* 从后向前搜索，当请求结构request的dev字段值=-1时，表示该项未被占用. */
    while (--req >= request)
        if (req->dev < 0)
            break;

    /* 如果没有找到空闲项，则让该次新请求睡眠：需检查是否提前读/写. */
    /**
     * 如果没有一项是空闲的(此时request数组指针已经搜索越过头部)，则查看
     * 此次请求是否是提前读/写(READA或WRITEA)，如果是则放弃此次请求。
     * 否则让本次请求睡眠(等待请求队列腾出空项)，过一会再来搜索请求队列.
     */
    /* 如果请求队列中没有空项，则: */
    if (req < request)
    {
        /* 如果是提前读/写请求，则解锁缓冲区，退出. */
        if (rw_ahead)
        {
            unlock_buffer(bh);
            return;
        }

        /* 否则让本次请求睡眠，过会再查看请求队列. */
        sleep_on(&wait_for_request);
        goto repeat;
    }

    /* 否则让本次请求睡眠，过会再查看请求队列,请求结构参见(kernel/blk_drv/blk.h). */
    req->dev = bh->b_dev;               /* 设备号. */
    req->cmd = rw;                      /* 命令(READ/WRITE). */
    req->errors = 0;                    /* 操作时产生的错误次数. */
    req->sector = bh->b_blocknr << 1;   /* 起始扇区。(1块=2扇区). */
    req->nr_sectors = 2;                /* 读写扇区数. */
    req->buffer = bh->b_data;           /* 数据缓冲区. */
    req->waiting = NULL;                /* 任务等待操作执行完成的地方. */
    req->bh = bh;                       /* 缓冲区头指针. */
    req->next = NULL;                   /* 指向下一请求项. */

    /* 将请求项加入队列中(blk_dev[major],req). */
    add_request(major + blk_dev, req);
}

/**
 * ll_rw_block - 低层读写数据块函数。
 * 该函数主要是在fs/buffer.c中被调用。实际的读写操作是由设备的request_fn()函数完成.
 * 对于硬盘操作，该函数是do_hd_request()。(kernel/blk_drv/hd.c).
 */
void ll_rw_block(int rw, struct buffer_head *bh)
{
    /* 主设备号(对于硬盘是3). */
    unsigned int major;

    /* 如果设备的主设备号不存在或者该设备的读写操作函数不存在，则显示出错信息，并返回. */
    if ((major = MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
        !(blk_dev[major].request_fn))
    {
        printk("Trying to read nonexistent block-device\n\r");
        return;
    }

    /* 创建请求项并插入请求队列. */
    make_request(major, rw, bh);
}

/**
 * blk_dev_init - 块设备初始化函数，由初始化程序main.c调用(init/main.c).
 * 初始化请求数组，将所有请求项置为空闲项(dev = -1)。有32项(NR_REQUEST = 32).
 */
void blk_dev_init(void)
{
    int i;

    for (i = 0; i < NR_REQUEST; i++)
    {
        request[i].dev = -1;
        request[i].next = NULL;
    }
}
