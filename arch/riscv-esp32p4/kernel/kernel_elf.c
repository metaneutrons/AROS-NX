/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Relocating ELF32 loader for boot-time AROS PKG modules.

    A package member is a complete relocatable AROS module.  The package
    image itself is writable and remains resident: section addresses are
    recorded back into its ELF headers and debug.library keeps pointers to
    its names, symbol tables and string tables.
*/

#include <inttypes.h>

#include <exec/types.h>
#define ELF_32BIT
#include <dos/elf.h>
#include <libraries/debug.h>

#include "kernel_intern.h"

/* Bump allocator over the raw PSRAM range supplied by the caller. */
static IPTR alloc_ptr, alloc_end;
static IPTR loaded_lo, loaded_hi;

/* Published through KRN_DebugInfo once the caller installs the boot tag. */
void *__ks_debuginfo;
static struct ELF_ModuleInfo *mod_chain_tail;

/* Address of the first executable section in the current module. */
static IPTR mod_text;

static int range_ok(IPTR total, IPTR off, IPTR size)
{
    return off <= total && size <= total - off;
}

static uint16_t rd16le(const void *at)
{
    const UBYTE *p = at;

    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32le(const void *at)
{
    const UBYTE *p = at;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const void *at)
{
    const UBYTE *p = at;

    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static void wr16le(void *at, uint16_t v)
{
    UBYTE *p = at;

    p[0] = (UBYTE)v;
    p[1] = (UBYTE)(v >> 8);
}

static void wr32le(void *at, uint32_t v)
{
    UBYTE *p = at;

    p[0] = (UBYTE)v;
    p[1] = (UBYTE)(v >> 8);
    p[2] = (UBYTE)(v >> 16);
    p[3] = (UBYTE)(v >> 24);
}

static void wr64le(void *at, uint64_t v)
{
    UBYTE *p = at;

    wr32le(p, (uint32_t)v);
    wr32le(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd32be(const void *at)
{
    const UBYTE *p = at;

    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void copy_bytes(UBYTE *to, const UBYTE *from, IPTR size)
{
    IPTR i;

    for (i = 0; i < size; i++)
        to[i] = from[i];
}

static void zero_bytes(UBYTE *to, IPTR size)
{
    IPTR i;

    for (i = 0; i < size; i++)
        to[i] = 0;
}

static int bounded_string(const char *s, IPTR room)
{
    IPTR i;

    for (i = 0; i < room; i++)
    {
        if (!s[i])
            return 1;
    }
    return 0;
}

static int bounded_streq(const char *s, IPTR room, const char *want)
{
    IPTR i;

    for (i = 0; i < room; i++)
    {
        if (s[i] != want[i])
            return 0;
        if (!want[i])
            return 1;
    }
    return 0;
}

static void *krnBumpAlloc(IPTR size, IPTR align)
{
    IPTR addr;

    /* ELF permits zero to mean that a section has no alignment demand. */
    if (!align)
        align = 1;
    if (align < 8)
        align = 8;
    if ((align & (align - 1)) != 0 || alloc_ptr > ~(IPTR)0 - (align - 1))
        return NULL;

    addr = (alloc_ptr + align - 1) & ~(align - 1);
    if (addr > alloc_end || size > alloc_end - addr)
        return NULL;

    alloc_ptr = addr + size;
    if (addr < loaded_lo)
        loaded_lo = addr;
    if (alloc_ptr > loaded_hi)
        loaded_hi = alloc_ptr;

    return (void *)addr;
}

static inline struct sheader *shdr(struct elfheader *eh, unsigned int n)
{
    return (struct sheader *)((UBYTE *)eh + eh->shoff + n * eh->shentsize);
}

static int section_named(struct elfheader *eh, unsigned int sec,
                         const char *want)
{
    struct sheader *names = shdr(eh, eh->shstrndx);
    struct sheader *sh = shdr(eh, sec);
    const char *str = (const char *)eh + names->offset + sh->name;

    return bounded_streq(str, names->size - sh->name, want);
}

static int validate_elf(struct elfheader *eh, IPTR size, const char *name)
{
    struct sheader *names;
    unsigned int i;

    if (size < sizeof(*eh) || ((IPTR)eh & 3))
        goto bad;

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[EI_CLASS] != ELFCLASS32 ||
        eh->ident[EI_DATA] != ELFDATA2LSB ||
        eh->ident[EI_VERSION] != EV_CURRENT ||
        eh->type != ET_REL || eh->machine != EM_RISCV ||
        eh->ehsize != sizeof(*eh) ||
        eh->shentsize != sizeof(struct sheader) || !eh->shnum ||
        eh->shstrndx >= eh->shnum || (eh->shoff & 3) ||
        !range_ok(size, eh->shoff, (IPTR)eh->shnum * eh->shentsize))
        goto bad;

    names = shdr(eh, eh->shstrndx);
    if (names->type != SHT_STRTAB || names->type == SHT_NOBITS ||
        !range_ok(size, names->offset, names->size))
        goto bad;

    for (i = 0; i < eh->shnum; i++)
    {
        struct sheader *sh = shdr(eh, i);

        if (sh->name >= names->size ||
            !bounded_string((const char *)eh + names->offset + sh->name,
                            names->size - sh->name))
            goto bad;
        if (sh->addralign && (sh->addralign & (sh->addralign - 1)))
            goto bad;
        if (sh->type != SHT_NOBITS && !range_ok(size, sh->offset, sh->size))
            goto bad;

        if (sh->type == SHT_RELA)
        {
            if ((sh->offset & 3) || sh->info >= eh->shnum ||
                sh->link >= eh->shnum || sh->entsize != sizeof(struct rela) ||
                sh->size % sizeof(struct rela))
                goto bad;
        }
        else if (sh->type == SHT_SYMTAB)
        {
            struct sheader *strings;

            if ((sh->offset & 3) || sh->link >= eh->shnum ||
                sh->entsize != sizeof(struct symbol) ||
                sh->size % sizeof(struct symbol))
                goto bad;
            strings = shdr(eh, sh->link);
            if (strings->type != SHT_STRTAB ||
                !range_ok(size, strings->offset, strings->size))
                goto bad;
        }
    }

    /* Validate symbol section indices, values and names before mutation. */
    for (i = 0; i < eh->shnum; i++)
    {
        struct sheader *syms = shdr(eh, i);
        struct sheader *strings;
        struct symbol *sym;
        IPTR count, j;

        if (syms->type != SHT_SYMTAB)
            continue;
        strings = shdr(eh, syms->link);
        sym = (struct symbol *)((UBYTE *)eh + syms->offset);
        count = syms->size / sizeof(*sym);

        for (j = 0; j < count; j++)
        {
            if (sym[j].name >= strings->size ||
                !bounded_string((const char *)eh + strings->offset + sym[j].name,
                                strings->size - sym[j].name))
                goto bad;
            if (sym[j].shindex < SHN_LORESERVE)
            {
                if (sym[j].shindex >= eh->shnum)
                    goto bad;
                if (sym[j].value > shdr(eh, sym[j].shindex)->size)
                    goto bad;
            }
        }
    }

    return 1;

bad:
    krnP4PutStr("[elf] malformed or incompatible ELF32 module: ");
    krnP4PutStr(name);
    krnP4PutStr("\n");
    return 0;
}

/*
 * Delete COUNT bytes from a placed section and update the same metadata GNU
 * ld updates in _bfd_riscv_relax_delete_bytes(): relocation offsets, symbol
 * values and the size of a symbol that spans the removed alignment padding.
 */
static int delete_section_bytes(struct elfheader *eh, unsigned int sec,
                                IPTR at, IPTR count)
{
    struct sheader *target = shdr(eh, sec);
    UBYTE *contents = (UBYTE *)(IPTR)target->addr;
    IPTR oldsize = target->size;
    unsigned int i;
    IPTR n;

    if (!count)
        return 1;
    if (!range_ok(oldsize, at, count))
        return 0;

    for (n = at; n + count < oldsize; n++)
        contents[n] = contents[n + count];
    zero_bytes(contents + oldsize - count, count);

    for (i = 0; i < eh->shnum; i++)
    {
        struct sheader *sh = shdr(eh, i);

        if (sh->type == SHT_RELA && sh->info == sec)
        {
            struct rela *rel = (struct rela *)((UBYTE *)eh + sh->offset);
            IPTR rels = sh->size / sizeof(*rel);
            IPTR j;

            for (j = 0; j < rels; j++)
            {
                if (rel[j].offset > at && rel[j].offset < oldsize)
                    rel[j].offset -= count;
            }
        }
        else if (sh->type == SHT_SYMTAB)
        {
            struct symbol *sym = (struct symbol *)((UBYTE *)eh + sh->offset);
            IPTR symbols = sh->size / sizeof(*sym);
            IPTR j;

            for (j = 0; j < symbols; j++)
            {
                IPTR value = sym[j].value;
                IPTR symsize = sym[j].size;

                if (sym[j].shindex != sec)
                    continue;
                if (value > at && value <= oldsize)
                    sym[j].value = value - count;
                else if (value <= at && symsize > at - value &&
                         symsize <= oldsize - value)
                    sym[j].size = symsize - count;
            }
        }
    }

    target->size = oldsize - count;
    return 1;
}

/*
 * R_RISCV_ALIGN reserves ADDEND bytes of maximum padding.  Its required
 * alignment is the first power of two greater than ADDEND; only the prefix
 * needed at the section's final address remains and the excess bytes are
 * deleted.  Leaving all NOPs in place, as the original loader did, shifts
 * every later symbol by two bytes in real rv32 AROS modules.
 */
static int relax_alignments(struct elfheader *eh, unsigned int sec,
                            const char *name)
{
    struct sheader *target = shdr(eh, sec);

    for (;;)
    {
        struct rela *best = NULL;
        IPTR bestoff = ~(IPTR)0;
        unsigned int i;
        IPTR maxpad, alignment, needed, delete_at, count, pos;
        UBYTE *contents;

        for (i = 0; i < eh->shnum; i++)
        {
            struct sheader *sh = shdr(eh, i);
            struct rela *rel;
            IPTR rels, j;

            if (sh->type != SHT_RELA || sh->info != sec)
                continue;
            rel = (struct rela *)((UBYTE *)eh + sh->offset);
            rels = sh->size / sizeof(*rel);
            for (j = 0; j < rels; j++)
            {
                if (ELF_R_TYPE(rel[j].info) == R_RISCV_ALIGN &&
                    rel[j].offset < bestoff)
                {
                    best = &rel[j];
                    bestoff = rel[j].offset;
                }
            }
        }

        if (!best)
            return 1;
        if (ELF_R_SYM(best->info) != 0 || best->addend < 0)
            goto bad;

        maxpad = (IPTR)best->addend;
        if (!range_ok(target->size, best->offset, maxpad) || (maxpad & 1))
            goto bad;

        alignment = 1;
        while (alignment <= maxpad)
        {
            if (alignment > ((IPTR)1 << 30))
                goto bad;
            alignment <<= 1;
        }

        needed = (-(target->addr + best->offset)) & (alignment - 1);
        if (needed > maxpad || (needed & 1))
            goto bad;

        contents = (UBYTE *)(IPTR)target->addr;
        for (pos = 0; pos + 4 <= needed; pos += 4)
            wr32le(contents + best->offset + pos, 0x00000013UL);
        if (pos < needed)
            wr16le(contents + best->offset + pos, 0x0001);

        /* Mark it handled before offsets are adjusted by the deletion. */
        best->info = ELF_R_INFO(0, R_RISCV_NONE);
        delete_at = best->offset + needed;
        count = maxpad - needed;
        if (!delete_section_bytes(eh, sec, delete_at, count))
            goto bad;
    }

bad:
    krnP4PutStr("[elf] invalid R_RISCV_ALIGN in ");
    krnP4PutStr(name);
    krnP4PutStr("\n");
    return 0;
}

static IPTR reloc_width(ULONG type)
{
    switch (type)
    {
    case R_RISCV_NONE:
    case R_RISCV_RELAX:
        return 0;
    case R_RISCV_SET6:
    case R_RISCV_SUB6:
    case R_RISCV_ADD8:
    case R_RISCV_SUB8:
    case R_RISCV_SET8:
        return 1;
    case R_RISCV_ADD16:
    case R_RISCV_SUB16:
    case R_RISCV_SET16:
    case R_RISCV_RVC_BRANCH:
    case R_RISCV_RVC_JUMP:
        return 2;
    case R_RISCV_64:
    case R_RISCV_ADD64:
    case R_RISCV_SUB64:
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT:
        return 8;
    default:
        return 4;
    }
}

static int krnRelocOne(ULONG type, UBYTE *loc, IPTR val, IPTR place)
{
    switch (type)
    {
    case R_RISCV_NONE:
    case R_RISCV_RELAX:
        break;

    case R_RISCV_64:
        wr64le(loc, (uint64_t)val);
        break;
    case R_RISCV_32:
        wr32le(loc, (uint32_t)val);
        break;
    case R_RISCV_32_PCREL:
        wr32le(loc, (uint32_t)(val - place));
        break;

    case R_RISCV_ADD8:  *loc = (UBYTE)(*loc + (UBYTE)val); break;
    case R_RISCV_SUB8:  *loc = (UBYTE)(*loc - (UBYTE)val); break;
    case R_RISCV_ADD16: wr16le(loc, rd16le(loc) + (uint16_t)val); break;
    case R_RISCV_SUB16: wr16le(loc, rd16le(loc) - (uint16_t)val); break;
    case R_RISCV_ADD32: wr32le(loc, rd32le(loc) + (uint32_t)val); break;
    case R_RISCV_SUB32: wr32le(loc, rd32le(loc) - (uint32_t)val); break;
    case R_RISCV_ADD64: wr64le(loc, rd64le(loc) + (uint64_t)val); break;
    case R_RISCV_SUB64: wr64le(loc, rd64le(loc) - (uint64_t)val); break;
    case R_RISCV_SET6:
        *loc = (*loc & 0xC0) | (val & 0x3F);
        break;
    case R_RISCV_SUB6:
        *loc = (*loc & 0xC0) | (((*loc & 0x3F) - val) & 0x3F);
        break;
    case R_RISCV_SET8:  *loc = (UBYTE)val; break;
    case R_RISCV_SET16: wr16le(loc, (uint16_t)val); break;
    case R_RISCV_SET32: wr32le(loc, (uint32_t)val); break;

    case R_RISCV_BRANCH:
    {
        SIPTR off = val - place;
        ULONG insn;

        if ((off & 1) || off < -4096 || off > 4094)
            return 0;
        insn = rd32le(loc) & 0x01FFF07F;
        insn |= ((off & 0x1000) << 19) | ((off & 0x07E0) << 20) |
                ((off & 0x001E) << 7) | ((off & 0x0800) >> 4);
        wr32le(loc, insn);
        break;
    }

    case R_RISCV_JAL:
    {
        SIPTR off = val - place;
        ULONG insn;

        if ((off & 1) || off < -1048576 || off > 1048574)
            return 0;
        insn = rd32le(loc) & 0x00000FFF;
        insn |= ((off & 0x100000) << 11) | ((off & 0x0007FE) << 20) |
                ((off & 0x000800) << 9) | (off & 0x0FF000);
        wr32le(loc, insn);
        break;
    }

    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT:
    {
        SIPTR off = val - place;
        ULONG hi = (off + 0x800) & 0xFFFFF000;
        ULONG lo = (off - hi) & 0xFFF;

        wr32le(loc, (rd32le(loc) & 0x00000FFF) | hi);
        wr32le(loc + 4, (rd32le(loc + 4) & 0x000FFFFF) | (lo << 20));
        break;
    }

    case R_RISCV_PCREL_HI20:
    {
        SIPTR off = val - place;
        ULONG hi = (off + 0x800) & 0xFFFFF000;

        wr32le(loc, (rd32le(loc) & 0x00000FFF) | hi);
        break;
    }

    case R_RISCV_HI20:
        wr32le(loc, (rd32le(loc) & 0x00000FFF) |
                    ((val + 0x800) & 0xFFFFF000));
        break;
    case R_RISCV_LO12_I:
        wr32le(loc, (rd32le(loc) & 0x000FFFFF) | ((val & 0xFFF) << 20));
        break;
    case R_RISCV_LO12_S:
    {
        ULONG v = val & 0xFFF;
        wr32le(loc, (rd32le(loc) & 0x01FFF07F) |
                    ((v & 0xFE0) << 20) | ((v & 0x1F) << 7));
        break;
    }

    case R_RISCV_RVC_BRANCH:
    {
        SIPTR off = val - place;
        UWORD insn;

        if ((off & 1) || off < -256 || off > 254)
            return 0;
        insn = rd16le(loc) & 0xE383;
        insn |= ((off & 0x100) << 4) | ((off & 0x018) << 7) |
                ((off & 0x0C0) >> 1) | ((off & 0x006) << 2) |
                ((off & 0x020) >> 3);
        wr16le(loc, insn);
        break;
    }

    case R_RISCV_RVC_JUMP:
    {
        SIPTR off = val - place;
        UWORD insn;

        if ((off & 1) || off < -2048 || off > 2046)
            return 0;
        insn = rd16le(loc) & 0xE003;
        insn |= ((off & 0x800) << 1) | ((off & 0x010) << 7) |
                ((off & 0x300) << 1) | ((off & 0x400) >> 2) |
                ((off & 0x040) << 1) | ((off & 0x080) >> 1) |
                ((off & 0x00E) << 2) | ((off & 0x020) >> 3);
        wr16le(loc, insn);
        break;
    }

    default:
        return 0;
    }
    return 1;
}

static int symbol_value(struct elfheader *eh, struct symbol *symtab,
                        IPTR symcount, ULONG index, IPTR addend, IPTR *value)
{
    struct symbol *sym;
    IPTR base;

    if (index >= symcount)
        return 0;
    sym = &symtab[index];

    if (sym->shindex == SHN_UNDEF || sym->shindex == SHN_COMMON ||
        sym->shindex == SHN_XINDEX)
        return 0;
    if (sym->shindex == SHN_ABS)
        base = 0;
    else
    {
        if (sym->shindex >= SHN_LORESERVE || sym->shindex >= eh->shnum ||
            !shdr(eh, sym->shindex)->addr)
            return 0;
        base = (IPTR)shdr(eh, sym->shindex)->addr;
    }

    *value = base + sym->value + addend;
    return 1;
}

/* Return 1 for a match, 0 for no match and -1 for malformed metadata. */
static int krnMatchHi20(struct elfheader *eh, struct rela *r2,
                        struct symbol *symtab, IPTR symcount, IPTR secaddr,
                        IPTR hiaddr, IPTR *hival)
{
    ULONG type = ELF_R_TYPE(r2->info);

    if ((type != R_RISCV_PCREL_HI20 && type != R_RISCV_GOT_HI20) ||
        secaddr + r2->offset != hiaddr)
        return 0;
    if (!symbol_value(eh, symtab, symcount, ELF_R_SYM(r2->info),
                      r2->addend, hival))
        return -1;
    return 1;
}

static int krnLoadModule(void *addr, IPTR size, const char *name)
{
    struct elfheader *eh = addr;
    unsigned int i, j;

    if (!validate_elf(eh, size, name))
        return 0;

    /* Clear file-supplied addresses, then place only runtime sections. */
    for (i = 0; i < eh->shnum; i++)
        shdr(eh, i)->addr = 0;

    mod_text = 0;
    for (i = 0; i < eh->shnum; i++)
    {
        struct sheader *sh = shdr(eh, i);
        UBYTE *p;

        if (!(sh->flags & SHF_ALLOC) || !sh->size)
            continue;

        if (section_named(eh, i, ".eh_frame"))
        {
            /*
             * There is no unwind consumer on this target, so do not spend
             * PSRAM or relocation time on this section.  Clear SHF_ALLOC as
             * well as leaving addr zero: debug.library registers every
             * non-empty allocated section as a runtime segment, and would
             * otherwise publish a fictitious module range starting at 0.
             */
            sh->flags &= ~SHF_ALLOC;
            continue;
        }

        p = krnBumpAlloc(sh->size, sh->addralign);
        if (!p)
            return 0;
        if (sh->type == SHT_NOBITS)
            zero_bytes(p, sh->size);
        else
            copy_bytes(p, (UBYTE *)eh + sh->offset, sh->size);
        sh->addr = (elf_ptr_t)(IPTR)p;

        if ((sh->flags & SHF_EXECINSTR) && !mod_text)
            mod_text = (IPTR)p;
    }

    /* Alignment relaxation changes symbol values and relocation offsets. */
    for (i = 0; i < eh->shnum; i++)
    {
        struct sheader *sh = shdr(eh, i);

        if (sh->addr && !relax_alignments(eh, i, name))
            return 0;
    }

    for (i = 0; i < eh->shnum; i++)
    {
        struct sheader *shrel = shdr(eh, i);
        struct sheader *target, *shsym, *shstr;
        struct symbol *symtab;
        const char *strtab;
        struct rela *rel;
        IPTR count, symcount;

        if (shrel->type != SHT_RELA)
            continue;
        target = shdr(eh, shrel->info);
        if (!(target->flags & SHF_ALLOC) || !target->addr)
            continue;

        shsym = shdr(eh, shrel->link);
        if (shsym->type != SHT_SYMTAB)
            return 0;
        shstr = shdr(eh, shsym->link);
        symtab = (struct symbol *)((UBYTE *)eh + shsym->offset);
        symcount = shsym->size / sizeof(*symtab);
        strtab = (const char *)eh + shstr->offset;
        rel = (struct rela *)((UBYTE *)eh + shrel->offset);
        count = shrel->size / sizeof(*rel);

        for (j = 0; j < count; j++, rel++)
        {
            ULONG type = ELF_R_TYPE(rel->info);
            ULONG symi = ELF_R_SYM(rel->info);
            IPTR width = reloc_width(type);
            UBYTE *loc;
            IPTR place, val;

            if (!range_ok(target->size, rel->offset, width))
                goto malformed_reloc;
            loc = (UBYTE *)(IPTR)target->addr + rel->offset;
            place = (IPTR)loc;

            if (type == R_RISCV_PCREL_LO12_I ||
                type == R_RISCV_PCREL_LO12_S)
            {
                struct rela *base =
                    (struct rela *)((UBYTE *)eh + shrel->offset);
                IPTR hiaddr, k, hival = 0;
                int found = 0;

                if (!symbol_value(eh, symtab, symcount, symi, 0, &hiaddr))
                    goto bad_symbol;

                for (k = j + 1; k-- > 0; )
                {
                    int match = krnMatchHi20(eh, &base[k], symtab, symcount,
                                             target->addr, hiaddr, &hival);
                    if (match < 0)
                        goto bad_symbol;
                    if (match)
                    {
                        found = 1;
                        break;
                    }
                }
                for (k = j + 1; !found && k < count; k++)
                {
                    int match = krnMatchHi20(eh, &base[k], symtab, symcount,
                                             target->addr, hiaddr, &hival);
                    if (match < 0)
                        goto bad_symbol;
                    if (match)
                        found = 1;
                }
                if (!found)
                {
                    krnP4PutStr("[elf] orphan PCREL_LO12 in ");
                    krnP4PutStr(name);
                    krnP4PutStr("\n");
                    return 0;
                }

                val = hival - hiaddr;
                {
                    SIPTR high = (val + 0x800) & ~0xFFFL;
                    val -= high;
                }
                if (type == R_RISCV_PCREL_LO12_I)
                    wr32le(loc, (rd32le(loc) & 0x000FFFFF) |
                                ((val & 0xFFF) << 20));
                else
                {
                    ULONG v = val & 0xFFF;
                    wr32le(loc, (rd32le(loc) & 0x01FFF07F) |
                                ((v & 0xFE0) << 20) | ((v & 0x1F) << 7));
                }
                continue;
            }

            if (symi == 0)
                val = rel->addend;
            else if (!symbol_value(eh, symtab, symcount, symi,
                                   rel->addend, &val))
            {
bad_symbol:
                krnP4PutStr("[elf] invalid/unplaced symbol in ");
                if (symi < symcount)
                    krnP4PutStr(strtab + symtab[symi].name);
                else
                    krnP4PutStr("<out-of-range>");
                krnP4PutStr(" (module ");
                krnP4PutStr(name);
                krnP4PutStr(")\n");
                return 0;
            }

            if (!krnRelocOne(type, loc, val, place))
            {
                krnP4PutStr("[elf] unsupported or out-of-range relocation ");
                krnP4PutDec(type);
                krnP4PutStr(" in ");
                krnP4PutStr(name);
                krnP4PutStr("\n");
                return 0;
            }
        }
    }

    return 1;

malformed_reloc:
    krnP4PutStr("[elf] relocation outside its target section in ");
    krnP4PutStr(name);
    krnP4PutStr("\n");
    return 0;
}

int krnLoadPackage(void *pkg, IPTR pkgsize, IPTR memlow, IPTR memhigh,
                   IPTR *lo, IPTR *hi, IPTR *memused)
{
    UBYTE *bytes = pkg;
    IPTR total, pos;
    int modules = 0;

    if (!lo || !hi || !memused)
        return 0;
    *lo = *hi = 0;
    *memused = memlow;

    if (!bytes || pkgsize < 8 || bytes[0] != 'P' || bytes[1] != 'K' ||
        bytes[2] != 'G' || bytes[3] != 0x01)
    {
        krnP4PutStr("[elf] not an AROS PKG v1 package\n");
        return 0;
    }

    total = rd32be(bytes + 4);
    if (total < 8 || total > pkgsize || memlow > memhigh)
    {
        krnP4PutStr("[elf] malformed package size\n");
        return 0;
    }

    alloc_ptr = memlow;
    alloc_end = memhigh;
    loaded_lo = ~(IPTR)0;
    loaded_hi = 0;
    __ks_debuginfo = NULL;
    mod_chain_tail = NULL;

    pos = 8;
    while (pos < total)
    {
        ULONG namelen, datalen;
        char *name;
        UBYTE *data;

        if (!range_ok(total, pos, 4))
            goto malformed_package;
        namelen = rd32be(bytes + pos);
        pos += 4;
        if (namelen == ~(ULONG)0 ||
            !range_ok(total, pos, (IPTR)namelen + 1))
            goto malformed_package;
        name = (char *)bytes + pos;
        if (name[namelen] != 0)
            goto malformed_package;
        pos += (IPTR)namelen + 1;

        if (!range_ok(total, pos, 4))
            goto malformed_package;
        datalen = rd32be(bytes + pos);
        pos += 4;
        if (!range_ok(total, pos, datalen))
            goto malformed_package;
        data = bytes + pos;
        pos += datalen;

        if (!datalen)
            continue;

        krnP4PutStr("[elf] loading ");
        krnP4PutStr(name);

        if (!krnLoadModule(data, datalen, name))
        {
            krnP4PutStr("[elf] FAILED to load ");
            krnP4PutStr(name);
            krnP4PutStr("\n");
            return 0;
        }

        krnP4PutStr(" .text @ ");
        krnP4PutHex32((uint32_t)mod_text);
        krnP4PutStr("\n");

        {
            struct ELF_ModuleInfo *mi = krnBumpAlloc(sizeof(*mi), 8);
            struct elfheader *eh = (struct elfheader *)data;
            unsigned int s;

            if (!mi)
                return 0;

            for (s = 0; s < eh->shnum; s++)
            {
                struct sheader *sh = shdr(eh, s);

                if (!(sh->flags & SHF_ALLOC) && sh->type != SHT_NOBITS &&
                    sh->size && !sh->addr)
                    sh->addr = (elf_ptr_t)(IPTR)((UBYTE *)eh + sh->offset);
            }

            mi->Next = NULL;
            mi->Name = name;
            mi->Type = DEBUG_ELF;
            mi->eh = eh;
            mi->sh = shdr(eh, 0);
            if (mod_chain_tail)
                mod_chain_tail->Next = mi;
            else
                __ks_debuginfo = mi;
            mod_chain_tail = mi;
        }
        modules++;
    }

    if (pos != total)
        goto malformed_package;

    *lo = loaded_lo;
    *hi = loaded_hi;
    *memused = alloc_ptr;
    return modules;

malformed_package:
    krnP4PutStr("[elf] malformed package entry\n");
    return 0;
}
