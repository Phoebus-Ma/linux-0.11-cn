/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

/**
 * 访问模式宏。x是include/fcntl.h第7行开始定义的文件访问标志。
 * 根据x值索引对应数值(数值表示rwx权限: r, w, rw, wxrwxrwx)(8进制).
 */
#define ACC_MODE(x)             ("\004\002\006\377"[(x)&O_ACCMODE])

/**
 * 如果想让文件名长度 > NAME_LEN的字符被截掉，就将下面定义注释掉.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC                1       /* 可执行(可进入). */
#define MAY_WRITE               2       /* 可写. */
#define MAY_READ                4       /* 可读. */

/**
 * permission()
 *
 * 该函数用于检测一个文件的读/写/执行权限。我不知道是否只需检查euid，还是
 * 需要检查euid和uid两者，不过这很容易修改.
 */
/**
 * permission - 检测文件访问许可权限。
 * 
 * 参数：inode - 文件对应的 i 节点；mask - 访问属性屏蔽码。
 * 返回：访问许可返回1，否则返回0.
 */
static int permission(struct m_inode *inode, int mask)
{
    int mode = inode->i_mode;

    /* special case: not even root can read/write a deleted file */
    if (inode->i_dev && !inode->i_nlinks)
        return 0;
    else if (current->euid == inode->i_uid)
        mode >>= 6;
    else if (current->egid == inode->i_gid)
        mode >>= 3;

    if (((mode & mask & 0007) == mask) || suser())
        return 1;

    return 0;
}

/**
 * ok, 我们不能使用strncmp字符串比较函数，因为名称不在我们的数据空间
 * (不在内核空间)。
 * 因而我们只能使用match()。问题不大。match()同样也处理一些完整的测试.
 *
 * 注意！与strncmp不同的是match()成功时返回1，失败时返回0.
 */
/**
 * match - 指定长度字符串比较函数。
 * 
 * 参数：len - 比较的字符串长度；name - 文件名指针；de - 目录项结构。
 * 返回：相同返回1，不同返回0.
 */
static int match(int len, const char *name, struct dir_entry *de)
{
    register int same __asm__("ax");

    if (!de || !de->inode || len > NAME_LEN)
        return 0;

    if (len < NAME_LEN && de->name[len])
        return 0;

    __asm__("cld\n\t"
            "fs ; repe ; cmpsb\n\t"
            "setz %%al"
            : "=a"(same)
            : "0"(0), "S"((long)name), "D"((long)de->name), "c"(len)
            : "cx", "di", "si");

    return same;
}

/**
 * find_entry()
 *
 * 在指定的目录中寻找一个与名字匹配的目录项。返回一个含有找到目录项的高速
 * 缓冲区以及目录项本身(作为一个参数-res_dir)。并不读目录项的inode - 如
 * 果需要的话需自己操作.
 *
 * '..'目录项，操作期间也会对几种特殊情况分别处理 - 比如横越一个伪根目录以
 * 及安装点.
 */
/**
 * find_entry - 查找指定目录和文件名的目录项。
 * 
 * 参数：dir - 指定目录 i 节点的指针；name - 文件名；namelen - 文件名长度；
 * 返回：高速缓冲区指针；res_dir - 返回的目录项结构指针.
 */
static struct buffer_head *find_entry(struct m_inode **dir,
                                      const char *name, int namelen, struct dir_entry **res_dir)
{
    int entries;
    int block, i;
    struct buffer_head *bh;
    struct dir_entry *de;
    struct super_block *sb;

#ifdef NO_TRUNCATE
    if (namelen > NAME_LEN)
        return NULL;
#else
    if (namelen > NAME_LEN)
        namelen = NAME_LEN;
#endif

    entries = (*dir)->i_size / (sizeof(struct dir_entry));
    *res_dir = NULL;

    if (!namelen)
        return NULL;

    /* check for '..', as we might have to do some "magic" for it */
    if (namelen == 2 && get_fs_byte(name) == '.' && get_fs_byte(name + 1) == '.')
    {
        /* '..' in a pseudo-root results in a faked '.' (just change namelen) */
        if ((*dir) == current->root)
            namelen = 1;
        else if ((*dir)->i_num == ROOT_INO)
        {
            /* '..' over a mount-point results in 'dir' being exchanged for the mounted
               directory-inode. NOTE! We set mounted, so that we can iput the new dir */
            sb = get_super((*dir)->i_dev);

            if (sb->s_imount)
            {
                iput(*dir);
                (*dir) = sb->s_imount;
                (*dir)->i_count++;
            }
        }
    }

    if (!(block = (*dir)->i_zone[0]))
        return NULL;

    if (!(bh = bread((*dir)->i_dev, block)))
        return NULL;

    i = 0;
    de = (struct dir_entry *)bh->b_data;

    while (i < entries)
    {
        if ((char *)de >= BLOCK_SIZE + bh->b_data)
        {
            brelse(bh);
            bh = NULL;

            if (!(block = bmap(*dir, i / DIR_ENTRIES_PER_BLOCK)) ||
                !(bh = bread((*dir)->i_dev, block)))
            {
                i += DIR_ENTRIES_PER_BLOCK;
                continue;
            }

            de = (struct dir_entry *)bh->b_data;
        }

        if (match(namelen, name, de))
        {
            *res_dir = de;
            return bh;
        }

        de++;
        i++;
    }

    brelse(bh);

    return NULL;
}

/**
 * add_entry()
 *
 * 使用与find_entry()同样的方法，往指定目录中添加一文件目录项。
 * 如果失败则返回 NULL.
 *
 * 注意！！'de'(指定目录项结构指针)的inode部分被设置为0 - 这表示
 * 在调用该函数和往目录项中添加信息之间不能睡眠，因为若睡眠那么其它
 * 人(进程)可能会已经使用了该目录项.
 */
/**
 * add_entry - 根据指定的目录和文件名添加目录项。
 * 
 * 参数：dir - 指定目录的 i 节点；name - 文件名；namelen - 文件名长度；
 * 返回：高速缓冲区指针；res_dir - 返回的目录项结构指针.
 */
static struct buffer_head *add_entry(struct m_inode *dir,
                                     const char *name, int namelen, struct dir_entry **res_dir)
{
    int block, i;
    struct buffer_head *bh;
    struct dir_entry *de;

    *res_dir = NULL;

#ifdef NO_TRUNCATE
    if (namelen > NAME_LEN)
        return NULL;
#else
    if (namelen > NAME_LEN)
        namelen = NAME_LEN;
#endif

    if (!namelen)
        return NULL;

    if (!(block = dir->i_zone[0]))
        return NULL;

    if (!(bh = bread(dir->i_dev, block)))
        return NULL;

    i = 0;
    de = (struct dir_entry *)bh->b_data;

    while (1)
    {
        if ((char *)de >= BLOCK_SIZE + bh->b_data)
        {
            brelse(bh);
            bh = NULL;
            block = create_block(dir, i / DIR_ENTRIES_PER_BLOCK);

            if (!block)
                return NULL;

            if (!(bh = bread(dir->i_dev, block)))
            {
                i += DIR_ENTRIES_PER_BLOCK;
                continue;
            }

            de = (struct dir_entry *)bh->b_data;
        }

        if (i * sizeof(struct dir_entry) >= dir->i_size)
        {
            de->inode = 0;
            dir->i_size = (i + 1) * sizeof(struct dir_entry);
            dir->i_dirt = 1;
            dir->i_ctime = CURRENT_TIME;
        }

        if (!de->inode)
        {
            dir->i_mtime = CURRENT_TIME;

            for (i = 0; i < NAME_LEN; i++)
                de->name[i] = (i < namelen) ? get_fs_byte(name + i) : 0;

            bh->b_dirt = 1;
            *res_dir = de;

            return bh;
        }

        de++;
        i++;
    }

    brelse(bh);

    return NULL;
}

/**
 * get_dir()
 *
 * 该函数根据给出的路径名进行搜索，直到达到最顶端的目录。
 * 如果失败则返回NULL.
 */
/**
 * get_dir - 搜寻指定路径名的目录。
 * 
 * 参数：pathname - 路径名。
 * 返回：目录的inode指针。失败时返回NULL.
 */
static struct m_inode *get_dir(const char *pathname)
{
    char c;
    const char *thisname;
    struct m_inode *inode;
    struct buffer_head *bh;
    int namelen, inr, idev;
    struct dir_entry *de;

    if (!current->root || !current->root->i_count)
        panic("No root inode");

    if (!current->pwd || !current->pwd->i_count)
        panic("No cwd inode");

    if ((c = get_fs_byte(pathname)) == '/')
    {
        inode = current->root;
        pathname++;
    }
    else if (c)
        inode = current->pwd;
    else
        return NULL; /* empty name is bad */

    inode->i_count++;

    while (1)
    {
        thisname = pathname;

        if (!S_ISDIR(inode->i_mode) || !permission(inode, MAY_EXEC))
        {
            iput(inode);
            return NULL;
        }

        for (namelen = 0; (c = get_fs_byte(pathname++)) && (c != '/'); namelen++)
            /* nothing */;

        if (!c)
            return inode;

        if (!(bh = find_entry(&inode, thisname, namelen, &de)))
        {
            iput(inode);
            return NULL;
        }

        inr = de->inode;
        idev = inode->i_dev;
        brelse(bh);
        iput(inode);

        if (!(inode = iget(idev, inr)))
            return NULL;
    }
}

/**
 * dir_namei()
 *
 * dir_namei()函数返回指定目录名的inode指针，以及在最顶层目录的名称.
 * 
 * 参数：pathname - 目录路径名；namelen - 路径名长度。
 * 返回：指定目录名最顶层目录的inode指针和最顶层目录名及其长度.
 */
static struct m_inode *dir_namei(const char *pathname,
                                 int *namelen, const char **name)
{
    char c;
    const char *basename;
    struct m_inode *dir;

    if (!(dir = get_dir(pathname)))
        return NULL;

    basename = pathname;

    while (c = get_fs_byte(pathname++))
        if (c == '/')
            basename = pathname;

    *namelen = pathname - basename - 1;
    *name = basename;

    return dir;
}

/**
 * namei()
 *
 * 该函数被许多简单的命令用于取得指定路径名称的inode。open、link等则使用它们
 * 自己的相应函数，但对于象修改模式'chmod'等这样的命令，该函数已足够用了.
 */
/**
 * namei - 取指定路径名的inode。
 * 
 * 参数：pathname - 路径名。
 * 返回：对应的inode.
 */
struct m_inode *namei(const char *pathname)
{
    const char *basename;
    int inr, dev, namelen;
    struct m_inode *dir;
    struct buffer_head *bh;
    struct dir_entry *de;

    if (!(dir = dir_namei(pathname, &namelen, &basename)))
        return NULL;

    if (!namelen) /* special case: '/usr/' etc */
        return dir;

    bh = find_entry(&dir, basename, namelen, &de);

    if (!bh)
    {
        iput(dir);
        return NULL;
    }

    inr = de->inode;
    dev = dir->i_dev;

    brelse(bh);
    iput(dir);

    dir = iget(dev, inr);

    if (dir)
    {
        dir->i_atime = CURRENT_TIME;
        dir->i_dirt = 1;
    }

    return dir;
}

/**
 * open_namei()
 *
 * open()所使用的namei函数 - 这其实几乎是完整的打开文件程序.
 */
/**
 * open_namei - 文件打开namei函数。
 * 
 * 参数：pathname - 文件路径名；flag - 文件打开标志；mode - 文件访问许可属性；
 * 返回：成功返回0，否则返回出错码；res_inode - 返回的对应文件路径名的的inode指针.
 */
int open_namei(const char *pathname, int flag, int mode,
               struct m_inode **res_inode)
{
    const char *basename;
    int inr, dev, namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
        flag |= O_WRONLY;

    mode &= 0777 & ~current->umask;
    mode |= I_REGULAR;

    if (!(dir = dir_namei(pathname, &namelen, &basename)))
        return -ENOENT;

    if (!namelen)
    { /* special case: '/usr/' etc */
        if (!(flag & (O_ACCMODE | O_CREAT | O_TRUNC)))
        {
            *res_inode = dir;
            return 0;
        }

        iput(dir);

        return -EISDIR;
    }

    bh = find_entry(&dir, basename, namelen, &de);

    if (!bh)
    {
        if (!(flag & O_CREAT))
        {
            iput(dir);
            return -ENOENT;
        }

        if (!permission(dir, MAY_WRITE))
        {
            iput(dir);
            return -EACCES;
        }

        inode = new_inode(dir->i_dev);

        if (!inode)
        {
            iput(dir);
            return -ENOSPC;
        }

        inode->i_uid = current->euid;
        inode->i_mode = mode;
        inode->i_dirt = 1;

        bh = add_entry(dir, basename, namelen, &de);

        if (!bh)
        {
            inode->i_nlinks--;
            iput(inode);
            iput(dir);
            return -ENOSPC;
        }

        de->inode = inode->i_num;
        bh->b_dirt = 1;

        brelse(bh);
        iput(dir);

        *res_inode = inode;

        return 0;
    }

    inr = de->inode;
    dev = dir->i_dev;

    brelse(bh);
    iput(dir);

    if (flag & O_EXCL)
        return -EEXIST;

    if (!(inode = iget(dev, inr)))
        return -EACCES;

    if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
        !permission(inode, ACC_MODE(flag)))
    {
        iput(inode);
        return -EPERM;
    }

    inode->i_atime = CURRENT_TIME;

    if (flag & O_TRUNC)
        truncate(inode);

    *res_inode = inode;

    return 0;
}

/**
 * sys_mknod - 创建一个特殊文件或普通文件节点(node)。
 * 创建名称为filename，由mode和dev指定的文件系统节点(普通文件、
 * 设备特殊文件或命名管道)。
 * 
 * 参数：
 * filename - 路径名；
 * mode - 指定使用许可以及所创建节点的类型；
 * dev - 设备号。
 * 
 * 返回：成功则返回 0，否则返回出错码.
 */
int sys_mknod(const char *filename, int mode, int dev)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    if (!suser())
        return -EPERM;

    if (!(dir = dir_namei(filename, &namelen, &basename)))
        return -ENOENT;

    if (!namelen)
    {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE))
    {
        iput(dir);
        return -EPERM;
    }

    bh = find_entry(&dir, basename, namelen, &de);

    if (bh)
    {
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }

    inode = new_inode(dir->i_dev);

    if (!inode)
    {
        iput(dir);
        return -ENOSPC;
    }

    inode->i_mode = mode;

    if (S_ISBLK(mode) || S_ISCHR(mode))
        inode->i_zone[0] = dev;

    inode->i_mtime = inode->i_atime = CURRENT_TIME;
    inode->i_dirt = 1;
    bh = add_entry(dir, basename, namelen, &de);

    if (!bh)
    {
        iput(dir);
        inode->i_nlinks = 0;
        iput(inode);
        return -ENOSPC;
    }

    de->inode = inode->i_num;
    bh->b_dirt = 1;
    iput(dir);
    iput(inode);
    brelse(bh);

    return 0;
}

/**
 * sys_mkdir - 创建目录。
 * 
 * 参数：pathname - 路径名；mode - 目录使用的权限属性。
 * 返回：成功则返回 0，否则返回出错码.
 */
int sys_mkdir(const char *pathname, int mode)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh, *dir_block;
    struct dir_entry *de;

    if (!suser())
        return -EPERM;

    if (!(dir = dir_namei(pathname, &namelen, &basename)))
        return -ENOENT;

    if (!namelen)
    {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE))
    {
        iput(dir);
        return -EPERM;
    }

    bh = find_entry(&dir, basename, namelen, &de);

    if (bh)
    {
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }

    inode = new_inode(dir->i_dev);

    if (!inode)
    {
        iput(dir);
        return -ENOSPC;
    }

    inode->i_size = 32;
    inode->i_dirt = 1;
    inode->i_mtime = inode->i_atime = CURRENT_TIME;

    if (!(inode->i_zone[0] = new_block(inode->i_dev)))
    {
        iput(dir);
        inode->i_nlinks--;
        iput(inode);
        return -ENOSPC;
    }

    inode->i_dirt = 1;

    if (!(dir_block = bread(inode->i_dev, inode->i_zone[0])))
    {
        iput(dir);
        free_block(inode->i_dev, inode->i_zone[0]);
        inode->i_nlinks--;
        iput(inode);
        return -ERROR;
    }

    de = (struct dir_entry *)dir_block->b_data;
    de->inode = inode->i_num;
    strcpy(de->name, ".");
    de++;
    de->inode = dir->i_num;
    strcpy(de->name, "..");
    inode->i_nlinks = 2;
    dir_block->b_dirt = 1;
    brelse(dir_block);
    inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
    inode->i_dirt = 1;

    bh = add_entry(dir, basename, namelen, &de);

    if (!bh)
    {
        iput(dir);
        free_block(inode->i_dev, inode->i_zone[0]);
        inode->i_nlinks = 0;
        iput(inode);
        return -ENOSPC;
    }

    de->inode = inode->i_num;
    bh->b_dirt = 1;
    dir->i_nlinks++;
    dir->i_dirt = 1;
    iput(dir);
    iput(inode);
    brelse(bh);

    return 0;
}

/**
 * 用于检查指定的目录是否为空的子程序(用于rmdir系统调用函数).
 */
/**
 * empty_dir - 检查指定目录是否是空的。
 * 
 * 参数：inode - 指定目录的 i 节点指针。
 * 返回：0 - 是空的；1 - 不空.
 */
static int empty_dir(struct m_inode *inode)
{
    int nr, block;
    int len;
    struct buffer_head *bh;
    struct dir_entry *de;

    len = inode->i_size / sizeof(struct dir_entry);

    if (len < 2 || !inode->i_zone[0] ||
        !(bh = bread(inode->i_dev, inode->i_zone[0])))
    {
        printk("warning - bad directory on dev %04x\n", inode->i_dev);
        return 0;
    }

    de = (struct dir_entry *)bh->b_data;

    if (de[0].inode != inode->i_num || !de[1].inode ||
        strcmp(".", de[0].name) || strcmp("..", de[1].name))
    {
        printk("warning - bad directory on dev %04x\n", inode->i_dev);
        return 0;
    }

    nr = 2;
    de += 2;

    while (nr < len)
    {
        if ((void *)de >= (void *)(bh->b_data + BLOCK_SIZE))
        {
            brelse(bh);
            block = bmap(inode, nr / DIR_ENTRIES_PER_BLOCK);

            if (!block)
            {
                nr += DIR_ENTRIES_PER_BLOCK;
                continue;
            }

            if (!(bh = bread(inode->i_dev, block)))
                return 0;

            de = (struct dir_entry *)bh->b_data;
        }

        if (de->inode)
        {
            brelse(bh);
            return 0;
        }

        de++;
        nr++;
    }

    brelse(bh);

    return 1;
}

/**
 * sys_rmdir - 删除指定名称的目录。
 * 
 * 参数： name - 目录名(路径名)。
 * 返回：返回 0 表示成功，否则返回出错号。
 */
int sys_rmdir(const char *name)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    if (!suser())
        return -EPERM;

    if (!(dir = dir_namei(name, &namelen, &basename)))
        return -ENOENT;

    if (!namelen)
    {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE))
    {
        iput(dir);
        return -EPERM;
    }

    bh = find_entry(&dir, basename, namelen, &de);

    if (!bh)
    {
        iput(dir);
        return -ENOENT;
    }

    if (!(inode = iget(dir->i_dev, de->inode)))
    {
        iput(dir);
        brelse(bh);
        return -EPERM;
    }

    if ((dir->i_mode & S_ISVTX) && current->euid &&
        inode->i_uid != current->euid)
    {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }

    if (inode->i_dev != dir->i_dev || inode->i_count > 1)
    {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }

    if (inode == dir)
    { /* we may not delete ".", but "../dir" is ok */
        iput(inode);
        iput(dir);
        brelse(bh);
        return -EPERM;
    }

    if (!S_ISDIR(inode->i_mode))
    {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -ENOTDIR;
    }

    if (!empty_dir(inode))
    {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -ENOTEMPTY;
    }

    if (inode->i_nlinks != 2)
        printk("empty directory has nlink!=2 (%d)", inode->i_nlinks);

    de->inode = 0;
    bh->b_dirt = 1;
    brelse(bh);
    inode->i_nlinks = 0;
    inode->i_dirt = 1;
    dir->i_nlinks--;
    dir->i_ctime = dir->i_mtime = CURRENT_TIME;
    dir->i_dirt = 1;
    iput(dir);
    iput(inode);

    return 0;
}

/**
 * sys_unlink - 删除文件名以及可能也删除其相关的文件。
 * 
 * 从文件系统删除一个名字。如果是一个文件的最后一个连接，并且没有进程正打开
 * 该文件，则该文件也将被删除，并释放所占用的设备空间。
 * 
 * 参数：name - 文件名。
 * 返回：成功则返回0，否则返回出错号.
 */
int sys_unlink(const char *name)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    if (!(dir = dir_namei(name, &namelen, &basename)))
        return -ENOENT;

    if (!namelen)
    {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE))
    {
        iput(dir);
        return -EPERM;
    }

    bh = find_entry(&dir, basename, namelen, &de);

    if (!bh)
    {
        iput(dir);
        return -ENOENT;
    }

    if (!(inode = iget(dir->i_dev, de->inode)))
    {
        iput(dir);
        brelse(bh);
        return -ENOENT;
    }

    if ((dir->i_mode & S_ISVTX) && !suser() &&
        current->euid != inode->i_uid &&
        current->euid != dir->i_uid)
    {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }

    if (S_ISDIR(inode->i_mode))
    {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -EPERM;
    }

    if (!inode->i_nlinks)
    {
        printk("Deleting nonexistent file (%04x:%d), %d\n",
               inode->i_dev, inode->i_num, inode->i_nlinks);
        inode->i_nlinks = 1;
    }

    de->inode = 0;
    bh->b_dirt = 1;
    brelse(bh);
    inode->i_nlinks--;
    inode->i_dirt = 1;
    inode->i_ctime = CURRENT_TIME;
    iput(inode);
    iput(dir);

    return 0;
}

/**
 * sys_link - 为文件建立一个文件名。
 * 
 * 为一个已经存在的文件创建一个新连接(也称为硬连接 - hard link)。
 * 参数：oldname - 原路径名；newname - 新的路径名。
 * 返回：若成功则返回0，否则返回出错号.
 */
int sys_link(const char *oldname, const char *newname)
{
    struct dir_entry *de;
    struct m_inode *oldinode, *dir;
    struct buffer_head *bh;
    const char *basename;
    int namelen;

    oldinode = namei(oldname);

    if (!oldinode)
        return -ENOENT;

    if (S_ISDIR(oldinode->i_mode))
    {
        iput(oldinode);
        return -EPERM;
    }

    dir = dir_namei(newname, &namelen, &basename);

    if (!dir)
    {
        iput(oldinode);
        return -EACCES;
    }

    if (!namelen)
    {
        iput(oldinode);
        iput(dir);
        return -EPERM;
    }

    if (dir->i_dev != oldinode->i_dev)
    {
        iput(dir);
        iput(oldinode);
        return -EXDEV;
    }

    if (!permission(dir, MAY_WRITE))
    {
        iput(dir);
        iput(oldinode);
        return -EACCES;
    }

    bh = find_entry(&dir, basename, namelen, &de);

    if (bh)
    {
        brelse(bh);
        iput(dir);
        iput(oldinode);
        return -EEXIST;
    }

    bh = add_entry(dir, basename, namelen, &de);

    if (!bh)
    {
        iput(dir);
        iput(oldinode);
        return -ENOSPC;
    }

    de->inode = oldinode->i_num;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    oldinode->i_nlinks++;
    oldinode->i_ctime = CURRENT_TIME;
    oldinode->i_dirt = 1;
    iput(oldinode);

    return 0;
}
