/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c程序含有处理inode和磁盘块位图的代码. */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

/**
 * 将指定地址(addr)处的一块内存清零。嵌入汇编程序宏。
 * 输入：eax = 0，ecx = 数据块大小BLOCK_SIZE / 4，edi = addr.
 */
#define clear_block(addr)                          \
    __asm__("cld\n\t"                              \    /* 清方向位. */
            "rep\n\t"                              \    /* 重复执行存储数据(0). */
            "stosl" ::"a"(0),                      \
            "c"(BLOCK_SIZE / 4), "D"((long)(addr)) \
            : "cx", "di")

/**
 * 置位指定地址开始的第nr个位偏移处的比特位(nr可以大于32!)。返回原比特位(0或1)。
 * 输入：%0 - eax(返回值)，%1 - eax(0)；%2 - nr，位偏移值；%3 - (addr)，addr的内容.
 */
#define set_bit(nr, addr) ({                        \
    register int res __asm__("ax");                 \
    __asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
    "=a" (res):"0" (0),"r" (nr),"m" (*(addr)));     \
    res; })

/**
 * 复位指定地址开始的第nr位偏移处的比特位。返回原比特位的反码(1或0)。
 * 输入：%0 - eax（返回值)，%1 - eax(0)；%2 - nr，位偏移值；%3 - (addr)，addr的内容.
 */
#define clear_bit(nr, addr) ({                      \
    register int res __asm__("ax");                 \
    __asm__ __volatile__("btrl %2,%3\n\tsetnb %%al":\
    "=a" (res):"0" (0),"r" (nr),"m" (*(addr)));     \
    res; })

/**
 * 从addr开始寻找第1个0值比特位。
 * 输入：%0 - ecx(返回值)；%1 - ecx(0)；%2 - esi(addr)。
 * 在addr指定地址开始的位图中寻找第1个是0的比特位，并将其距离addr的比特位偏移值返回.
 */
#define find_first_zero(addr) ({                    \
    int __res;                                      \
    __asm__("cld\n"                                 \   /* 清方向位. */
        "1:\tlodsl\n\t"                             \   /* 取[esi]??eax. */
        "notl %%eax\n\t"                            \   /* eax中每位取反. */
        "bsfl %%eax,%%edx\n\t"                      \   /* 从位0扫描eax中是1的第1个位，其偏移值??edx. */
        "je 2f\n\t"                                 \   /* 如果eax中全是0，则向前跳转到标号2处. */
        "addl %%edx,%%ecx\n\t"                      \   /* 偏移值加入ecx(ecx中是位图中首个是0的比特位的偏移值). */
        "jmp 3f\n"                                  \   /* 向前跳转到标号3处(结束). */
        "2:\taddl $32,%%ecx\n\t"                    \   /* 没有找到0比特位，则将ecx加上1个长字的位偏移量32. */
        "cmpl $8192,%%ecx\n\t"                      \   /* 已经扫描了8192位(1024字节)了吗? */
        "jl 1b\n"                                   \   /* 若还没有扫描完1块数据，则向前跳转到标号1处，继续. */
        "3:"                                        \   /* 结束。此时 ecx 中是位偏移量. */
        :"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
    __res; })

/**
 * 释放设备dev上数据区中的逻辑块block。
 * 复位指定逻辑块block的逻辑块位图比特位。
 * 参数：dev是设备号，block是逻辑块号(盘块号).
 */
void free_block(int dev, int block)
{
    struct super_block *sb;
    struct buffer_head *bh;

    /* 取指定设备dev的超级块，如果指定设备不存在，则出错死机. */
    if (!(sb = get_super(dev)))
        panic("trying to free block on nonexistent device");

    /* 若逻辑块号小于首个逻辑块号或者大于设备上总逻辑块数，则出错，死机. */
    if (block < sb->s_firstdatazone || block >= sb->s_nzones)
        panic("trying to free block not in datazone");

    /**
     * 从hash表中寻找该块数据。若找到了则判断其有效性，并清已修改和更新标志，释放该数据块。
     * 该段代码的主要用途是如果该逻辑块当前存在于高速缓冲中，就释放对应的缓冲块.
     */
    bh = get_hash_table(dev, block);

    if (bh)
    {
        if (bh->b_count != 1)
        {
            printk("trying to free block (%04x:%d), count=%d\n",
                   dev, block, bh->b_count);
            return;
        }

        bh->b_dirt = 0;             /* 复位脏(已修改)标志位. */
        bh->b_uptodate = 0;         /* 复位更新标志. */
        brelse(bh);
    }

    /**
     * 计算block在数据区开始算起的数据逻辑块号(从1开始计数)。然后对逻辑块(区块)
     * 位图进行操作，复位对应的比特位。若对应比特位原来即是0，则出错，死机.
     */
    block -= sb->s_firstdatazone - 1;

    if (clear_bit(block & 8191, sb->s_zmap[block / 8192]->b_data))
    {
        printk("block (%04x:%d) ", dev, block + sb->s_firstdatazone - 1);
        panic("free_block: bit already cleared");
    }

    /* 置相应逻辑块位图所在缓冲区已修改标志. */
    sb->s_zmap[block / 8192]->b_dirt = 1;
}

/**
 * 向设备dev申请一个逻辑块(盘块，区块)。返回逻辑块号(盘块号)。
 * 置位指定逻辑块block的逻辑块位图比特位.
 */
int new_block(int dev)
{
    struct buffer_head *bh;
    struct super_block *sb;
    int i, j;

    /* 从设备dev取超级块，如果指定设备不存在，则出错死机. */
    if (!(sb = get_super(dev)))
        panic("trying to get new block from nonexistant device");

    /* 扫描逻辑块位图，寻找首个0比特位，寻找空闲逻辑块，获取放置该逻辑块的块号. */
    j = 8192;

    for (i = 0; i < 8; i++)
        if (bh = sb->s_zmap[i])
            if ((j = find_first_zero(bh->b_data)) < 8192)
                break;

    /**
     * 如果全部扫描完还没找到(i>=8或j>=8192)或者位图所在的缓冲块无效(bh=NULL)
     * 则返回0，退出(没有空闲逻辑块).
     */
    if (i >= 8 || !bh || j >= 8192)
        return 0;

    /* 设置新逻辑块对应逻辑块位图中的比特位，若对应比特位已经置位，则出错，死机. */
    if (set_bit(j, bh->b_data))
        panic("new_block: bit already set");

    /**
     * 置对应缓冲区块的已修改标志。如果新逻辑块大于该设备上的总逻辑块数，则说明
     * 指定逻辑块在对应设备上不存在。申请失败，返回0，退出.
     */
    bh->b_dirt = 1;
    j += i * 8192 + sb->s_firstdatazone - 1;

    if (j >= sb->s_nzones)
        return 0;

    /* 读取设备上的该新逻辑块数据(验证)。如果失败则死机. */
    if (!(bh = getblk(dev, j)))
        panic("new_block: cannot get block");

    /* 新块的引用计数应为1。否则死机. */
    if (bh->b_count != 1)
        panic("new block: count is != 1");

    /* 将该新逻辑块清零，并置位更新标志和已修改标志。然后释放对应缓冲区，返回逻辑块号. */
    clear_block(bh->b_data);
    bh->b_uptodate = 1;
    bh->b_dirt = 1;
    brelse(bh);

    return j;
}

/**
 * 释放指定的inode.
 * 复位对应inode位图比特位.
 */
void free_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;

    /* 如果inode指针=NULL，则退出. */
    if (!inode)
        return;

    /**
     * 如果inode上的设备号字段为0，说明该节点无用，则用0清空对应inode所
     * 占内存区，并返回.
     */
    if (!inode->i_dev)
    {
        memset(inode, 0, sizeof(*inode));
        return;
    }

    /* 如果此inode还有其它程序引用，则不能释放，说明内核有问题，死机. */
    if (inode->i_count > 1)
    {
        printk("trying to free inode with count=%d\n", inode->i_count);
        panic("free_inode");
    }

    /**
     * 如果文件目录项连接数不为0，则表示还有其它文件目录项在使用该节点，
     * 不应释放，而应该放回等。
     */
    if (inode->i_nlinks)
        panic("trying to free inode with links");

    /* 取inode所在设备的超级块，测试设备是否存在. */
    if (!(sb = get_super(inode->i_dev)))
        panic("trying to free inode on nonexistent device");

    /* 如果inode号=0或大于该设备上inode总数，则出错(0号inode保留没有使用). */
    if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
        panic("trying to free inode 0 or nonexistant inode");

    /* 如果该inode对应的节点位图不存在，则出错. */
    if (!(bh = sb->s_imap[inode->i_num >> 13]))
        panic("nonexistent imap in superblock");

    /* 复位inode对应的节点位图中的比特位，如果该比特位已经等于0，则出错. */
    if (clear_bit(inode->i_num & 8191, bh->b_data))
        printk("free_inode: bit already cleared.\n\r");

    /* 置inode位图所在缓冲区已修改标志，并清空该inode结构所占内存区. */
    bh->b_dirt = 1;
    memset(inode, 0, sizeof(*inode));
}

/**
 * 为设备dev建立一个新inode。返回该新inode的指针。
 * 在内存inode表中获取一个空闲inode表项，并从inode位图中找一个空闲inode.
 */
struct m_inode *new_inode(int dev)
{
    struct m_inode *inode;
    struct super_block *sb;
    struct buffer_head *bh;
    int i, j;

    /* 从内存inode表(inode_table)中获取一个空闲inode项(inode). */
    if (!(inode = get_empty_inode()))
        return NULL;

    /* 读取指定设备的超级块结构. */
    if (!(sb = get_super(dev)))
        panic("new_inode with unknown device");

    /* 扫描inode位图，寻找首个0比特位，寻找空闲节点，获取放置该inode的节点号. */
    j = 8192;

    for (i = 0; i < 8; i++)
        if (bh = sb->s_imap[i])
            if ((j = find_first_zero(bh->b_data)) < 8192)
                break;

    /**
     * 如果全部扫描完还没找到，或者位图所在的缓冲块无效(bh=NULL)则返回0，
     * 退出(没有空闲inode).
     */
    if (!bh || j >= 8192 || j + i * 8192 > sb->s_ninodes)
    {
        iput(inode);
        return NULL;
    }

    /* 置位对应新inode的inode位图相应比特位，如果已经置位，则出错. */
    if (set_bit(j, bh->b_data))
        panic("new_inode: bit already set");

    /* 置inode位图所在缓冲区已修改标志. */
    bh->b_dirt = 1;

    /* 初始化该inode结构. */
    inode->i_count  = 1;                /* 引用计数. */
    inode->i_nlinks = 1;                /* 文件目录项链接数. */
    inode->i_dev    = dev;              /* inode所在的设备号. */
    inode->i_uid    = current->euid;    /* inode所属用户id. */
    inode->i_gid    = current->egid;    /* 组id. */
    inode->i_dirt   = 1;                /* 已修改标志置位. */
    inode->i_num    = j + i * 8192;     /* 对应设备中的inode号. */
    inode->i_mtime  = inode->i_atime = inode->i_ctime = CURRENT_TIME;   /* 设置时间. */

    /* 返回该inode指针. */
    return inode;
}
