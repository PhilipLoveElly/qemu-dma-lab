.section .multiboot
.align 4

.set MAGIC,    0x1BADB002
.set FLAGS,    0x00000003
.set CHECKSUM, -(MAGIC + FLAGS)

.long MAGIC
.long FLAGS
.long CHECKSUM


.section .bss
.align 16

stack_bottom:
.skip 16384
stack_top:


.section .text
.global _start
.type _start, @function

.extern dma_test_main
.extern dma_irq_handler

_start:
    cli

    mov $stack_top, %esp
    mov %esp, %ebp

    call dma_test_main

.hang:
    hlt
    jmp .hang


.global dma_irq_entry
.type dma_irq_entry, @function

dma_irq_entry:
    pusha

    call dma_irq_handler

    popa
    iret
