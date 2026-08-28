.option norvc

.global _entry
.global fromhost
.global tohost

.text
_entry:
csrr  x1, mhartid
beq x1, zero, thread1_startup

thread2_startup:
  la t0, SNIPPY_ENTRY_2
  jalr t0
  j sc_exit

thread1_startup:
  la t0, SNIPPY_ENTRY_1
  jalr t0

sc_exit:
  la x30, sync_point
  li x31, -1
  amoadd.d.aqrl x31, x31, (x30)
  bne x31, zero, the_infinite_loop
  li ra, 1
  la sp, tohost
  sd ra, 0(sp)

the_infinite_loop:
  j the_infinite_loop

.balign 8
tohost:
.8byte 0x0
.balign 8
fromhost:
.8byte 0x0
.balign 64
sync_point:
.8byte 1

