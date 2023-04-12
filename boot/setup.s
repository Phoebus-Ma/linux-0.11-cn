!
! setup.s       (C) 1991 Linus Torvalds
!
! setup.s负责从BIOS中获取系统数据,并将这些数据放到系统内存的适当地方。
! 此时setup.s和system已经由bootsect引导块加载到内存中.
!
! 这段代码询问bios有关内存/磁盘/其它参数，并将这些参数放到一个
! "安全的"地方:0x90000-0x901FF,也即原来 bootsect 代码块曾经在
! 的地方,然后在被缓冲块覆盖掉之前由保护模式的system读取.
!

! NOTE! 以下这些参数最好和bootsect.s中的相同!

INITSEG  = 0x9000   ! 原来bootsect所处的段.
SYSSEG   = 0x1000   ! system在0x10000(64k)处.
SETUPSEG = 0x9020   ! 本程序所在的段地址.

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

entry start
start:

! ok, 整个读磁盘过程都正常，现在将光标位置保存以备今后使用.

    mov ax,#INITSEG ! this is done in bootsect already, but...
    mov ds,ax
    mov ah,#0x03    ! read cursor pos
    xor bh,bh
    int 0x10        ! save it in known place, con_init fetches
    mov [0],dx      ! it from 0x90000.

! Get memory size (extended mem, kB)

    mov ah,#0x88
    int 0x15
    mov [2],ax

! Get video-card data:

    mov ah,#0x0f
    int 0x10
    mov [4],bx      ! bh = display page
    mov [6],ax      ! al = video mode, ah = window width

! check for EGA/VGA and some config parameters

    mov ah,#0x12
    mov bl,#0x10
    int 0x10
    mov [8],ax      ! 0x90008 = ??
    mov [10],bx     ! 0x9000A = 安装的显示内存,0x9000B = 显示状态(彩色/单色).
    mov [12],cx     ! 0x9000C = 显示卡特性参数.

! Get hd0 data

    mov ax,#0x0000
    mov ds,ax
    lds si,[4*0x41] ! 取中断向量0x41的值,也即hd0参数表的地址.
    mov ax,#INITSEG
    mov es,ax
    mov di,#0x0080  ! 传输的目的地址: 0x9000:0x0080.
    mov cx,#0x10    ! 共传输0x10字节.
    rep
    movsb

! Get hd1 data

    mov ax,#0x0000
    mov ds,ax
    lds si,[4*0x46] ! 取中断向量0x46的值,也即hd1参数表的地址.
    mov ax,#INITSEG
    mov es,ax
    mov di,#0x0090  ! 传输的目的地址: 0x9000:0x0090.
    mov cx,#0x10
    rep
    movsb

! Check that there IS a hd1 :-)

    mov ax,#0x01500
    mov dl,#0x81
    int 0x13
    jc  no_disk1
    cmp ah,#3
    je  is_disk1
no_disk1:
    mov ax,#INITSEG ! 第2个硬盘不存在,则对第2个硬盘表清零.
    mov es,ax
    mov di,#0x0090
    mov cx,#0x10
    mov ax,#0x00
    rep
    stosb
is_disk1:

! now we want to move to protected mode ...

    cli             ! no interrupts allowed !

! first we move the system to it's rightful place

    mov ax,#0x0000
    cld             ! 'direction'=0, movs moves forward
do_move:
    mov es,ax       ! destination segment
    add ax,#0x1000
    cmp ax,#0x9000  ! 已经把从0x8000段开始的64k代码移动完?
    jz  end_move
    mov ds,ax       ! source segment
    sub di,di
    sub si,si
    mov cx,#0x8000  ! 移动0x8000字.
    rep
    movsw
    jmp do_move

! then we load the segment descriptors

end_move:
    mov ax,#SETUPSEG    ! right, forgot this at first. didn't work :-)
    mov ds,ax           ! ds指向本程序(setup)段.
    lidt    idt_48      ! load idt with 0,0
    lgdt    gdt_48      ! load gdt with whatever appropriate

! 以上的操作很简单,现在我们开启A20地址线.

    call    empty_8042  ! 等待输入缓冲器空.
    mov al,#0xD1        ! command write
    out #0x64,al
    call    empty_8042  ! 等待输入缓冲器空，看命令是否被接受.
    mov al,#0xDF        ! 选通A20地址线的参数.
    out #0x60,al
    call    empty_8042  ! 输入缓冲器为空,则表示A20线已经选通.

! well, that went ok, I hope. Now we have to reprogram the interrupts :-(
! we put them right after the intel-reserved hardware interrupts, at
! int 0x20-0x2F. There they won't mess up anything. Sadly IBM really
! messed this up with the original PC, and they haven't been able to
! rectify it afterwards. Thus the bios puts interrupts at 0x08-0x0f,
! which is used for the internal hardware interrupts as well. We just
! have to reprogram the 8259's, and it isn't fun.

    mov al,#0x11        ! initialization sequence
    out #0x20,al        ! send it to 8259A-1
    .word   0x00eb,0x00eb   ! jmp $+2, jmp $+2
    out #0xA0,al        ! and to 8259A-2
    .word   0x00eb,0x00eb
    mov al,#0x20        ! start of hardware int's (0x20)
    out #0x21,al        ! 送主芯片ICW2命令字,起始中断号,要送奇地址.
    .word   0x00eb,0x00eb
    mov al,#0x28        ! start of hardware int's 2 (0x28)
    out #0xA1,al        ! 送从芯片ICW2命令字,从芯片的起始中断号.
    .word   0x00eb,0x00eb
    mov al,#0x04        ! 8259-1 is master
    out #0x21,al        ! 送主芯片ICW3命令字,主芯片的IR2连从芯片INT.
    .word   0x00eb,0x00eb
    mov al,#0x02        ! 8259-2 is slave
    out #0xA1,al        ! 送从芯片ICW3命令字,表示从芯片的INT连到主芯片的IR2引脚上.
    .word   0x00eb,0x00eb
    mov al,#0x01        ! 8086 mode for both
    out #0x21,al        ! 送主芯片ICW4命令字.
    .word   0x00eb,0x00eb
    out #0xA1,al        ! 送从芯片ICW4命令字,内容同上.
    .word   0x00eb,0x00eb
    mov al,#0xFF        ! mask off all interrupts for now
    out #0x21,al        ! 屏蔽主芯片所有中断请求.
    .word   0x00eb,0x00eb
    out #0xA1,al        ! 屏蔽从芯片所有中断请求.

! well, that certainly wasn't fun :-(. Hopefully it works, and we don't
! need no steenking BIOS anyway (except for the initial loading :-).
! The BIOS-routine wants lots of unnecessary data, and it's less
! "interesting" anyway. This is how REAL programmers do it.
!
! Well, now's the time to actually move into protected mode. To make
! things as simple as possible, we do no register set-up or anything,
! we let the gnu-compiled 32-bit programs do that. We just jump to
! absolute address 0x00000, in 32-bit protected mode.

    mov ax,#0x0001  ! protected mode (PE) bit
    lmsw    ax      ! This is it!
    jmpi    0,8     ! jmp offset 0 of segment 8 (cs)

! This routine checks that the keyboard command queue is empty
! No timeout is used - if this hangs there is something wrong with
! the machine, and we probably couldn't proceed anyway.
empty_8042:
    .word   0x00eb,0x00eb
    in  al,#0x64    ! 8042 status port
    test    al,#2   ! is input buffer full?
    jnz empty_8042  ! yes - loop
    ret

gdt:
    .word   0,0,0,0     ! dummy

    .word   0x07FF      ! 8Mb - limit=2047 (2048*4096=8Mb)
    .word   0x0000      ! base address=0
    .word   0x9A00      ! code read/exec
    .word   0x00C0      ! granularity=4096, 386

    .word   0x07FF      ! 8Mb - limit=2047 (2048*4096=8Mb)
    .word   0x0000      ! base address=0
    .word   0x9200      ! data read/write
    .word   0x00C0      ! granularity=4096, 386

idt_48:
    .word   0           ! idt limit=0
    .word   0,0         ! idt base=0L

gdt_48:
    .word   0x800       ! gdt limit=2048, 256 GDT entries
    .word   512+gdt,0x9 ! gdt base = 0X9xxxx

.text
endtext:
.data
enddata:
.bss
endbss:
