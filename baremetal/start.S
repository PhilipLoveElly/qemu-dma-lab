.global _start
.extern main

.section .text

_start:
    cli

    mov $0x200000, %rsp

    call main

1:
    hlt
    jmp 1b
