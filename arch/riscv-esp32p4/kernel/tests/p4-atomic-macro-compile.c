/* Compile in both core and public-module modes, then inspect disassembly. */
#include <aros/atomic.h>

struct AtomicMacroOperands
{
    UBYTE byte;
    UWORD word;
    ULONG longword;
};

void p4_atomic_macro_compile(volatile struct AtomicMacroOperands *operands,
                             ULONG mask)
{
    AROS_ATOMIC_INC(operands->byte);
    AROS_ATOMIC_DEC(operands->byte);
    AROS_ATOMIC_AND(operands->byte, mask);
    AROS_ATOMIC_OR(operands->byte, mask);
    AROS_ATOMIC_INC(operands->word);
    AROS_ATOMIC_DEC(operands->word);
    AROS_ATOMIC_AND(operands->word, mask);
    AROS_ATOMIC_OR(operands->word, mask);
    AROS_ATOMIC_INC(operands->longword);
    AROS_ATOMIC_DEC(operands->longword);
    AROS_ATOMIC_AND(operands->longword, mask);
    AROS_ATOMIC_OR(operands->longword, mask);
}
