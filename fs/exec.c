/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-开始的程序检测部分是由tytso实现的.
 */

/**
 * 需求时加载是于1991.12.1实现的 - 只需将执行文件头部分读进内存而无须
 * 将整个执行文件都加载进内存。执行文件的inode被放在当前进程的可执行字段中
 * ("current->executable")，而页异常会进行执行文件的实际加载操作以及清理工作.
 *
 * 我可以再一次自豪地说，linux经得起修改：只用了不到2小时的工作时间就完全
 * 实现了需求加载处理.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES 定义了新程序分配给参数和环境变量使用的内存最大页数。
 * 32页内存应该足够了，这使得环境和参数(env+arg)空间的总合达到128kB!
 */
#define MAX_ARG_PAGES           32

/*
 * create_tables()函数在新用户内存中解析环境变量和参数字符串，由此
 * 创建指针表，并将它们的地址放到"堆栈"上，然后返回新栈的指针值.
 */
/**
 * create_tables - 在新用户堆栈中创建环境和参数变量指针表。
 * 
 * 参数：p-以数据段为起点的参数和环境信息偏移指针；argc-参数个数；envc-环境变量数。
 * 返回：堆栈指针.
*/
static unsigned long *create_tables(char *p, int argc, int envc)
{
    unsigned long *argv, *envp;
    unsigned long *sp;

    sp = (unsigned long *)(0xfffffffc & (unsigned long)p);
    sp -= envc + 1;
    envp = sp;
    sp -= argc + 1;
    argv = sp;

    put_fs_long((unsigned long)envp, --sp);
    put_fs_long((unsigned long)argv, --sp);
    put_fs_long((unsigned long)argc, --sp);

    while (argc-- > 0)
    {
        put_fs_long((unsigned long)p, argv++);

        while (get_fs_byte(p++)) /* nothing */
            ;
    }

    put_fs_long(0, argv);

    while (envc-- > 0)
    {
        put_fs_long((unsigned long)p, envp++);
        while (get_fs_byte(p++)) /* nothing */
            ;
    }

    put_fs_long(0, envp);

    return sp;
}

/*
 * count()函数计算命令行参数/环境变量的个数.
 */
/**
 * count - 计算参数个数。
 * 参数：argv - 参数指针数组，最后一个指针项是NULL.
 * 返回：参数个数.
*/
static int count(char **argv)
{
    int i = 0;
    char **tmp;

    if (tmp = argv)
        while (get_fs_long((unsigned long *)(tmp++)))
            i++;

    return i;
}

/**
 * 'copy_string()'函数从用户内存空间拷贝参数和环境字符串到内核空闲页面内存中。
 * 这些已具有直接放到新用户内存中的格式.
 *
 * 由TYT(Tytso)于1991.12.24日修改，增加了from_kmem参数，该参数指明了字符串或
 * 字符串数组是来自用户段还是内核段:
 *
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 *
 * 我们是通过巧妙处理fs段寄存器来操作的。由于加载一个段寄存器代价太大，所以
 * 我们尽量避免调用set_fs()，除非实在必要.
 */
/**
 * copy_strings - 复制指定个数的参数字符串到参数和环境空间.
 * 
 * 参数：
 * argc - 欲添加的参数个数；
 * argv - 参数指针数组；
 * page - 参数和环境空间页面指针数组。
 * 
 * p-在参数表空间中的偏移指针，始终指向已复制串的头部；from_kmem-字符串来源标志。
 * 在do_execve()函数中，p初始化为指向参数表(128kB)空间的最后一个长字处，参数字符
 * 串是以堆栈操作方式逆向往其中复制存放的，因此p指针会始终指向参数字符串的头部。
 * 
 * 返回：参数和环境空间当前头部指针.
 */
static unsigned long copy_strings(int argc, char **argv, unsigned long *page,
                                  unsigned long p, int from_kmem)
{
    char *tmp, *pag;
    int len, offset = 0;
    unsigned long old_fs, new_fs;

    if (!p)
        return 0; /* bullet-proofing */

    new_fs = get_ds();
    old_fs = get_fs();

    if (from_kmem == 2)
        set_fs(new_fs);

    while (argc-- > 0)
    {
        if (from_kmem == 1)
            set_fs(new_fs);

        if (!(tmp = (char *)get_fs_long(((unsigned long *)argv) + argc)))
            panic("argc is wrong");

        if (from_kmem == 1)
            set_fs(old_fs);

        len = 0; /* remember zero-padding */

        do
        {
            len++;
        } while (get_fs_byte(tmp++));

        if (p - len < 0)
        { /* this shouldn't happen - 128kB */
            set_fs(old_fs);
            return 0;
        }

        while (len)
        {
            --p;
            --tmp;
            --len;

            if (--offset < 0)
            {
                offset = p % PAGE_SIZE;

                if (from_kmem == 2)
                    set_fs(old_fs);

                if (!(pag = (char *)page[p / PAGE_SIZE]) &&
                    !(pag = (char *)page[p / PAGE_SIZE] =
                          (unsigned long *)get_free_page()))
                    return 0;

                if (from_kmem == 2)
                    set_fs(new_fs);
            }
            *(pag + offset) = get_fs_byte(tmp);
        }
    }

    if (from_kmem == 2)
        set_fs(old_fs);

    return p;
}

/**
 * 修改局部描述符表中的描述符基址和段限长，并将参数和环境空间页面放置在数据段末端。
 * 
 * 参数：
 * text_size - 执行文件头部中a_text字段给出的代码段长度值；
 * page - 参数和环境空间页面指针数组.
 * 
 * 返回：数据段限长值(64MB).
 */
static unsigned long change_ldt(unsigned long text_size, unsigned long *page)
{
    unsigned long code_limit, data_limit, code_base, data_base;
    int i;

    /**
     * 根据执行文件头部a_text值，计算以页面长度为边界的代码段限长。
     * 并设置数据段长度为64MB.
     */
    code_limit = text_size + PAGE_SIZE - 1;
    code_limit &= 0xFFFFF000;
    data_limit = 0x4000000;

    /* 取当前进程中局部描述符表代码段描述符中代码段基址，代码段基址与数据段基址相同. */
    code_base = get_base(current->ldt[1]);
    data_base = code_base;

    /* 重新设置局部表中代码段和数据段描述符的基址和段限长. */
    set_base(current->ldt[1], code_base);
    set_limit(current->ldt[1], code_limit);
    set_base(current->ldt[2], data_base);
    set_limit(current->ldt[2], data_limit);

    /* 要确信fs段寄存器已指向新的数据段. */
    /* fs段寄存器中放入局部表数据段描述符的选择符(0x17). */
    __asm__("pushl $0x17\n\tpop %%fs" ::);

    /**
     * 将参数和环境空间已存放数据的页面(共可有MAX_ARG_PAGES页，128kB)放到
     * 数据段线性地址的末端。是调用函数put_page()进行操作的(mm/memory.c).
     */
    data_base += data_limit;

    for (i = MAX_ARG_PAGES - 1; i >= 0; i--)
    {
        data_base -= PAGE_SIZE;

        /* 如果该页面存在. */
        if (page[i])
            /* 就放置该页面. */
            put_page(page[i], data_base);
    }

    /* 最后返回数据段限长(64MB). */
    return data_limit;
}

/*
 * 'do_execve()'函数执行一个新程序.
 */
/**
 * do_execve - 系统中断调用函数。加载并执行子进程(其它程序).
 * 该函数系统中断调用(int 0x80)功能号__NR_execve 调用的函数。
 * 
 * 参数：
 * eip - 指向堆栈中调用系统中断的程序代码指针eip处，参见
 * kernel/system_call.s程序开始部分的说明；
 * tmp - 系统中断调用本函数时的返回地址，无用；
 * filename - 被执行程序文件名；
 * argv - 命令行参数指针数组；
 * envp - 环境变量指针数组。
 * 
 * 返回：如果调用成功，则不返回；否则设置出错号，并返回-1.
 */
int do_execve(unsigned long *eip, long tmp, char *filename,
              char **argv, char **envp)
{
    struct m_inode *inode;
    struct buffer_head *bh;
    struct exec ex;
    unsigned long page[MAX_ARG_PAGES];
    int i, argc, envc;
    int e_uid, e_gid;
    int retval;
    int sh_bang = 0;
    unsigned long p = PAGE_SIZE * MAX_ARG_PAGES - 4;

    if ((0xffff & eip[1]) != 0x000f)
        panic("execve called from supervisor mode");

    for (i = 0; i < MAX_ARG_PAGES; i++) /* clear page-table */
        page[i] = 0;

    if (!(inode = namei(filename))) /* get executables inode */
        return -ENOENT;

    argc = count(argv);
    envc = count(envp);

restart_interp:
    if (!S_ISREG(inode->i_mode))
    { /* must be regular file */
        retval = -EACCES;
        goto exec_error2;
    }

    i = inode->i_mode;
    e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
    e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;

    if (current->euid == inode->i_uid)
        i >>= 6;
    else if (current->egid == inode->i_gid)
        i >>= 3;

    if (!(i & 1) &&
        !((inode->i_mode & 0111) && suser()))
    {
        retval = -ENOEXEC;
        goto exec_error2;
    }

    if (!(bh = bread(inode->i_dev, inode->i_zone[0])))
    {
        retval = -EACCES;
        goto exec_error2;
    }

    ex = *((struct exec *)bh->b_data); /* read exec-header */

    if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang))
    {
        /*
         * This section does the #! interpretation.
         * Sorta complicated, but hopefully it will work.  -TYT
         */

        char buf[1023], *cp, *interp, *i_name, *i_arg;
        unsigned long old_fs;

        strncpy(buf, bh->b_data + 2, 1022);
        brelse(bh);
        iput(inode);
        buf[1022] = '\0';

        if (cp = strchr(buf, '\n'))
        {
            *cp = '\0';
            for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++)
                ;
        }

        if (!cp || *cp == '\0')
        {
            retval = -ENOEXEC; /* No interpreter name found */
            goto exec_error1;
        }

        interp = i_name = cp;
        i_arg = 0;

        for (; *cp && (*cp != ' ') && (*cp != '\t'); cp++)
        {
            if (*cp == '/')
                i_name = cp + 1;
        }

        if (*cp)
        {
            *cp++ = '\0';
            i_arg = cp;
        }

        /*
         * OK, we've parsed out the interpreter name and
         * (optional) argument.
         */
        if (sh_bang++ == 0)
        {
            p = copy_strings(envc, envp, page, p, 0);
            p = copy_strings(--argc, argv + 1, page, p, 0);
        }

        /*
         * Splice in (1) the interpreter's name for argv[0]
         *           (2) (optional) argument to interpreter
         *           (3) filename of shell script
         *
         * This is done in reverse order, because of how the
         * user environment and arguments are stored.
         */
        p = copy_strings(1, &filename, page, p, 1);
        argc++;

        if (i_arg)
        {
            p = copy_strings(1, &i_arg, page, p, 2);
            argc++;
        }

        p = copy_strings(1, &i_name, page, p, 2);
        argc++;

        if (!p)
        {
            retval = -ENOMEM;
            goto exec_error1;
        }

        /*
         * OK, now restart the process with the interpreter's inode.
         */
        old_fs = get_fs();
        set_fs(get_ds());

        if (!(inode = namei(interp)))
        { /* get executables inode */
            set_fs(old_fs);
            retval = -ENOENT;
            goto exec_error1;
        }

        set_fs(old_fs);

        goto restart_interp;
    }

    brelse(bh);

    if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
        ex.a_text + ex.a_data + ex.a_bss > 0x3000000 ||
        inode->i_size < ex.a_text + ex.a_data + ex.a_syms + N_TXTOFF(ex))
    {
        retval = -ENOEXEC;
        goto exec_error2;
    }

    if (N_TXTOFF(ex) != BLOCK_SIZE)
    {
        printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
        retval = -ENOEXEC;
        goto exec_error2;
    }

    if (!sh_bang)
    {
        p = copy_strings(envc, envp, page, p, 0);
        p = copy_strings(argc, argv, page, p, 0);
        if (!p)
        {
            retval = -ENOMEM;
            goto exec_error2;
        }
    }

    /* OK, This is the point of no return */
    if (current->executable)
        iput(current->executable);

    current->executable = inode;

    for (i = 0; i < 32; i++)
        current->sigaction[i].sa_handler = NULL;

    for (i = 0; i < NR_OPEN; i++)
        if ((current->close_on_exec >> i) & 1)
            sys_close(i);

    current->close_on_exec = 0;
    free_page_tables(get_base(current->ldt[1]), get_limit(0x0f));
    free_page_tables(get_base(current->ldt[2]), get_limit(0x17));

    if (last_task_used_math == current)
        last_task_used_math = NULL;

    current->used_math = 0;
    p += change_ldt(ex.a_text, page) - MAX_ARG_PAGES * PAGE_SIZE;
    p = (unsigned long)create_tables((char *)p, argc, envc);
    current->brk = ex.a_bss +
                   (current->end_data = ex.a_data +
                                        (current->end_code = ex.a_text));
    current->start_stack = p & 0xfffff000;
    current->euid = e_uid;
    current->egid = e_gid;
    i = ex.a_text + ex.a_data;

    while (i & 0xfff)
        put_fs_byte(0, (char *)(i++));

    eip[0] = ex.a_entry; /* eip, magic happens :-) */
    eip[3] = p;          /* stack pointer */

    return 0;

exec_error2:
    iput(inode);

exec_error1:
    for (i = 0; i < MAX_ARG_PAGES; i++)
        free_page(page[i]);

    return (retval);
}
