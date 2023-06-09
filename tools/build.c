/*
 *  linux/tools/build.c
 *
 *  (C) 1991  Linus Torvalds
 */

/**
 * 该程序从三个不同的程序中创建磁盘映象文件:
 *
 * - bootsect: 该文件的8086机器码最长为510字节，用于加载其它程序.
 * - setup: 该文件的8086机器码最长为4个磁盘扇区，用于设置系统参数.
 * - system: 实际系统的80386代码.
 *
 * 该程序首先检查所有程序模块的类型是否正确，并将检查结果在终端上显示出来，
 * 然后删除模块头部并扩充大正确的长度。该程序也会将一些系统数据写到stderr.
 */

/*
 * tytso对该程序作了修改，以允许指定根文件设备.
 */

#include <stdio.h>      /* fprintf */
#include <string.h>
#include <stdlib.h>     /* contains exit */
#include <sys/types.h>  /* unistd.h needs this */
#include <sys/stat.h>
#include <linux/fs.h>
#include <unistd.h>     /* contains read/write */
#include <fcntl.h>

/* minix二进制模块头部长度为32字节. */
#define MINIX_HEADER            32
/* GCC头部信息长度为1024字节. */
#define GCC_HEADER              1024
/* system文件最长节数(字节数为SYS_SIZE*16=128KB). */
#define SYS_SIZE                0x2000
/* 默认根设备主设备号 - 3(硬盘). */
#define DEFAULT_MAJOR_ROOT      3
/* 默认根设备次设备号 - 6(第2个硬盘的第1分区). */
#define DEFAULT_MINOR_ROOT      6

/* 下面指定setup模块占的最大扇区数：不要改变该值，除非也改变bootsect等相应文件. */
/* setup最大长度为4个扇区(4*512字节). */
#define SETUP_SECTS             4
/* 用于出错时显示语句中表示扇区数. */
#define STRINGIFY(x)            #x

/**
 * 显示出错信息，并终止程序.
 */
void die(char *str)
{
    fprintf(stderr, "%s\n", str);
    exit(1);
}

/**
 * 显示程序使用方法，并退出.
 */
void usage(void)
{
    die("Usage: build bootsect setup system [rootdev] [> image]");
}

int main(int argc, char **argv)
{
    int i, c, id;
    char buf[1024];
    char major_root, minor_root;
    struct stat sb;

    /* 如果程序命令行参数不是4或5个，则显示程序用法并退出. */
    if ((argc != 4) && (argc != 5))
        usage();

    /* 如果参数是5个，则说明带有根设备名. */
    if (argc == 5)
    {
        /* 如果根设备名是软盘("FLOPPY")，则取该设备文件的状态信息，若出错则显示信息，退出. */
        if (strcmp(argv[4], "FLOPPY"))
        {
            if (stat(argv[4], &sb))
            {
                perror(argv[4]);
                die("Couldn't stat root device.");
            }

            /* 若成功则取该设备名状态结构中的主设备号和次设备号. */
            major_root = MAJOR(sb.st_rdev);
            minor_root = MINOR(sb.st_rdev);
        }
        else
        {
            /* 否则让主设备号和次设备号取0. */
            major_root = 0;
            minor_root = 0;
        }
    }
    else
    {
        /* 若参数只有4个，则让主设备号和次设备号等于系统默认的根设备. */
        major_root = DEFAULT_MAJOR_ROOT;
        minor_root = DEFAULT_MINOR_ROOT;
    }

    /* 在标准错误终端上显示所选择的根设备主、次设备号. */
    fprintf(stderr, "Root device is (%d, %d)\n", major_root, minor_root);

    /* 如果主设备号不等于2(软盘)或3(硬盘)，也不等于0(取系统默认根设备)，则显示出错信息，退出. */
    if ((major_root != 2) && (major_root != 3) &&
        (major_root != 0))
    {
        fprintf(stderr, "Illegal root device (major = %d)\n",
                major_root);
        die("Bad root device --- major #");
    }

    /* 初始化buf缓冲区，全置0. */
    for (i = 0; i < sizeof buf; i++)
        buf[i] = 0;

    /* 以只读方式打开参数1指定的文件(bootsect)，若出错则显示出错信息，退出. */
    if ((id = open(argv[1], O_RDONLY, 0)) < 0)
        die("Unable to open 'boot'");

    /* 读取文件中的minix执行头部信息(参见列表后说明)，若出错则显示出错信息，退出. */
    if (read(id, buf, MINIX_HEADER) != MINIX_HEADER)
        die("Unable to read header of 'boot'");

    /* 0x0301 - minix头部a_magic魔数；0x10 - a_flag可执行；0x04 - a_cpu, Intel 8086机器码. */
    if (((long *)buf)[0] != 0x04100301)
        die("Non-Minix header of 'boot'");

    /* 判断头部长度字段a_hdrlen(字节)是否正确。(后三字节正好没有用，是0). */
    if (((long *)buf)[1] != MINIX_HEADER)
        die("Non-Minix header of 'boot'");

    /* 判断数据段长a_data字段(long)内容是否为0. */
    if (((long *)buf)[3] != 0)
        die("Illegal data segment in 'boot'");

    /* 判断堆a_bss字段(long)内容是否为0. */
    if (((long *)buf)[4] != 0)
        die("Illegal bss in 'boot'");

    /* 判断执行点a_entry字段(long)内容是否为0. */
    if (((long *)buf)[5] != 0)
        die("Non-Minix header of 'boot'");

    /* 判断符号表长字段a_sym的内容是否为0. */
    if (((long *)buf)[7] != 0)
        die("Illegal symbol table in 'boot'");

    /* 读取实际代码数据，应该返回读取字节数为512字节. */
    i = read(id, buf, sizeof buf);
    fprintf(stderr, "Boot sector %d bytes.\n", i);

    if (i != 512)
        die("Boot block must be exactly 512 bytes");

    /* 判断boot块0x510处是否有可引导标志0xAA55. */
    if ((*(unsigned short *)(buf + 510)) != 0xAA55)
        die("Boot block hasn't got boot flag (0xAA55)");

    /* 引导块的508，509偏移处存放的是根设备号. */
    buf[508] = (char)minor_root;
    buf[509] = (char)major_root;

    /* 将该boot块512字节的数据写到标准输出stdout，若写出字节数不对，则显示出错信息，退出. */
    i = write(1, buf, 512);

    if (i != 512)
        die("Write call failed");

    /* 最后关闭bootsect模块文件. */
    close(id);

    /* 现在开始处理setup模块。首先以只读方式打开该模块，若出错则显示出错信息，退出. */
    if ((id = open(argv[2], O_RDONLY, 0)) < 0)
        die("Unable to open 'setup'");

    /* 读取该文件中的minix执行头部信息(32字节)，若出错则显示出错信息，退出. */
    if (read(id, buf, MINIX_HEADER) != MINIX_HEADER)
        die("Unable to read header of 'setup'");

    /* 0x0301 - minix头部a_magic魔数；0x10 - a_flag可执行；0x04 - a_cpu, Intel 8086机器码. */
    if (((long *)buf)[0] != 0x04100301)
        die("Non-Minix header of 'setup'");

    /* 判断头部长度字段a_hdrlen(字节)是否正确。(后三字节正好没有用，是0). */
    if (((long *)buf)[1] != MINIX_HEADER)
        die("Non-Minix header of 'setup'");

    /* 判断数据段长a_data字段(long)内容是否为0. */
    if (((long *)buf)[3] != 0)
        die("Illegal data segment in 'setup'");

    /* 判断堆a_bss字段(long)内容是否为0. */
    if (((long *)buf)[4] != 0)
        die("Illegal bss in 'setup'");

    /* 判断执行点a_entry字段(long)内容是否为0. */
    if (((long *)buf)[5] != 0)
        die("Non-Minix header of 'setup'");

    /* 判断符号表长字段a_sym的内容是否为0. */
    if (((long *)buf)[7] != 0)
        die("Illegal symbol table in 'setup'");

    /* 读取随后的执行代码数据，并写到标准输出 stdout. */
    for (i = 0; (c = read(id, buf, sizeof buf)) > 0; i += c)
        if (write(1, buf, c) != c)
            die("Write call failed");

    /* 关闭setup模块文件. */
    close(id);

    /* 若setup模块长度大于4个扇区，则算出错，显示出错信息，退出. */
    if (i > SETUP_SECTS * 512)
        die("Setup exceeds " STRINGIFY(SETUP_SECTS) " sectors - rewrite build/boot/setup");

    /* 在标准错误stderr显示setup文件的长度值. */
    fprintf(stderr, "Setup is %d bytes.\n", i);

    /* 将缓冲区buf清零. */
    for (c = 0; c < sizeof(buf); c++)
        buf[c] = '\0';

    /* 若setup长度小于4*512字节，则用\0将setup填足为4*512字节. */
    while (i < SETUP_SECTS * 512)
    {
        c = SETUP_SECTS * 512 - i;

        if (c > sizeof(buf))
            c = sizeof(buf);

        if (write(1, buf, c) != c)
            die("Write call failed");
        i += c;
    }

    /* 下面处理system模块。首先以只读方式打开该文件. */
    if ((id = open(argv[3], O_RDONLY, 0)) < 0)
        die("Unable to open 'system'");

    /* system模块是GCC格式的文件，先读取GCC格式的头部结构信息(linux的执行文件也采用该格式). */
    if (read(id, buf, GCC_HEADER) != GCC_HEADER)
        die("Unable to read header of 'system'");

    /* 该结构中的执行代码入口点字段a_entry值应为0. */
    if (((long *)buf)[5] != 0)
        die("Non-GCC header of 'system'");

    /* 读取随后的执行代码数据，并写到标准输出stdout. */
    for (i = 0; (c = read(id, buf, sizeof buf)) > 0; i += c)
        if (write(1, buf, c) != c)
            die("Write call failed");

    /* 关闭system文件，并向stderr上打印system的字节数. */
    close(id);
    fprintf(stderr, "System is %d bytes.\n", i);

    /* 若system代码数据长度超过SYS_SIZE节(或128KB字节)，则显示出错信息，退出. */
    if (i > SYS_SIZE * 16)
        die("System is too big");

    return (0);
}
