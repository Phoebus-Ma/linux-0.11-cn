/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/memory.h>

/* 内存主设备号是1. */
#define MAJOR_NR                1

#include "blk.h"

/* 虚拟盘在内存中的起始位置。在初始化函数rd_init()中确定。参见(init/main.c). */
char *rd_start;
/* 虚拟盘所占内存大小(字节). */
int rd_length = 0;

/**
 * 执行虚拟盘(ramdisk)读写操作。程序结构与do_hd_request()类似(kernel/blk_drv/hd.c).
 */
void do_rd_request(void)
{
    int len;
    char *addr;

    /* 检测请求的合法性(参见kernel/blk_drv/blk.h). */
    INIT_REQUEST;

    /*  */
    /**
     * 下面语句取得ramdisk的起始扇区对应的内存起始位置和内存长度.
     * 其中sector << 9表示sector * 512，CURRENT定义为
     * (blk_dev[MAJOR_NR].current_request).
     */
    addr = rd_start + (CURRENT->sector << 9);
    len = CURRENT->nr_sectors << 9;

    /**
     * 如果子设备号不为1或者对应内存起始位置>虚拟盘末尾，则结束该请求，并跳转到repeat处.
     */
    if ((MINOR(CURRENT->dev) != 1) || (addr + len > rd_start + rd_length))
    {
        end_request(0);
        goto repeat;
    }

    /**
     * 如果是写命令(WRITE)，则将请求项中缓冲区的内容复制到addr处，长度为len字节.
     */
    if (CURRENT->cmd == WRITE)
    {
        (void)memcpy(addr,
                     CURRENT->buffer,
                     len);
    }
    /* 如果是读命令(READ)，则将addr开始的内容复制到请求项中缓冲区中，长度为len字节. */
    else if (CURRENT->cmd == READ)
    {
        (void)memcpy(CURRENT->buffer,
                     addr,
                     len);
    }
    /* 否则显示命令不存在，死机. */
    else
        panic("unknown ramdisk-command");

    /* 请求项成功后处理，置更新标志。并继续处理本设备的下一请求项. */
    end_request(1);

    goto repeat;
}

/**
 * 返回内存虚拟盘ramdisk所需的内存量.
 */
/* 虚拟盘初始化函数。确定虚拟盘在内存中的起始地址，长度。并对整个虚拟盘区清零. */
long rd_init(long mem_start, int length)
{
    int i;
    char *cp;

    blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
    rd_start = (char *)mem_start;
    rd_length = length;
    cp = rd_start;

    for (i = 0; i < length; i++)
        *cp++ = '\0';

    return (length);
}

/**
 * 如果根文件系统设备(root device)是ramdisk的话，则尝试加载它。root device
 * 原先是指向软盘的，我们将它改成指向ramdisk.
 */
/* 加载根文件系统到ramdisk. */
void rd_load(void)
{
    struct buffer_head *bh;
    struct super_block s;
    int block = 256;        /* Start at block 256 */
    int i = 1;
    int nblocks;
    char *cp;               /* Move pointer */

    /* 如果ramdisk的长度为零，则退出. */
    if (!rd_length)
        return;

    /* 显示ramdisk的大小以及内存起始位置. */
    printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length,
           (int)rd_start);

    /* 如果此时根文件设备不是软盘，则退出. */
    if (MAJOR(ROOT_DEV) != 2)
        return;

    /**
     * 读软盘块256+1,256,256+2。breada()用于读取指定的数据块，并标出还需要读的块，
     * 然后返回含有数据块的缓冲区指针。如果返回NULL，则表示数据块不可读(fs/buffer.c)
     * 这里block+1是指磁盘上的超级块.
     */
    bh = breada(ROOT_DEV, block + 1, block, block + 2, -1);

    if (!bh)
    {
        printk("Disk error while looking for ramdisk!\n");
        return;
    }

    /* 将s指向缓冲区中的磁盘超级块。(d_super_block磁盘中超级块结构). */
    *((struct d_super_block *)&s) = *((struct d_super_block *)bh->b_data);

    /* [?? 为什么数据没有复制就立刻释放呢？]. */
    brelse(bh);

    /* 如果超级块中魔数不对，则说明不是minix文件系统. */
    if (s.s_magic != SUPER_MAGIC)
        /* 磁盘中没有ramdisk映像文件，退出执行通常的软盘引导. */
        return;

    /**
     * 块数 = 逻辑块数(区段数) * 2^(每区段块数的次方)。
     * 如果数据块数大于内存中虚拟盘所能容纳的块数，则不能加载，显示出错信息
     * 并返回。否则显示加载数据块信息.
     */
    nblocks = s.s_nzones << s.s_log_zone_size;

    if (nblocks > (rd_length >> BLOCK_SIZE_BITS))
    {
        printk("Ram disk image too big!  (%d blocks, %d avail)\n",
               nblocks, rd_length >> BLOCK_SIZE_BITS);
        return;
    }

    printk("Loading %d bytes into ram disk... 0000k",
           nblocks << BLOCK_SIZE_BITS);

    /* cp指向虚拟盘起始处，然后将磁盘上的根文件系统映象文件复制到虚拟盘上. */
    cp = rd_start;

    while (nblocks)
    {
        /* 如果需读取的块数多于3快则采用超前预读方式读数据块. */
        if (nblocks > 2)
            bh = breada(ROOT_DEV, block, block + 1, block + 2, -1);
        /* 否则就单块读取. */
        else
            bh = bread(ROOT_DEV, block);

        if (!bh)
        {
            printk("I/O error on block %d, aborting load\n",
                   block);
            return;
        }

        /* 将缓冲区中的数据复制到cp处. */
        (void)memcpy(cp, bh->b_data, BLOCK_SIZE);
        /* 释放缓冲区. */
        brelse(bh);

        /* 打印加载块计数值. */
        printk("\010\010\010\010\010%4dk", i);

        /* 虚拟盘指针前移. */
        cp += BLOCK_SIZE;
        block++;
        nblocks--;
        i++;
    }

    printk("\010\010\010\010\010done \n");

    /* 修改ROOT_DEV使其指向虚拟盘ramdisk. */
    ROOT_DEV = 0x0101;
}
