/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

/* 内存中inode表(NR_INODE=32项). */
struct m_inode inode_table[NR_INODE] = {
    {
        0,
    },
};

static void read_inode(struct m_inode *inode);
static void write_inode(struct m_inode *inode);

/**
 * wait_on_inode - 等待指定的inode可用.
 * 
 * 如果inode已被锁定，则将当前任务置为不可中断的等待状态。直到该inode解锁.
 */
static inline void wait_on_inode(struct m_inode *inode)
{
    cli();

    while (inode->i_lock)
        sleep_on(&inode->i_wait);

    sti();
}

/**
 * lock_inode - 对指定的inode上锁(锁定指定的inode).
 * 
 * 如果inode已被锁定，则将当前任务置为不可中断的等待状态。直到该inode解锁，
 * 然后对其上锁.
 */
static inline void lock_inode(struct m_inode *inode)
{
    cli();

    while (inode->i_lock)
        sleep_on(&inode->i_wait);

    inode->i_lock = 1;

    sti();
}

/**
 * unlock_inode - 对指定的inode解锁.
 * 
 * 复位inode的锁定标志，并明确地唤醒等待此inode的进程.
 */
static inline void unlock_inode(struct m_inode *inode)
{
    inode->i_lock = 0;
    wake_up(&inode->i_wait);
}

/**
 * invalidate_inodes - 释放内存中设备dev的所有inode.
 * 
 * 扫描内存中的inode表数组，如果是指定设备使用的inode就释放之.
 */
void invalidate_inodes(int dev)
{
    int i;
    struct m_inode *inode;

    inode = 0 + inode_table;

    for (i = 0; i < NR_INODE; i++, inode++)
    {
        wait_on_inode(inode);

        if (inode->i_dev == dev)
        {
            if (inode->i_count)
                printk("inode in use on removed disk\n\r");

            inode->i_dev = inode->i_dirt = 0;
        }
    }
}

/**
 * sync_inodes - 同步所有inode.
 * 
 * 同步内存与设备上的所有inode信息.
 */
void sync_inodes(void)
{
    int i;
    struct m_inode *inode;

    inode = 0 + inode_table;

    for (i = 0; i < NR_INODE; i++, inode++)
    {
        wait_on_inode(inode);
        if (inode->i_dirt && !inode->i_pipe)
            write_inode(inode);
    }
}

/**
 * _bmap - 文件数据块映射到盘块的处理操作.
 * 
 * 参数：
 * inode – 文件的inode；
 * block – 文件中的数据块号；
 * create - 创建标志。
 * 
 * 如果创建标志置位，则在对应逻辑块不存在时就申请新磁盘块。
 * 返回block数据块对应在设备上的逻辑块号(盘块号).
 */
static int _bmap(struct m_inode *inode, int block, int create)
{
    struct buffer_head *bh;
    int i;

    if (block < 0)
        panic("_bmap: block<0");

    if (block >= 7 + 512 + 512 * 512)
        panic("_bmap: block>big");

    if (block < 7)
    {
        if (create && !inode->i_zone[block])
            if (inode->i_zone[block] = new_block(inode->i_dev))
            {
                inode->i_ctime = CURRENT_TIME;
                inode->i_dirt = 1;
            }

        return inode->i_zone[block];
    }

    block -= 7;

    if (block < 512)
    {
        if (create && !inode->i_zone[7])
            if (inode->i_zone[7] = new_block(inode->i_dev))
            {
                inode->i_dirt = 1;
                inode->i_ctime = CURRENT_TIME;
            }

        if (!inode->i_zone[7])
            return 0;

        if (!(bh = bread(inode->i_dev, inode->i_zone[7])))
            return 0;

        i = ((unsigned short *)(bh->b_data))[block];

        if (create && !i)
            if (i = new_block(inode->i_dev))
            {
                ((unsigned short *)(bh->b_data))[block] = i;
                bh->b_dirt = 1;
            }

        brelse(bh);

        return i;
    }

    block -= 512;

    if (create && !inode->i_zone[8])
        if (inode->i_zone[8] = new_block(inode->i_dev))
        {
            inode->i_dirt = 1;
            inode->i_ctime = CURRENT_TIME;
        }

    if (!inode->i_zone[8])
        return 0;

    if (!(bh = bread(inode->i_dev, inode->i_zone[8])))
        return 0;

    i = ((unsigned short *)bh->b_data)[block >> 9];

    if (create && !i)
        if (i = new_block(inode->i_dev))
        {
            ((unsigned short *)(bh->b_data))[block >> 9] = i;
            bh->b_dirt = 1;
        }

    brelse(bh);

    if (!i)
        return 0;

    if (!(bh = bread(inode->i_dev, i)))
        return 0;

    i = ((unsigned short *)bh->b_data)[block & 511];

    if (create && !i)
        if (i = new_block(inode->i_dev))
        {
            ((unsigned short *)(bh->b_data))[block & 511] = i;
            bh->b_dirt = 1;
        }

    brelse(bh);

    return i;
}

/**
 * bmap - 根据inode信息取文件数据块block在设备上对应的逻辑块号.
 */
int bmap(struct m_inode *inode, int block)
{
    return _bmap(inode, block, 0);
}

/**
 * create_block - 创建文件数据块block在设备上对应的逻辑块，
 * 并返回设备上对应的逻辑块号.
 */
int create_block(struct m_inode *inode, int block)
{
    return _bmap(inode, block, 1);
}

/**
 * iput - 释放一个inode(回写入设备).
 */
void iput(struct m_inode *inode)
{
    if (!inode)
        return;

    wait_on_inode(inode);

    if (!inode->i_count)
        panic("iput: trying to free free inode");

    if (inode->i_pipe)
    {
        wake_up(&inode->i_wait);
        if (--inode->i_count)
            return;
        free_page(inode->i_size);
        inode->i_count = 0;
        inode->i_dirt = 0;
        inode->i_pipe = 0;
        return;
    }

    if (!inode->i_dev)
    {
        inode->i_count--;
        return;
    }

    if (S_ISBLK(inode->i_mode))
    {
        sync_dev(inode->i_zone[0]);
        wait_on_inode(inode);
    }

repeat:
    if (inode->i_count > 1)
    {
        inode->i_count--;
        return;
    }

    if (!inode->i_nlinks)
    {
        truncate(inode);
        free_inode(inode);
        return;
    }

    if (inode->i_dirt)
    {
        write_inode(inode); /* we can sleep - so do again */
        wait_on_inode(inode);
        goto repeat;
    }

    inode->i_count--;

    return;
}

/**
 * get_empty_inode - 从inode表(inode_table)中获取一个空闲inode项.
 * 寻找引用计数count为0的inode，并将其写盘后清零，返回其指针.
 */
struct m_inode *get_empty_inode(void)
{
    struct m_inode *inode;
    static struct m_inode *last_inode = inode_table;
    int i;

    do
    {
        inode = NULL;

        for (i = NR_INODE; i; i--)
        {
            if (++last_inode >= inode_table + NR_INODE)
                last_inode = inode_table;

            if (!last_inode->i_count)
            {
                inode = last_inode;

                if (!inode->i_dirt && !inode->i_lock)
                    break;
            }
        }

        if (!inode)
        {
            for (i = 0; i < NR_INODE; i++)
                printk("%04x: %6d\t", inode_table[i].i_dev,
                       inode_table[i].i_num);

            panic("No free inodes in mem");
        }

        wait_on_inode(inode);

        while (inode->i_dirt)
        {
            write_inode(inode);
            wait_on_inode(inode);
        }
    } while (inode->i_count);

    memset(inode, 0, sizeof(*inode));
    inode->i_count = 1;

    return inode;
}

/**
 * get_pipe_inode - 获取管道节点。返回为inode指针(如果是NULL则失败).
 * 
 * 首先扫描inode表，寻找一个空闲inode项，然后取得一页空闲内存供管道使用。
 * 然后将得到的inode的引用计数置为2(读者和写者)，初始化管道头和尾，置inode
 * 的管道类型表示.
 */
struct m_inode *get_pipe_inode(void)
{
    struct m_inode *inode;

    if (!(inode = get_empty_inode()))
        return NULL;

    if (!(inode->i_size = get_free_page()))
    {
        inode->i_count = 0;
        return NULL;
    }

    inode->i_count = 2; /* sum of readers/writers */
    PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
    inode->i_pipe = 1;

    return inode;
}

/**
 * iget - 从设备上读取指定节点号的inode.
 * nr - inode号.
 */
struct m_inode *iget(int dev, int nr)
{
    struct m_inode *inode, *empty;

    if (!dev)
        panic("iget with dev==0");

    empty = get_empty_inode();
    inode = inode_table;

    while (inode < NR_INODE + inode_table)
    {
        if (inode->i_dev != dev || inode->i_num != nr)
        {
            inode++;
            continue;
        }

        wait_on_inode(inode);

        if (inode->i_dev != dev || inode->i_num != nr)
        {
            inode = inode_table;
            continue;
        }

        inode->i_count++;

        if (inode->i_mount)
        {
            int i;

            for (i = 0; i < NR_SUPER; i++)
                if (super_block[i].s_imount == inode)
                    break;

            if (i >= NR_SUPER)
            {
                printk("Mounted inode hasn't got sb\n");

                if (empty)
                    iput(empty);

                return inode;
            }

            iput(inode);
            dev = super_block[i].s_dev;
            nr = ROOT_INO;
            inode = inode_table;
            continue;
        }

        if (empty)
            iput(empty);

        return inode;
    }

    if (!empty)
        return (NULL);

    inode = empty;
    inode->i_dev = dev;
    inode->i_num = nr;
    read_inode(inode);

    return inode;
}

/**
 * read_inode - 从设备上读取指定inode的信息到内存中(缓冲区中).
 */
static void read_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;
    int block;

    lock_inode(inode);

    if (!(sb = get_super(inode->i_dev)))
        panic("trying to read inode without dev");

    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
            (inode->i_num - 1) / INODES_PER_BLOCK;

    if (!(bh = bread(inode->i_dev, block)))
        panic("unable to read i-node block");

    *(struct d_inode *)inode =
        ((struct d_inode *)bh->b_data)
            [(inode->i_num - 1) % INODES_PER_BLOCK];

    brelse(bh);
    unlock_inode(inode);
}

/**
 * write_inode - 将指定inode信息写入设备(写入缓冲区相应的缓冲块中，待缓冲区刷新时会写入盘中).
 */
static void write_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;
    int block;

    lock_inode(inode);

    if (!inode->i_dirt || !inode->i_dev)
    {
        unlock_inode(inode);
        return;
    }

    if (!(sb = get_super(inode->i_dev)))
        panic("trying to write inode without device");

    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
            (inode->i_num - 1) / INODES_PER_BLOCK;

    if (!(bh = bread(inode->i_dev, block)))
        panic("unable to read i-node block");

    ((struct d_inode *)bh->b_data)
        [(inode->i_num - 1) % INODES_PER_BLOCK] =
            *(struct d_inode *)inode;

    bh->b_dirt = 1;
    inode->i_dirt = 0;
    brelse(bh);
    unlock_inode(inode);
}
