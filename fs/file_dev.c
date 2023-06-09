/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

/* 取a,b中的最小值. */
#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
/* 取a,b中的最大值. */
#define MAX(a, b)               (((a) > (b)) ? (a) : (b))

/**
 * 文件读函数 - 根据inode和文件结构，读设备数据。
 * 由i节点可以知道设备号，由filp结构可以知道文件中当前读写指针位置。
 * buf指定用户态中缓冲区的位置，count为需要读取的字节数。返回值是
 * 实际读取的字节数，或出错号(小于0).
 */
int file_read(struct m_inode *inode, struct file *filp, char *buf, int count)
{
    int left, chars, nr;
    struct buffer_head *bh;

    /* 若需要读取的字节计数值小于等于零，则返回. */
    if ((left = count) <= 0)
        return 0;

    /* 若还需要读取的字节数不等于0，就循环执行以下操作，直到全部读出. */
    while (left)
    {
        /**
         * 根据inode和文件表结构信息，取数据块文件当前读写位置在设备上
         * 对应的逻辑块号nr。若nr不为0，则从inode指定的设备上读取该逻辑块，
         * 如果读操作失败则退出循环。若nr为0，表示指定的数据块不存在，置缓
         * 冲块指针为NULL.
         */
        if (nr = bmap(inode, (filp->f_pos) / BLOCK_SIZE))
        {
            if (!(bh = bread(inode->i_dev, nr)))
                break;
        }
        else
            bh = NULL;

        /**
         * 计算文件读写指针在数据块中的偏移值nr，则该块中可读字节数为
         * (BLOCK_SIZE-nr)，然后与还需读取的字节数left作比较，其中小值
         * 即为本次需读的字节数chars。若(BLOCK_SIZE-nr)大则说明该块是需要
         * 读取的最后一块数据，反之则还需要读取一块数据.
         */
        nr = filp->f_pos % BLOCK_SIZE;
        chars = MIN(BLOCK_SIZE - nr, left);

        /**
         * 调整读写文件指针。指针前移此次将读取的字节数chars。剩余字节计数
         * 相应减去chars.
         */
        filp->f_pos += chars;
        left -= chars;

        /**
         * 若从设备上读到了数据，则将p指向读出数据块缓冲区中开始读取的位置，
         * 并且复制chars字节到用户缓冲区buf中。否则往用户缓冲区中填入chars
         * 个0值字节.
         */
        if (bh)
        {
            char *p = nr + bh->b_data;
            while (chars-- > 0)
                put_fs_byte(*(p++), buf++);
            brelse(bh);
        }
        else
        {
            while (chars-- > 0)
                put_fs_byte(0, buf++);
        }
    }

    /**
     * 修改该inode的访问时间为当前时间。返回读取的字节数，若读取字节数为0，
     * 则返回出错号。
     */
    inode->i_atime = CURRENT_TIME;

    return (count - left) ? (count - left) : -ERROR;
}

/**
 * 文件写函数 - 根据inode和文件结构信息，将用户数据写入指定设备。
 * 由inode可以知道设备号，由filp结构可以知道文件中当前读写指针位置。
 * buf指定用户态中缓冲区的位置，count为需要写入的字节数。返回值是
 * 实际写入的字节数，或出错号(小于0).
 */
int file_write(struct m_inode *inode, struct file *filp, char *buf, int count)
{
    off_t pos;
    int block, c;
    struct buffer_head *bh;
    char *p;
    int i = 0;

    /**
     * ok,当许多进程同时写时，append 操作可能不行，但那又怎样。不管怎样
     * 那样做会导致混乱一团.
     * 如果是要向文件后添加数据，则将文件读写指针移到文件尾部。否则就将在
     * 文件读写指针处写入.
     */
    if (filp->f_flags & O_APPEND)
        pos = inode->i_size;
    else
        pos = filp->f_pos;

    /* 若已写入字节数i小于需要写入的字节数count，则循环执行以下操作. */
    while (i < count)
    {
        /**
         * 创建数据块号(pos/BLOCK_SIZE)在设备上对应的逻辑块，并返回在设备上
         * 的逻辑块号。如果逻辑块号=0，则表示创建失败，退出循环.
         */
        if (!(block = create_block(inode, pos / BLOCK_SIZE)))
            break;

        /* 根据该逻辑块号读取设备上的相应数据块，若出错则退出循环. */
        if (!(bh = bread(inode->i_dev, block)))
            break;

        /**
         * 求出文件读写指针在数据块中的偏移值c，将p指向读出数据块缓冲区中
         * 开始读取的位置。置该缓冲区已修改标志.
         */
        c = pos % BLOCK_SIZE;
        p = c + bh->b_data;
        bh->b_dirt = 1;

        /**
         * 从开始读写位置到块末共可写入c = (BLOCK_SIZE-c)个字节。若c大于剩余
         * 还需写入的字节数(count-i)，则此次只需再写入c = (count-i)即可.
         */
        c = BLOCK_SIZE - c;

        if (c > count - i)
            c = count - i;

        /**
         * 文件读写指针前移此次需写入的字节数。如果当前文件读写指针位置值超过了
         * 文件的大小，则修改inode中文件大小字段，并置inode已修改标志.
         */
        pos += c;

        if (pos > inode->i_size)
        {
            inode->i_size = pos;
            inode->i_dirt = 1;
        }

        /**
         * 已写入字节计数累加此次写入的字节数c。从用户缓冲区buf中复制c个字节到
         * 高速缓冲区中p指向开始的位置处。然后释放该缓冲区.
         */
        i += c;

        while (c-- > 0)
            *(p++) = get_fs_byte(buf++);

        brelse(bh);
    }

    /* 更改文件修改时间为当前时间. */
    inode->i_mtime = CURRENT_TIME;

    /**
     * 如果此次操作不是在文件尾添加数据，则把文件读写指针调整到当前读写位置，
     * 并更改inode修改时间为当前时间.
     */
    if (!(filp->f_flags & O_APPEND))
    {
        filp->f_pos = pos;
        inode->i_ctime = CURRENT_TIME;
    }

    /* 返回写入的字节数，若写入字节数为0，则返回出错号-1. */
    return (i ? i : -1);
}
