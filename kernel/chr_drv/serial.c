/*
 *  linux/kernel/serial.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * serial.c
 *
 * 该程序用于实现rs232的输入输出功能.
 *  void rs_write(struct tty_struct * queue);
 *  void rs_init(void);
 * 以及与传输 IO 有关系的所有中断处理程序.
 */

#include <linux/tty.h>
#include <linux/sched.h>
#include <asm/system.h>
#include <asm/io.h>

/* 当写队列中含有WAKEUP_CHARS个字符时，就开始发送. */
#define WAKEUP_CHARS            (TTY_BUF_SIZE / 4)

/* 串行口1的中断处理程序(rs_io.s). */
extern void rs1_interrupt(void);
/* 串行口2的中断处理程序(rs_io.s). */
extern void rs2_interrupt(void);

/**
 * init - 初始化串行端口.
 * port: 串口 1 - 0x3F8，串口 2 - 0x2F8.
 */
static void init(int port)
{
    outb_p(0x80, port + 3);     /* 设置线路控制寄存器的DLAB位(位7). */
    outb_p(0x30, port);         /* 发送波特率因子低字节，0x30->2400bps. */
    outb_p(0x00, port + 1);     /* 发送波特率因子高字节，0x00. */
    outb_p(0x03, port + 3);     /* 复位DLAB位，数据位为8位. */
    outb_p(0x0b, port + 4);     /* 设置DTR，RTS，辅助用户输出2. */
    outb_p(0x0d, port + 1);     /* 除了写(写保持空)以外，允许所有中断源中断. */

    (void)inb(port);            /* 读数据口，以进行复位操作(?). */
}

/**
 * rs_init - 初始化串行中断程序和串行接口.
 */
void rs_init(void)
{
    /* 设置串行口1的中断门向量(硬件IRQ4信号). */
    set_intr_gate(0x24, rs1_interrupt);
    /* 设置串行口2的中断门向量(硬件IRQ3信号). */
    set_intr_gate(0x23, rs2_interrupt);

    /* 初始化串行口1(.data是端口号). */
    init(tty_table[1].read_q.data);
    /* 初始化串行口2. */
    init(tty_table[2].read_q.data);

    /* 允许主8259A芯片的IRQ3，IRQ4中断信号请求. */
    outb(inb_p(0x21) & 0xE7, 0x21);
}

/*
 * 在 tty_write()已将数据放入输出(写)队列时会调用下面的子程序。必须首先
 * 检查写队列是否为空，并相应设置中断寄存器.
 */
/**
 * rs_write - 串行数据发送输出。
 * 实际上只是开启串行发送保持寄存器已空中断标志，在UART将数据发送出去后
 * 允许发中断信号.
*/
void rs_write(struct tty_struct *tty)
{
    /* 关中断. */
    cli();

    /**
     * 如果写队列不空，则从0x3f9(或0x2f9)首先读取中断允许寄存器内容，
     * 添上发送保持寄存器中断允许标志(位1)后，再写回该寄存器.
     */
    if (!EMPTY(tty->write_q))
        outb(inb_p(tty->write_q.data + 1) | 0x02, tty->write_q.data + 1);

    /* 开中断. */
    sti();
}
