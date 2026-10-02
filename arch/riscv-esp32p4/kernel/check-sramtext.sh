#!/bin/sh

# A P4_SRAMCODE function may run while the external-memory cache is
# suspended. A direct call, jump or data reference back into the XIP window
# would then stop the hart before it could report the mistake. Keep this as
# a link check because `static inline` is not a promise: at -Os GCC may emit
# one out-of-line copy in .flash.text and call it from .sramtext.

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 OBJDUMP ELF" >&2
    exit 2
fi

objdump=$1
image=$2
disassembly=$("$objdump" -dr --section=.sramtext "$image")

# The external flash instruction/data window is 0x40000000-0x43ffffff.
# ROM calls at 0x4fc... and internal SRAM targets at 0x4ff... are expected.
bad=$(printf '%s\n' "$disassembly" |
    awk '
    function hex_value(text,    i, digit, value) {
        value = 0
        for (i = 1; i <= length(text); i++) {
            digit = index("0123456789abcdef", tolower(substr(text, i, 1))) - 1
            if (digit < 0)
                return 0
            value = value * 16 + digit
        }
        return value
    }
    function number_value(text,    negative) {
        number_valid = 0
        if (text !~ /^-?(0[xX][[:xdigit:]]+|[0-9]+)$/)
            return 0
        negative = 0
        if (substr(text, 1, 1) == "-") {
            negative = 1
            text = substr(text, 2)
        }
        if (text ~ /^0[xX]/)
            text = hex_value(substr(text, 3))
        else
            text += 0
        number_valid = 1
        return negative ? -text : text
    }
    function sign_extend_20(value) {
        return value >= 524288 ? value - 1048576 : value
    }
    function wrap_32(value) {
        value %= 4294967296
        return value < 0 ? value + 4294967296 : value
    }
    function is_xip(value) {
        return value >= hex_value("40000000") && value < hex_value("44000000")
    }
    function is_sram(value) {
        return value >= hex_value("4ff00000") && value < hex_value("4ff80000")
    }
    function is_memory_op(op) {
        return op ~ /^(lb|lbu|lh|lhu|lw|lwu|ld|sb|sh|sw|sd|flw|fld|fsw|fsd)$/
    }
    function clear_register(reg) {
        delete known[reg]
        delete known_addr[reg]
        delete auipc_origin[reg]
        delete auipc_addi[reg]
        delete auipc_mv[reg]
    }
    function invalidate_call_clobbers(    reg) {
        for (reg in known) {
            if (reg ~ /^(ra|t[0-6]|a[0-7])$/)
                clear_register(reg)
        }
    }
    /^[0-9a-f]+ <[^>]+>:$/ {
        if ($0 !~ /<[^>]+\+0x[0-9a-f]+>:/ && $0 !~ /<\.L[^>]+>:/)
            for (reg in known) clear_register(reg)
    }
    {
        split($4, args, ",")
        op = $3
        rd = args[1]
        source = args[2]
        immediate = args[3]
        memory_base = source
        sub(/.*\(/, "", memory_base)
        sub(/\).*/, "", memory_base)
        memory_offset = source
        sub(/\(.*/, "", memory_offset)
        memory_offset_value = number_value(memory_offset)
        memory_offset_valid = number_valid
        source_immediate_value = number_value(source)
        source_immediate_valid = number_valid
        instruction_immediate_value = number_value(immediate)
        instruction_immediate_valid = number_valid
        effective_addr = 0
        if (known[memory_base] && memory_offset_valid)
            effective_addr = wrap_32(known_addr[memory_base] + memory_offset_value)
        candidate = ($0 ~ /4[0-3][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] <[^>]*>/)
        operands = $0
        comment = ""
        if (index($0, "#")) {
            comment = substr($0, index($0, "#") + 1)
            sub(/#.*/, "", operands)
        }
        # GNU objdump can retain stale register values in comments. A known
        # absolute libreq-version symbol is not a real XIP reference; ignore
        # only its trailing annotation unless the tracked source is XIP.
        if (candidate && comment ~ /[0-9a-f]+ <__aros_libreq_[^>]+\+0x[0-9a-f]+>/) {
            if (operands !~ /4[0-3][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] <[^>]*>/)
                candidate = ((op == "addi" && known[source] &&
                              is_xip(known_addr[source])) ||
                    (is_memory_op(op) && known[memory_base] &&
                     is_xip(known_addr[memory_base])))
        } else if (candidate && comment ~ /[0-9a-f]+ <[^>]+>/ &&
                   operands !~ /4[0-3][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] <[^>]*>/ &&
                   is_memory_op(op) && known[memory_base] &&
                   auipc_origin[memory_base] &&
                   auipc_addi[memory_base] && auipc_mv[memory_base] &&
                   is_sram(known_addr[memory_base]) &&
                   is_sram(effective_addr)) {
            # A stale real-XIP symbol comment is ignored only on a memory
            # dereference whose register base and effective address are
            # proven to be internal SRAM through AUIPC/ADDI/MV provenance.
            # Unknown bases, direct XIP operands and all other instructions
            # remain fail-closed.
            candidate = 0
        }
        if (candidate) print

        # Preserve refusal for genuine absolute LUI/ADDI materialization.
        # Track RV32 constant addresses through LUI/AUIPC + ADDI/MV; any
        # other register write invalidates the value. This is a linear named-
        # reference lint, not a full CFG/dataflow proof.
        if ($1 ~ /^[0-9a-f]+:$/ && $2 ~ /^[0-9a-f]+$/ &&
            op ~ /^(call|jal|jalr|tail)$/)
            invalidate_call_clobbers()

        if ($1 ~ /^[0-9a-f]+:$/ && $2 ~ /^[0-9a-f]+$/ &&
            rd ~ /^(zero|ra|sp|gp|tp|t[0-6]|s([0-9]|1[01])|a[0-7])$/ &&
            rd != "zero" &&
            op !~ /^(sb|sh|sw|beq|bne|blt|bge|bltu|bgeu|beqz|bnez|bltz|bgez|blez|bgtz|jr)$/) {
            source_known = known[source]
            source_addr = known_addr[source]
            source_auipc_origin = auipc_origin[source]
            source_auipc_addi = auipc_addi[source]
            source_auipc_mv = auipc_mv[source]
            clear_register(rd)
            if (op == "lui" && source_immediate_valid) {
                known[rd] = 1
                known_addr[rd] = wrap_32(sign_extend_20(source_immediate_value) * 4096)
            } else if (op == "auipc" && source_immediate_valid) {
                pc_text = $1
                sub(/:$/, "", pc_text)
                pc = hex_value(pc_text)
                known[rd] = 1
                known_addr[rd] = wrap_32(pc + sign_extend_20(source_immediate_value) * 4096)
                auipc_origin[rd] = 1
                auipc_addi[rd] = 0
                auipc_mv[rd] = 0
            } else if (op == "addi" && source_known && instruction_immediate_valid) {
                known[rd] = 1
                known_addr[rd] = wrap_32(source_addr + instruction_immediate_value)
                auipc_origin[rd] = source_auipc_origin
                auipc_addi[rd] = source_auipc_addi || source_auipc_origin
                auipc_mv[rd] = source_auipc_mv
            } else if (op == "mv" && source_known) {
                known[rd] = 1
                known_addr[rd] = source_addr
                auipc_origin[rd] = source_auipc_origin
                auipc_addi[rd] = source_auipc_addi
                auipc_mv[rd] = source_auipc_mv ||
                               (source_auipc_origin && source_auipc_addi)
            }
        }
    }')

if [ -n "$bad" ]; then
    echo "error: .sramtext refers to the XIP flash window:" >&2
    printf '%s\n' "$bad" >&2
    exit 1
fi
