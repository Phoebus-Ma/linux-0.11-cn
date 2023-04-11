/*
 *  linux/boot/head.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * head.s含有32位启动代码.
 *
 * 注意!!!32位启动代码是从绝对地址0x00000000开始的,这里也同样是页目录将存在的地方,
 * 因此这里的启动代码将被页目录覆盖掉.
 */
.text
.globl _idt,_gdt,_pg_dir,_tmp_floppy_area
_pg_dir:
startup_32:
    movl $0x10,%eax
    mov %ax,%ds
    mov %ax,%es
    mov %ax,%fs
    mov %ax,%gs
    lss _stack_start,%esp   /* 表示_stack_start??ss:esp,设置系统堆栈. */
    call setup_idt          /* 调用设置中断描述符表子程序. */
    call setup_gdt          /* 调用设置全局描述符表子程序. */
    movl $0x10,%eax         /* reload all the segment registers. */
    mov %ax,%ds             /* after changing gdt. CS was already. */
    mov %ax,%es             /* reloaded in 'setup_gdt'. */
    mov %ax,%fs             /* 因为修改了gdt，所以需要重新装载所有的段寄存器. */
    mov %ax,%gs             /* CS代码段寄存器已经在setup_gdt中重新加载过了. */
    lss _stack_start,%esp
    xorl %eax,%eax
1:  incl %eax               /* check that A20 really IS enabled. */
    movl %eax,0x000000      /* loop forever if it isn't. */
    cmpl %eax,0x100000
    je 1b
/*
 * 注意! 在下面这段程序中，486应该将位16置位，以检查在超级用户模式下的写保护,
 * 此后"verify_area()"调用中就不需要了。486的用户通常也会想将NE(#5)置位，以便
 * 对数学协处理器的出错使用int 16.
 */
    movl %cr0,%eax          /* check math chip. */
    andl $0x80000011,%eax   /* Save PG,PE,ET. */
/* "orl $0x10020,%eax" here for 486 might be good */
    orl $2,%eax             /* set MP. */
    movl %eax,%cr0
    call check_x87
    jmp after_page_tables

/*
 * We depend on ET to be correct. This checks for 287/387.
 */
check_x87:
    fninit
    fstsw %ax
    cmpb $0,%al
    je 1f                   /* no coprocessor: have to set bits */
    movl %cr0,%eax          /* 如果存在的则向前跳转到标号1处,否则改写cr0. */
    xorl $6,%eax            /* reset MP, set EM */
    movl %eax,%cr0
    ret
.align 2
1:  .byte 0xDB,0xE4         /* fsetpm for 287, ignored by 387 */
    ret

/*
 *  setup_idt
 *
 *  sets up a idt with 256 entries pointing to
 *  ignore_int, interrupt gates. It then loads
 *  idt. Everything that wants to install itself
 *  in the idt-table may do so themselves. Interrupts
 *  are enabled elsewhere, when we can be relatively
 *  sure everything is ok. This routine will be over-
 *  written by the page tables.
 */
setup_idt:
    lea ignore_int,%edx
    movl $0x00080000,%eax
    movw %dx,%ax            /* selector = 0x0008 = cs */
    movw $0x8E00,%dx        /* interrupt gate - dpl=0, present */

    lea _idt,%edi           /* _idt是中断描述符表的地址. */
    mov $256,%ecx
rp_sidt:
    movl %eax,(%edi)        /* 将哑中断门描述符存入表中. */
    movl %edx,4(%edi)
    addl $8,%edi            /* edi指向表中下一项. */
    dec %ecx
    jne rp_sidt
    lidt idt_descr          /* 加载中断描述符表寄存器值. */
    ret

/*
 *  setup_gdt
 *
 *  This routines sets up a new gdt and loads it.
 *  Only two entries are currently built, the same
 *  ones that were built in init.s. The routine
 *  is VERY complicated at two whole lines, so this
 *  rather long comment is certainly needed :-).
 *  This routine will beoverwritten by the page tables.
 */
setup_gdt:
    lgdt gdt_descr          /* 加载全局描述符表寄存器. */
    ret

/*
 * I put the kernel page tables right after the page directory,
 * using 4 of them to span 16 Mb of physical memory. People with
 * more than 16MB will have to expand this.
 */
.org 0x1000
pg0:

.org 0x2000
pg1:

.org 0x3000
pg2:

.org 0x4000
pg3:

.org 0x5000                 /* 定义下面的内存数据块从偏移0x5000处开始. */
/*
 * tmp_floppy_area is used by the floppy-driver when DMA cannot
 * reach to a buffer-block. It needs to be aligned, so that it isn't
 * on a 64kB border.
 */
_tmp_floppy_area:
    .fill 1024,1,0          /* 共保留1024项，每项1字节，填充数值0. */

after_page_tables:
    pushl $0                /* These are the parameters to main :-) */
    pushl $0                /* 这些是调用main程序的参数(指init/main.c). */
    pushl $0
    pushl $L6               /* return address for main, if it decides to. */
    pushl $_main            /* '_main'是编译程序对 main 的内部表示方法. */
    jmp setup_paging
L6:
    jmp L6                  /* main should never return here, but */
                            /* just in case, we know what happens. */

/* 下面是默认的中断"handler" :-) */
int_msg:
    .asciz "Unknown interrupt\n\r"  /* 定义字符串"未知中断(回车换行)". */
.align 2                    /* 按4字节方式对齐内存地址. */
ignore_int:
    pushl %eax
    pushl %ecx
    pushl %edx
    push %ds
    push %es
    push %fs
    movl $0x10,%eax         /* 置段选择符(使ds,es,fs指向gdt表中的数据段). */
    mov %ax,%ds
    mov %ax,%es
    mov %ax,%fs
    pushl $int_msg          /* 把调用printk函数的参数指针(地址)入栈. */
    call _printk            /* 该函数在/kernel/printk.c中. */
    popl %eax
    pop %fs
    pop %es
    pop %ds
    popl %edx
    popl %ecx
    popl %eax
    iret                    /* 中断返回. */


/*
 * Setup_paging
 *
 * This routine sets up paging by setting the page bit
 * in cr0. The page tables are set up, identity-mapping
 * the first 16MB. The pager assumes that no illegal
 * addresses are produced (ie >4Mb on a 4Mb machine).
 *
 * NOTE! Although all physical memory should be identity
 * mapped by this routine, only the kernel page functions
 * use the >1Mb addresses directly. All "normal" functions
 * use just the lower 1Mb, or the local data space, which
 * will be mapped to some other place - mm keeps track of
 * that.
 *
 * For those with more memory than 16 Mb - tough luck. I've
 * not got it, why should you :-) The source is here. Change
 * it. (Seriously - it shouldn't be too difficult. Mostly
 * change some constants etc. I left it at 16Mb, as my machine
 * even cannot be extended past that (ok, but it was cheap :-)
 * I've tried to show which constants to change by having
 * some kind of marker at them (search for "16Mb"), but I
 * won't guarantee that's all :-( )
 */
.align 2                    /* 按4字节方式对齐内存地址边界. */
setup_paging:               /* 首先对5页内存(1页目录 + 4页页表)清零. */
    movl $1024*5,%ecx       /* 5 pages - pg_dir+4 page tables */
    xorl %eax,%eax
    xorl %edi,%edi          /* pg_dir is at 0x000 */
    cld;rep;stosl
    movl $pg0+7,_pg_dir     /* set present bit/user r/w */
    movl $pg1+7,_pg_dir+4   /*  --------- " " --------- */
    movl $pg2+7,_pg_dir+8   /*  --------- " " --------- */
    movl $pg3+7,_pg_dir+12  /*  --------- " " --------- */
    movl $pg3+4092,%edi
    movl $0xfff007,%eax     /*  16Mb - 4096 + 7 (r/w user,p) */
    std
1:  stosl                   /* fill pages backwards - more efficient :-) */
    subl $0x1000,%eax
    jge 1b
    xorl %eax,%eax          /* pg_dir is at 0x0000 */
    movl %eax,%cr3          /* cr3 - page directory start */
    movl %cr0,%eax
    orl $0x80000000,%eax    /* 添上PG标志. */
    movl %eax,%cr0          /* set paging (PG) bit */
    ret                     /* this also flushes prefetch-queue */

.align 2                    /* 按4字节方式对齐内存地址边界. */
.word 0
idt_descr:                  /* 下面两行是lidt指令的6字节操作数：长度，基址. */
    .word 256*8-1           /* idt contains 256 entries. */
    .long _idt
.align 2
.word 0
gdt_descr:                  /* 下面两行是lgdt指令的6字节操作数：长度，基址. */
    .word 256*8-1           /* so does gdt (not that that's any */
    .long _gdt              /* magic number, but it works for me :^) */

    .align 3                /* 按8字节方式对齐内存地址边界. */
_idt:   .fill 256,8,0       /* idt is uninitialized. */

_gdt:   .quad 0x0000000000000000/* NULL descriptor */
    .quad 0x00c09a0000000fff    /* 代码段最大长度16Mb */
    .quad 0x00c0920000000fff    /* 数据段最大长度16Mb */
    .quad 0x0000000000000000    /* TEMPORARY - don't use */
    .fill 252,8,0               /* space for LDT's and TSS's etc */
