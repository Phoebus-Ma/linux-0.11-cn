!
! SYS_SIZE是要加载的节数(16字节为1节).
! 0x3000 is 0x30000 bytes = 196kB, 对于当前的版本空间已足够了.
!
SYSSIZE = 0x3000
!
! bootsect.s        (C) 1991 Linus Torvalds
!
! bootsect.s被 bios-启动子程序加载至 0x7c00 (31k)处,并将自己
! 移到了地址0x90000(576k)处,并跳转至那里.
!
! I它然后使用BIOS中断将'setup'直接加载到自己的后面(0x90200)(576.5k),
! 并将system加载到地址0x10000处.
!
! 注意! 目前的内核系统最大长度限制为(8*65536)(512k)字节,即使是在
! 将来这也应该没有问题的。我想让它保持简单明了。这样512k的最大内核长度应该
! 足够了,尤其是这里没有象 minix中一样包含缓冲区高速缓冲.
!
! 加载程序已经做的够简单了,所以持续的读出错将导致死循环。只能手工重启.
! 只要可能,通过一次取取所有的扇区,加载过程可以做的很快的.

! 定义了6个全局标识符
.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

SETUPLEN = 4                ! setup程序的扇区数(setup-sectors)值.
BOOTSEG  = 0x07c0           ! bootsect的原始地址(是段地址,以下同).
INITSEG  = 0x9000           ! 我们移动boot到这里 - 避开.
SETUPSEG = 0x9020           ! setup程序从这里开始.
SYSSEG   = 0x1000           ! system模块加载在0x10000 (64KB处).
ENDSEG   = SYSSEG + SYSSIZE ! 停止加载的段地址.

! ROOT_DEV: 0x000 - 使用与引导时同样的软驱设备.
! 0x301 - 在第一个硬盘的第一个分区上.
ROOT_DEV = 0x306            ! 指定根文件系统设备是第2个硬盘的第1个分区。这是Linux老式的硬盘命名.

entry start                 ! 告知连接程序,程序从start标号开始执行.
start:
    mov ax,#BOOTSEG         ! 将ds段寄存器置为0x7C0.
    mov ds,ax
    mov ax,#INITSEG         ! 将es段寄存器置为0x9000.
    mov es,ax
    mov cx,#256             ! 移动计数值=256字.
    sub si,si               ! 源地址 ds:si = 0x07C0:0x0000.
    sub di,di               ! 目的地址 es:di = 0x9000:0x0000.
    rep                     ! 重复执行,直到 cx = 0.
    movw                    ! 移动1个字.
    jmpi    go,INITSEG      ! 间接跳转。这里INITSEG指出跳转到的段地址.
go: mov ax,cs               ! 将ds、es和ss都置成移动后代码所在的段处(0x9000).
    mov ds,ax               ! 由于程序中有堆栈操作(push,pop,call),因此必须设置堆栈.
    mov es,ax
! 将堆栈指针sp指向0x9ff00(即0x9000:0xff00)处.
    mov ss,ax
    mov sp,#0xFF00          ! arbitrary value >>512

! 在bootsect程序块后紧根着加载setup模块的代码数据.
! 注意es已经设置好了。(在移动代码时es已经指向目的段地址处0x9000).

load_setup:
    mov dx,#0x0000          ! drive 0, head 0
    mov cx,#0x0002          ! sector 2, track 0
    mov bx,#0x0200          ! address = 512, in INITSEG
    mov ax,#0x0200+SETUPLEN ! service 2, nr of sectors
    int 0x13                ! read it
    jnc ok_load_setup       ! ok - continue
    mov dx,#0x0000
    mov ax,#0x0000          ! reset the diskette
    int 0x13
    j   load_setup

ok_load_setup:

! Get disk drive parameters, specifically nr of sectors/track

    mov dl,#0x00
    mov ax,#0x0800          ! AH=8 is get drive parameters
    int 0x13
    mov ch,#0x00
    seg cs                  ! 表示下一条语句的操作数在cs段寄存器所指的段中.
    mov sectors,cx          ! 保存每磁道扇区数.
    mov ax,#INITSEG
    mov es,ax               ! 因为上面取磁盘参数中断改掉了es的值,这里重新改回.

! Print some inane message

    mov ah,#0x03            ! read cursor pos
    xor bh,bh               ! 读光标位置.
    int 0x10

    mov cx,#24              ! 共24个字符.
    mov bx,#0x0007          ! page 0, attribute 7 (normal)
    mov bp,#msg1            ! 指向要显示的字符串.
    mov ax,#0x1301          ! write string, move cursor
    int 0x10                ! 写字符串并移动光标.

! ok, we've written the message, now
! we want to load the system (at 0x10000)
! 现在开始将system模块加载到0x10000(64k)处.

    mov ax,#SYSSEG
    mov es,ax               ! segment of 0x010000,es = 存放system的段地址.
    call    read_it         ! 读磁盘上system模块,es为输入参数.
    call    kill_motor      ! 关闭驱动器马达,这样就可以知道驱动器的状态了.

! 此后,我们检查要使用哪个根文件系统设备（简称根设备）。如果已经指定了设备(!=0)
! 就直接使用给定的设备。否则就需要根据 BIOS 报告的每磁道扇区数来
! 确定到底使用/dev/PS0 (2,28) 还是 /dev/at0 (2,8).
! 上面一行中两个设备文件的含义：
! 在Linux中软驱的主设备号是2,次设备号 = type*4 + nr,其中
! nr为0-3分别对应软驱A、B、C或D. type是软驱的类型(2??1.2M或7??1.44M等).
! 因为7*4 + 0 = 28,所以 /dev/PS0 (2,28)指的是 1.44M A驱动器,其设备号是0x021c
! 同理/dev/at0 (2,8)指的是1.2M A驱动器, 其设备号是0x0208.

    seg cs
    mov ax,root_dev         ! 将根设备号.
    cmp ax,#0
    jne root_defined
    seg cs
    mov bx,sectors
    mov ax,#0x0208          ! /dev/ps0 - 1.2Mb
    cmp bx,#15              ! 判断每磁道扇区数是否=15.
    je  root_defined        ! 如果等于,则ax中就是引导驱动器的设备号.
    mov ax,#0x021c          ! /dev/PS0 - 1.44Mb
    cmp bx,#18
    je  root_defined
undef_root:
    jmp undef_root          ! 如果都不一样,则死循环(死机).
root_defined:
    seg cs
    mov root_dev,ax         ! 将检查过的设备号保存起来.

! after that (everyting loaded), we jump to
! the setup-routine loaded directly after
! the bootblock:

    jmpi    0,SETUPSEG

! This routine loads the system at address 0x10000, making sure
! no 64kB boundaries are crossed. We try to load it as fast as
! possible, loading whole tracks whenever we can.
!
! in:   es - starting address segment (normally 0x1000)
!
sread:  .word 1+SETUPLEN    ! sectors read of current track
head:   .word 0             ! current head
track:  .word 0             ! current track

read_it:
    mov ax,es
    test ax,#0x0fff
die:    jne die             ! es must be at 64kB boundary
    xor bx,bx               ! bx is starting address within segment
rp_read:
    mov ax,es
    cmp ax,#ENDSEG          ! have we loaded all yet?
    jb ok1_read
    ret
ok1_read:
    seg cs
    mov ax,sectors          ! 取每磁道扇区数.
    sub ax,sread            ! 减去当前磁道已读扇区数.
    mov cx,ax               ! cx = ax = 当前磁道未读扇区数.
    shl cx,#9               ! cx = cx * 512 字节.
    add cx,bx               ! cx = cx + 段内当前偏移值(bx).
    jnc ok2_read            ! 若没有超过64KB字节,则跳转至ok2_read处执行.
    je ok2_read
    xor ax,ax               ! 若加上此次将读磁道上所有未读扇区时会超过64KB,则计算
    sub ax,bx               ! 此时最多能读入的字节数(64KB - 段内读偏移位置),再转换
    shr ax,#9               ! 成需要读取的扇区数.
ok2_read:
    call read_track
    mov cx,ax               ! cx = 该次操作已读取的扇区数.
    add ax,sread            ! 当前磁道上已经读取的扇区数.
    seg cs
    cmp ax,sectors          ! 如果当前磁道上的还有扇区未读,则跳转到ok3_read处.
    jne ok3_read            ! 读该磁道的下一磁头面(1号磁头)上的数据。如果已经完成,则去读下一磁道.
    mov ax,#1
    sub ax,head             ! 判断当前磁头号.
    jne ok4_read            ! 如果是0磁头,则再去读1磁头面上的扇区数据.
    inc track               ! 否则去读下一磁道.
ok4_read:
    mov head,ax             ! 保存当前磁头号.
    xor ax,ax               ! 清当前磁道已读扇区数.
ok3_read:
    mov sread,ax            ! 保存当前磁道已读扇区数.
    shl cx,#9               ! 上次已读扇区数*512字节.
    add bx,cx               ! 调整当前段内数据开始位置.
    jnc rp_read             ! 若小于 64KB 边界值,则跳转到 rp_read处,继续读数据.
    mov ax,es
    add ax,#0x1000          ! 将段基址调整为指向下一个64KB段内存.
    mov es,ax
    xor bx,bx               ! 清段内数据开始偏移值.
    jmp rp_read             ! 跳转至rp_read处,继续读数据.

read_track:
    push ax
    push bx
    push cx
    push dx
    mov dx,track            ! 取当前磁道号.
    mov cx,sread            ! 取当前磁道上已读扇区数.
    inc cx                  ! cl = 开始读扇区.
    mov ch,dl               ! ch = 当前磁道号.
    mov dx,head             ! 取当前磁头号.
    mov dh,dl               ! dh = 磁头号.
    mov dl,#0               ! dl = 驱动器号(为0表示当前驱动器).
    and dx,#0x0100          ! 磁头号不大于1.
    mov ah,#2               ! ah = 2,读磁盘扇区功能号.
    int 0x13
    jc bad_rt               ! 若出错,则跳转至bad_rt.
    pop dx
    pop cx
    pop bx
    pop ax
    ret
bad_rt: mov ax,#0
    mov dx,#0
    int 0x13
    pop dx
    pop cx
    pop bx
    pop ax
    jmp read_track

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 */
kill_motor:
    push dx
    mov dx,#0x3f2           ! 软驱控制卡的驱动端口,只写.
    mov al,#0               ! A驱动器,关闭 FDC,禁止 DMA 和中断请求,关闭马达.
    outb                    ! 将al中的内容输出到dx指定的端口去.
    pop dx
    ret

sectors:
    .word 0                 ! 存放当前启动软盘每磁道的扇区数.

msg1:
    .byte 13,10             ! 回车、换行的ASCII码.
    .ascii "Loading system ..."
    .byte 13,10,13,10       ! 共24个ASCII码字符.

.org 508                    ! 表示下面语句从地址508(0x1FC)开始.
root_dev:
    .word ROOT_DEV          ! 这里存放根文件系统所在的设备号(init/main.c中会用).
boot_flag:
    .word 0xAA55            ! 硬盘有效标识.

.text
endtext:
.data
enddata:
.bss
endbss:
