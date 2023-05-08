/*
 *  linux/kernel/console.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *	console.c
 *
 * 该模块实现控制台输入输出功能:
 *  'void con_init(void)'
 *  'void con_write(struct tty_queue * queue)'
 * 希望这是一个非常完整的VT102实现.
 *
 * Beeping thanks to John T Kohl.
 */

/*
 * 注意!!! 我们有时短暂地禁止和允许中断(在将一个字(word)放到视频IO)，但即使
 * 对于键盘中断这也是可以工作的。因为我们使用陷阱门，所以我们知道在获得一个
 * 键盘中断时中断是不允许的。希望一切均正常.
 */

/*
 * 检测不同显示卡的代码大多数是Galen Hunt编写的,
 * <g-hunt@ee.utah.edu>
 */

#include <linux/sched.h>
#include <linux/tty.h>
#include <asm/io.h>
#include <asm/system.h>

/*
 * 这些是设置子程序setup在引导启动系统时设置的参数:
 */
/* 参见对boot/setup.s的注释，和setup程序读取并保留的参数表. */
#define ORIG_X                  (*(unsigned char *)0x90000)     /* 光标列号. */
#define ORIG_Y                  (*(unsigned char *)0x90001)     /* 光标行号. */
#define ORIG_VIDEO_PAGE         (*(unsigned short *)0x90004)    /* 显示页面. */
#define ORIG_VIDEO_MODE         ((*(unsigned short *)0x90006) & 0xff) /* 显示模式. */
#define ORIG_VIDEO_COLS         (((*(unsigned short *)0x90006) & 0xff00) >> 8) /* 字符列数. */
#define ORIG_VIDEO_LINES        (25)                            /* 显示行数. */
#define ORIG_VIDEO_EGA_AX       (*(unsigned short *)0x90008)
#define ORIG_VIDEO_EGA_BX       (*(unsigned short *)0x9000a)    /* 显示内存大小和色彩模式. */
#define ORIG_VIDEO_EGA_CX       (*(unsigned short *)0x9000c)    /* 显示卡特性参数. */

#define VIDEO_TYPE_MDA          0x10    /* 单色文本. */
#define VIDEO_TYPE_CGA          0x11    /* CGA显示器. */
#define VIDEO_TYPE_EGAM         0x20    /* EGA/VGA单色. */
#define VIDEO_TYPE_EGAC         0x21    /* EGA/VGA彩色. */

#define NPAR                    16

extern void keyboard_interrupt(void);

static unsigned char video_type;        /* 使用的显示类型. */
static unsigned long video_num_columns; /* 屏幕文本列数. */
static unsigned long video_size_row;    /* 每行使用的字节数. */
static unsigned long video_num_lines;   /* 屏幕文本行数. */
static unsigned char video_page;        /* 初始显示页面. */
static unsigned long video_mem_start;   /* 显示内存起始地址. */
static unsigned long video_mem_end;     /* 显示内存结束(末端)地址. */
static unsigned short video_port_reg;   /* 显示控制索引寄存器端口. */
static unsigned short video_port_val;   /* 显示控制数据寄存器端口. */
static unsigned short video_erase_char; /* 擦除字符属性与字符(0x0720). */

/* 以下这些变量用于屏幕卷屏操作. */
static unsigned long origin;            /* 用于EGA/VGA快速滚屏(滚屏起始内存地址). */
static unsigned long scr_end;           /* 用于EGA/VGA快速滚屏(滚屏末端内存地址). */
static unsigned long pos;               /* 当前光标对应的显示内存位置. */
static unsigned long x, y;              /* 当前光标位置. */
static unsigned long top, bottom;       /* 滚动时顶行行号；底行行号. */
/* state用于标明处理ESC转义序列时的当前步骤。npar,par[]用于存放ESC序列的中间处理参数. */
static unsigned long state = 0;         /* ANSI转义字符序列处理状态. */
static unsigned long npar, par[NPAR];   /* ANSI转义字符序列参数个数和参数数组. */
static unsigned long ques = 0;
static unsigned char attr = 0x07;       /* 字符属性(黑底白字). */

/* 系统蜂鸣函数. */
static void sysbeep(void);

/*
 * 下面是终端回应ESC-Z或csi0c请求的应答(=vt100响应).
 */
/* csi - 控制序列引导码(Control Sequence Introducer). */
#define RESPONSE                "\033[?1;2c"

/* 注意！gotoxy函数认为x==video_num_columns，这是正确的. */
/**
 * gotoxy - 跟踪光标当前位置.
 * 
 * 参数：new_x - 光标所在列号；new_y - 光标所在行号。
 * 更新当前光标位置变量x,y，并修正pos指向光标在显示内存中的对应位置.
 */
static inline void gotoxy(unsigned int new_x, unsigned int new_y)
{
    /**
     * 如果输入的光标行号超出显示器列数，或者光标行号超出显示的最大行数，
     * 则退出.
     */
    if (new_x > video_num_columns || new_y >= video_num_lines)
        return;

    /* 更新当前光标变量；更新光标位置对应的在显示内存中位置变量pos. */
    x = new_x;
    y = new_y;
    pos = origin + y * video_size_row + (x << 1);
}

/**
 * set_origin - 设置滚屏起始显示内存地址.
 */
static inline void set_origin(void)
{
    cli();

    /**
     * 首先选择显示控制数据寄存器r12，然后写入卷屏起始地址高字节。向右移动
     * 9位，表示向右移动8位，再除以2(2字节代表屏幕上1字符)。是相对于默认
     * 显示内存操作的。
     */
    outb_p(12, video_port_reg);
    outb_p(0xff & ((origin - video_mem_start) >> 9), video_port_val);

    /**
     * 再选择显示控制数据寄存器r13，然后写入卷屏起始地址底字节。向右移动
     * 1位表示除以2.
     */
    outb_p(13, video_port_reg);
    outb_p(0xff & ((origin - video_mem_start) >> 1), video_port_val);

    sti();
}

/**
 * scrup - 向上卷动一行(屏幕窗口向下移动).
 *
 * 将屏幕窗口向下移动一行.
 */
static void scrup(void)
{
    /* 如果显示类型是EGA，则执行以下操作. */
    if (video_type == VIDEO_TYPE_EGAC || video_type == VIDEO_TYPE_EGAM)
    {
        /**
         * 如果移动起始行top=0，移动最底行 bottom=video_num_lines=25，
         * 则表示整屏窗口向下移动。
         */
        if (!top && bottom == video_num_lines)
        {
            /**
             * 调整屏幕显示对应内存的起始位置指针origin为向下移一行屏幕字符
             * 对应的内存位置，同时也调整当前光标对应的内存位置以及屏幕末行
             * 末端字符指针scr_end的位置.
             */
            origin += video_size_row;
            pos += video_size_row;
            scr_end += video_size_row;

            /**
             * 如果屏幕末端最后一个显示字符所对应的显示内存指针scr_end超出了
             * 实际显示内存的末端，则将屏幕内容内存数据移动到显示内存的起始位置
             * video_mem_start处，并在出现的新行上填入空格字符.
             */
            if (scr_end > video_mem_end)
            {
                /**
                 * %0 - eax(擦除字符+属性)；
                 * %1 - ecx((显示器字符行数-1)所对应的字符数/2，是以长字移动)；
                 * %2 - edi(显示内存起始位置 video_mem_start)；
                 * %3 - esi(屏幕内容对应的内存起始位置origin).
                 * 移动方向：[edi]??[esi]，移动 ecx 个长字.
                 */
                __asm__("cld\n\t"       /* 清方向位. */
                        "rep\n\t"       /* 重复操作，将当前屏幕内存数据. */
                        "movsl\n\t"     /* 移动到显示内存起始处. */
                        "movl _video_num_columns,%1\n\t" /* ecx=1行字符数. */
                        "rep\n\t"       /* 在新行上填入空格字符. */
                        "stosw" ::"a"(video_erase_char),
                        "c"((video_num_lines - 1) * video_num_columns >> 1),
                        "D"(video_mem_start),
                        "S"(origin)
                        : "cx", "di", "si");

                /**
                 * 根据屏幕内存数据移动后的情况，重新调整当前屏幕对应内存的起始指针、
                 * 光标位置指针和屏幕末端对应内存指针scr_end.
                 */
                scr_end -= origin - video_mem_start;
                pos -= origin - video_mem_start;
                origin = video_mem_start;
            }
            /**
             * 如果调整后的屏幕末端对应的内存指针scr_end没有超出显示内存的末端video_mem_end，
             * 则只需在新行上填入擦除字符(空格字符).
             * %0 - eax(擦除字符+属性)；
             * %1 - ecx(显示器字符行数)；
             * %2 - edi(屏幕对应内存最后一行开始处)；
             */
            else
            {
                __asm__("cld\n\t"       /* 清方向位. */
                        "rep\n\t"       /* 重复操作，在新出现行上. */
                        "stosw" ::"a"(video_erase_char), /* 填入擦除字符(空格字符). */
                        "c"(video_num_columns),
                        "D"(scr_end - video_size_row)
                        : "cx", "di");
            }

            /* 向显示控制器中写入新的屏幕内容对应的内存起始位置值. */
            set_origin();
        }
        /**
         * 否则表示不是整屏移动。也即表示从指定行top开始的所有行向上移动1行(删除1行)。
         * 此时直接将屏幕从指定行top到屏幕末端所有行对应的显示内存数据向上移动1行，
         * 并在新出现的行上填入擦除字符。
         * %0-eax(擦除字符+属性)；
         * %1-ecx(top 行下 1 行开始到屏幕末行的行数所对应的内存长字数)；
         * %2-edi(top 行所处的内存位置)；
         * %3-esi(top+1 行所处的内存位置).
         */
        else
        {
            __asm__("cld\n\t"           /* 清方向位. */
                    "rep\n\t"           /* 循环操作，将top+1到bottom行. */
                    "movsl\n\t"         /* 所对应的内存块移到top行开始处. */
                    "movl _video_num_columns,%%ecx\n\t" /* ecx = 1行字符数. */
                    "rep\n\t"           /* 在新行上填入擦除字符. */
                    "stosw" ::"a"(video_erase_char),
                    "c"((bottom - top - 1) * video_num_columns >> 1),
                    "D"(origin + video_size_row * top),
                    "S"(origin + video_size_row * (top + 1))
                    : "cx", "di", "si");
        }
    }
    /**
     * 如果显示类型不是EGA(是MDA)，则执行下面移动操作。因为MDA显示控制卡会自动调整
     * 超出显示范围的情况，也即会自动翻卷指针，所以这里不对屏幕内容对应内存超出显示
     * 内存的情况单独处理。处理方法与EGA非整屏移动情况完全一样.
     */
    else /* Not EGA/VGA */
    {
        __asm__("cld\n\t"
                "rep\n\t"
                "movsl\n\t"
                "movl _video_num_columns,%%ecx\n\t"
                "rep\n\t"
                "stosw" ::"a"(video_erase_char),
                "c"((bottom - top - 1) * video_num_columns >> 1),
                "D"(origin + video_size_row * top),
                "S"(origin + video_size_row * (top + 1))
                : "cx", "di", "si");
    }
}

/**
 * scrdown - 向下卷动一行(屏幕窗口向上移动).
 * 
 * 将屏幕窗口向上移动一行，屏幕显示的内容向下移动1行，在被移动开始行的
 * 上方出现一新行。处理方法与scrup()相似，只是为了在移动显示内存数据时
 * 不出现数据覆盖错误情况，复制是以反方向进行的，也即从屏幕倒数第2行的
 * 最后一个字符开始复制.
 */
static void scrdown(void)
{
    if (video_type == VIDEO_TYPE_EGAC || video_type == VIDEO_TYPE_EGAM)
    {
        __asm__("std\n\t"
                "rep\n\t"
                "movsl\n\t"
                "addl $2,%%edi\n\t" /* %edi has been decremented by 4 */
                "movl _video_num_columns,%%ecx\n\t"
                "rep\n\t"
                "stosw" ::"a"(video_erase_char),
                "c"((bottom - top - 1) * video_num_columns >> 1),
                "D"(origin + video_size_row * bottom - 4),
                "S"(origin + video_size_row * (bottom - 1) - 4)
                : "ax", "cx", "di", "si");
    }
    else /* Not EGA/VGA */
    {
        __asm__("std\n\t"
                "rep\n\t"
                "movsl\n\t"
                "addl $2,%%edi\n\t" /* %edi has been decremented by 4 */
                "movl _video_num_columns,%%ecx\n\t"
                "rep\n\t"
                "stosw" ::"a"(video_erase_char),
                "c"((bottom - top - 1) * video_num_columns >> 1),
                "D"(origin + video_size_row * bottom - 4),
                "S"(origin + video_size_row * (bottom - 1) - 4)
                : "ax", "cx", "di", "si");
    }
}

/**
 * lf - 光标位置下移一行(lf - line feed换行).
 */
static void lf(void)
{
    if (y + 1 < bottom)
    {
        y++;
        pos += video_size_row;
        return;
    }

    scrup();
}

/**
 * ri - 光标上移一行(ri - reverse line feed反向换行).
 */
static void ri(void)
{
    if (y > top)
    {
        y--;
        pos -= video_size_row;
        return;
    }

    scrdown();
}

/**
 * cr - 光标回到第1列(0列)左端(cr - carriage return回车).
 */
static void cr(void)
{
    pos -= x << 1;
    x = 0;
}

/**
 * del - 擦除光标前一字符(用空格替代)(del - delete删除).
 */
static void del(void)
{
    if (x)
    {
        pos -= 2;
        x--;
        *(unsigned short *)pos = video_erase_char;
    }
}

/**
 * csi_J - 删除屏幕上与光标位置相关的部分，以屏幕为单位。
 * csi - 控制序列引导码(ControlSequenceIntroducer).
 * ANSI 转义序列：'ESC [sJ'(s = 0删除光标到屏幕底端；1删除屏幕开始
 * 到光标处；2整屏删除)。
 * 参数：par - 对应上面s.
 */
static void csi_J(int par)
{
    long count __asm__("cx");
    long start __asm__("di");

    switch (par)
    {
    case 0: /* erase from cursor to end of display */
        count = (scr_end - pos) >> 1;
        start = pos;
        break;
    case 1: /* erase from start to cursor */
        count = (pos - origin) >> 1;
        start = origin;
        break;
    case 2: /* erase whole display */
        count = video_num_columns * video_num_lines;
        start = origin;
        break;
    default:
        return;
    }

    __asm__("cld\n\t"
            "rep\n\t"
            "stosw\n\t" ::"c"(count),
            "D"(start), "a"(video_erase_char)
            : "cx", "di");
}

/**
 * csi_K - 删除行内与光标位置相关的部分，以一行为单位.
 * ANSI转义字符序列：'ESC [sK'(s = 0删除到行尾；1从开始删除；
 * 2整行都删除).
 */
static void csi_K(int par)
{
    long count __asm__("cx");
    long start __asm__("di");

    switch (par)
    {
    case 0: /* erase from cursor to end of line */
        if (x >= video_num_columns)
            return;
        count = video_num_columns - x;
        start = pos;
        break;
    case 1: /* erase from start of line to cursor */
        start = pos - (x << 1);
        count = (x < video_num_columns) ? x : video_num_columns;
        break;
    case 2: /* erase whole line */
        start = pos - (x << 1);
        count = video_num_columns;
        break;
    default:
        return;
    }

    __asm__("cld\n\t"
            "rep\n\t"
            "stosw\n\t" ::"c"(count),
            "D"(start), "a"(video_erase_char)
            : "cx", "di");
}

/**
 * csi_m - 允许翻译(重显)(允许重新设置字符显示方式，比如加粗、加下划线、
 * 闪烁、反显等).
 * ANSI 转义字符序列：'ESC [nm'。n = 0 正常显示；1加粗；4加下划线；
 * 7反显；27正常显示.
 */
void csi_m(void)
{
    int i;

    for (i = 0; i <= npar; i++)
        switch (par[i])
        {
        case 0:
            attr = 0x07;
            break;
        case 1:
            attr = 0x0f;
            break;
        case 4:
            attr = 0x0f;
            break;
        case 7:
            attr = 0x70;
            break;
        case 27:
            attr = 0x07;
            break;
        }
}

/**
 * set_cursor - 根据设置显示光标.
 * 根据显示内存光标对应位置pos，设置显示控制器光标的显示位置.
 */
static inline void set_cursor(void)
{
    cli();
    outb_p(14, video_port_reg);
    outb_p(0xff & ((pos - video_mem_start) >> 9), video_port_val);
    outb_p(15, video_port_reg);
    outb_p(0xff & ((pos - video_mem_start) >> 1), video_port_val);
    sti();
}

/**
 * respond - 发送对终端VT100的响应序列.
 * 将响应序列放入读缓冲队列中.
 */
static void respond(struct tty_struct *tty)
{
    char *p = RESPONSE;

    cli();

    while (*p)
    {
        PUTCH(*p, tty->read_q);
        p++;
    }

    sti();
    copy_to_cooked(tty);
}

/**
 * insert_char - 在光标处插入一空格字符.
 */
static void insert_char(void)
{
    int i = x;
    unsigned short tmp, old = video_erase_char;
    unsigned short *p = (unsigned short *)pos;

    while (i++ < video_num_columns)
    {
        tmp = *p;
        *p = old;
        old = tmp;
        p++;
    }
}

/**
 * insert_line - 在光标处插入一行(则光标将处在新的空行上).
 * 将屏幕从光标所在行到屏幕底向下卷动一行.
 */
static void insert_line(void)
{
    int oldtop, oldbottom;

    oldtop = top;
    oldbottom = bottom;
    top = y;
    bottom = video_num_lines;
    scrdown();
    top = oldtop;
    bottom = oldbottom;
}

/**
 * delete_char - 删除光标处的一个字符.
 */
static void delete_char(void)
{
    int i;
    unsigned short *p = (unsigned short *)pos;

    if (x >= video_num_columns)
        return;

    i = x;

    while (++i < video_num_columns)
    {
        *p = *(p + 1);
        p++;
    }

    *p = video_erase_char;
}

/**
 * delete_line - 删除光标所在行.
 * 从光标所在行开始屏幕内容上卷一行.
 */
static void delete_line(void)
{
    int oldtop, oldbottom;

    oldtop = top;
    oldbottom = bottom;
    top = y;
    bottom = video_num_lines;
    scrup();
    top = oldtop;
    bottom = oldbottom;
}

/**
 * csi_at - 在光标处插入nr个字符.
 * ANSI 转义字符序列：'ESC [n@ '。
 * 参数nr = 上面n.
 */
static void csi_at(unsigned int nr)
{
    if (nr > video_num_columns)
        nr = video_num_columns;
    else if (!nr)
        nr = 1;

    while (nr--)
        insert_char();
}

/**
 * csi_L - 在光标位置处插入nr行.
 * ANSI转义字符序列'ESC [nL'.
 */
static void csi_L(unsigned int nr)
{
    if (nr > video_num_lines)
        nr = video_num_lines;
    else if (!nr)
        nr = 1;

    while (nr--)
        insert_line();
}

/**
 * csi_P - 删除光标处的nr个字符.
 * ANSI转义序列：'ESC [nP'.
 */
static void csi_P(unsigned int nr)
{
    if (nr > video_num_columns)
        nr = video_num_columns;
    else if (!nr)
        nr = 1;

    while (nr--)
        delete_char();
}

/**
 * csi_M - 删除光标处的nr行.
 * ANSI转义序列：'ESC [nM'.
 */
static void csi_M(unsigned int nr)
{
    if (nr > video_num_lines)
        nr = video_num_lines;
    else if (!nr)
        nr = 1;

    while (nr--)
        delete_line();
}

/* 保存的光标列号. */
static int saved_x = 0;
/* 保存的光标行号. */
static int saved_y = 0;

/**
 * save_cur - 保存当前光标位置.
 */
static void save_cur(void)
{
    saved_x = x;
    saved_y = y;
}

/**
 * restore_cur - 恢复保存的光标位置.
 */
static void restore_cur(void)
{
    gotoxy(saved_x, saved_y);
}

/**
 * con_write - 控制台写函数.
 * 从终端对应的tty写缓冲队列中取字符，并显示在屏幕上.
 */
void con_write(struct tty_struct *tty)
{
    int nr;
    char c;

    nr = CHARS(tty->write_q);
    while (nr--)
    {
        GETCH(tty->write_q, c);

        switch (state)
        {
        case 0:
            if (c > 31 && c < 127)
            {
                if (x >= video_num_columns)
                {
                    x -= video_num_columns;
                    pos -= video_size_row;
                    lf();
                }
                __asm__("movb _attr,%%ah\n\t"
                        "movw %%ax,%1\n\t" ::"a"(c),
                        "m"(*(short *)pos)
                        : "ax");
                pos += 2;
                x++;
            }
            else if (c == 27)
                state = 1;
            else if (c == 10 || c == 11 || c == 12)
                lf();
            else if (c == 13)
                cr();
            else if (c == ERASE_CHAR(tty))
                del();
            else if (c == 8)
            {
                if (x)
                {
                    x--;
                    pos -= 2;
                }
            }
            else if (c == 9)
            {
                c = 8 - (x & 7);
                x += c;
                pos += c << 1;
                if (x > video_num_columns)
                {
                    x -= video_num_columns;
                    pos -= video_size_row;
                    lf();
                }
                c = 9;
            }
            else if (c == 7)
                sysbeep();
            break;

        case 1:
            state = 0;
            if (c == '[')
                state = 2;
            else if (c == 'E')
                gotoxy(0, y + 1);
            else if (c == 'M')
                ri();
            else if (c == 'D')
                lf();
            else if (c == 'Z')
                respond(tty);
            else if (x == '7')
                save_cur();
            else if (x == '8')
                restore_cur();
            break;

        case 2:
            for (npar = 0; npar < NPAR; npar++)
                par[npar] = 0;
            npar = 0;
            state = 3;

            if (ques = (c == '?'))
                break;

        case 3:
            if (c == ';' && npar < NPAR - 1)
            {
                npar++;
                break;
            }
            else if (c >= '0' && c <= '9')
            {
                par[npar] = 10 * par[npar] + c - '0';
                break;
            }
            else
                state = 4;

        case 4:
            state = 0;

            switch (c)
            {
            case 'G':
            case '`':
                if (par[0])
                    par[0]--;
                gotoxy(par[0], y);
                break;

            case 'A':
                if (!par[0])
                    par[0]++;
                gotoxy(x, y - par[0]);
                break;

            case 'B':
            case 'e':
                if (!par[0])
                    par[0]++;
                gotoxy(x, y + par[0]);
                break;

            case 'C':
            case 'a':
                if (!par[0])
                    par[0]++;
                gotoxy(x + par[0], y);
                break;

            case 'D':
                if (!par[0])
                    par[0]++;
                gotoxy(x - par[0], y);
                break;

            case 'E':
                if (!par[0])
                    par[0]++;
                gotoxy(0, y + par[0]);
                break;

            case 'F':
                if (!par[0])
                    par[0]++;
                gotoxy(0, y - par[0]);
                break;

            case 'd':
                if (par[0])
                    par[0]--;
                gotoxy(x, par[0]);
                break;

            case 'H':
            case 'f':
                if (par[0])
                    par[0]--;
                if (par[1])
                    par[1]--;
                gotoxy(par[1], par[0]);
                break;

            case 'J':
                csi_J(par[0]);
                break;

            case 'K':
                csi_K(par[0]);
                break;

            case 'L':
                csi_L(par[0]);
                break;

            case 'M':
                csi_M(par[0]);
                break;

            case 'P':
                csi_P(par[0]);
                break;

            case '@':
                csi_at(par[0]);
                break;

            case 'm':
                csi_m();
                break;

            case 'r':
                if (par[0])
                    par[0]--;
                if (!par[1])
                    par[1] = video_num_lines;
                if (par[0] < par[1] &&
                    par[1] <= video_num_lines)
                {
                    top = par[0];
                    bottom = par[1];
                }
                break;

            case 's':
                save_cur();
                break;

            case 'u':
                restore_cur();
                break;
            }
        }
    }

    set_cursor();
}

/*
 *  void con_init(void);
 *
 * 这个子程序初始化控制台中断，其它什么都不做。如果你想让屏幕干净的话，就使用
 * 适当的转义字符序列调用tty_write()函数.
 *
 * 读取setup.s程序保存的信息，用以确定当前显示器类型，并且设置所有相关参数.
 */
void con_init(void)
{
    register unsigned char a;
    char *display_desc  = "????";
    char *display_ptr;

    video_num_columns   = ORIG_VIDEO_COLS;
    video_size_row      = video_num_columns * 2;
    video_num_lines     = ORIG_VIDEO_LINES;
    video_page          = ORIG_VIDEO_PAGE;
    video_erase_char    = 0x0720;

    if (ORIG_VIDEO_MODE == 7) /* Is this a monochrome display? */
    {
        video_mem_start = 0xb0000;
        video_port_reg = 0x3b4;
        video_port_val = 0x3b5;

        if ((ORIG_VIDEO_EGA_BX & 0xff) != 0x10)
        {
            video_type = VIDEO_TYPE_EGAM;
            video_mem_end = 0xb8000;
            display_desc = "EGAm";
        }
        else
        {
            video_type = VIDEO_TYPE_MDA;
            video_mem_end = 0xb2000;
            display_desc = "*MDA";
        }
    }
    else /* If not, it is color. */
    {
        video_mem_start = 0xb8000;
        video_port_reg = 0x3d4;
        video_port_val = 0x3d5;

        if ((ORIG_VIDEO_EGA_BX & 0xff) != 0x10)
        {
            video_type = VIDEO_TYPE_EGAC;
            video_mem_end = 0xbc000;
            display_desc = "EGAc";
        }
        else
        {
            video_type = VIDEO_TYPE_CGA;
            video_mem_end = 0xba000;
            display_desc = "*CGA";
        }
    }

    /* Let the user known what kind of display driver we are using */

    display_ptr = ((char *)video_mem_start) + video_size_row - 8;

    while (*display_desc)
    {
        *display_ptr++ = *display_desc++;
        display_ptr++;
    }

    /* Initialize the variables used for scrolling (mostly EGA/VGA)	*/

    origin = video_mem_start;
    scr_end = video_mem_start + video_num_lines * video_size_row;
    top = 0;
    bottom = video_num_lines;

    gotoxy(ORIG_X, ORIG_Y);
    set_trap_gate(0x21, &keyboard_interrupt);
    outb_p(inb_p(0x21) & 0xfd, 0x21);
    a = inb_p(0x61);
    outb_p(a | 0x80, 0x61);
    outb(a, 0x61);
}
/* from bsd-net-2: */

/**
 * sysbeepstop - 停止蜂鸣.
 * 复位8255A PB端口的位1和位0.
 */
void sysbeepstop(void)
{
    /* disable counter 2 */
    outb(inb_p(0x61) & 0xFC, 0x61);
}

int beepcount = 0;

/**
 * sysbeep - 开通蜂鸣.
 * 
 * 8255A芯片PB端口的位1用作扬声器的开门信号；位0用作8253定时器2的门信号，
 * 该定时器的输出脉冲送往扬声器，作为扬声器发声的频率。因此要使扬声器蜂鸣，
 * 需要两步：首先开启PB端口位1和位0(置位)，然后设置定时器发送一定的定时频率即可.
 */
static void sysbeep(void)
{
    /* 开启定时器2. */
    outb_p(inb_p(0x61) | 3, 0x61);

    /* 送设置定时器2命令. */
    outb_p(0xB6, 0x43);

    /* 设置频率为750HZ，因此送定时值0x637. */
    outb_p(0x37, 0x42);
    outb(0x06, 0x42);

    /* 蜂鸣时间为1/8秒. */
    beepcount = HZ / 8;
}
