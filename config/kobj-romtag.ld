/*
 * Section-ordering script for the partial link of a kickstart module,
 * config/make.tmpl's `$(AROS_LD) -Ur ... $(KERNEL_KOBJ_LDSCRIPT)`.
 *
 * The romtag scanner leaps from a module's Resident tag to rt_EndSkip
 * (rom/kernel/kernel_romtags.c:267), which genmodule points at the module's
 * End marker (tools/genmodule/writeend.c). The leap is only correct while the
 * marker lies behind the module's own tag and in front of the next module's
 * tag.
 *
 * Nothing guarantees that while the tags and the markers live in sections of
 * their own. The kickstart link merges every .text.romtag into one block and
 * every .text.moduleend into another block behind it, so the first tag the
 * scanner reads has an rt_EndSkip pointing past all the others: one module
 * gets registered and the rest are skipped. On x86_64-pc that is the whole
 * boot, which ends in
 *
 *     Failed to create ExecBase
 *     exec.library is not found
 *
 * Folding the three sections into the module's own .text, in this order, makes
 * each module one contiguous block with its tag first and its marker last, so
 * the leap lands exactly in front of the next module's tag.
 *
 * The patterns must not overlap. `*(.text .text.*)` would claim
 * .text.moduleend before the last pattern is reached and put the marker in the
 * middle of the module, which is the same defect again. Sections not named
 * here -- .rodata, the linker sets, .aros.startup, a compiler's .text.unlikely
 * -- keep their own output sections; they hold no tags, so a scanner walking
 * them costs time and nothing else.
 *
 * arch/m68k-amiga/boot/kobj-romtag.ld overrides this for amiga-m68k, where
 * .rodata is folded in as well to cut the walk over a 1MB ROM.
 */
SECTIONS
{
  .text : {
    *(.text.romtag)
    *(.text)
    *(.text.moduleend)
  }
}
