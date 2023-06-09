/*
 *  linux/fs/truncate.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>

#include <sys/stat.h>

/**
 * free_ind - 释放一次间接块。
 */
static void free_ind(int dev, int block)
{
    struct buffer_head *bh;
    unsigned short *p;
    int i;

    /* 如果逻辑块号为0，则返回. */
    if (!block)
        return;

    /**
     * 读取一次间接块，并释放其上表明使用的所有逻辑块，然后释放
     * 该一次间接块的缓冲区.
     */
    if (bh = bread(dev, block))
    {
        /* 指向数据缓冲区. */
        p = (unsigned short *)bh->b_data;

        /* 每个逻辑块上可有512个块号. */
        for (i = 0; i < 512; i++, p++)
            if (*p)
                /* 释放指定的逻辑块. */
                free_block(dev, *p);

        /* 释放缓冲区. */
        brelse(bh);
    }

    /**
     * 其它字段:
     * i_zone[0]...i_zone[6],
     * inode,
     * 直接块号、一次间接块、二次间接块的一级块,
     * 二次间接块的二级块、一次间接块号、二次间接块号,
     * i_zone[7]、i_zone[8]释放设备上的一次间接块.
     */
    free_block(dev, block);
}

/**
 * free_dind - 释放二次间接块.
 */
static void free_dind(int dev, int block)
{
    struct buffer_head *bh;
    unsigned short *p;
    int i;

    /* 如果逻辑块号为0，则返回. */
    if (!block)
        return;

    /**
     * 读取二次间接块的一级块，并释放其上表明使用的所有逻辑块，
     * 然后释放该一级块的缓冲区.
     */
    if (bh = bread(dev, block))
    {
        /* 指向数据缓冲区. */
        p = (unsigned short *)bh->b_data;

        /* 每个逻辑块上可连接512个二级块. */
        for (i = 0; i < 512; i++, p++)
            if (*p)
                /* 释放所有一次间接块. */
                free_ind(dev, *p);

        /* 释放缓冲区. */
        brelse(bh);
    }

    /* 最后释放设备上的二次间接块. */
    free_block(dev, block);
}

/**
 * truncate - 将节点对应的文件长度截为 0，并释放占用的设备空间.
 */
void truncate(struct m_inode *inode)
{
    int i;

    /* 如果不是常规文件或者是目录文件，则返回. */
    if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))
        return;

    /* 释放inode的7个直接逻辑块，并将这7个逻辑块项全置零. */
    for (i = 0; i < 7; i++)
        /* 如果块号不为0，则释放. */
        if (inode->i_zone[i])
        {
            free_block(inode->i_dev, inode->i_zone[i]);
            inode->i_zone[i] = 0;
        }

    /* 释放一次间接块. */
    free_ind(inode->i_dev, inode->i_zone[7]);
    /* 释放二次间接块. */
    free_dind(inode->i_dev, inode->i_zone[8]);

    /* 逻辑块项7、8置零. */
    inode->i_zone[7] = inode->i_zone[8] = 0;
    /* 文件大小置零. */
    inode->i_size = 0;
    /* 置节点已修改标志. */
    inode->i_dirt = 1;
    /* 重置文件和节点修改时间为当前时间. */
    inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}
