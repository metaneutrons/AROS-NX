    .option push
    .option norvc
    .option norelax

    .section .text.secondary_xip_signature,"ax",@progbits
    .balign 4
    .globl __p4_secondary_xip_signature_start
__p4_secondary_xip_signature_start:
    .globl __p4_secondary_xip_signature
    .type __p4_secondary_xip_signature, @function
__p4_secondary_xip_signature:
    /* fixed input transform: XOR, rotate-left 7, then immediate XOR */
    li      t0, 0x13579bdf
    xor     a0, a0, t0
    slli    t0, a0, 7
    srli    a0, a0, 25
    or      a0, a0, t0
    xori    a0, a0, 0x2a5
    ret
    .size __p4_secondary_xip_signature, .-__p4_secondary_xip_signature

    .globl __p4_secondary_xip_signature_end
__p4_secondary_xip_signature_end:

    .option pop
