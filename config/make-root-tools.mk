# Host tools that must be complete before MetaMake starts parallel recursive
# directory builds. A per-directory makefile can otherwise notice a missing
# genmodule binary and race another recursive make that is writing the same
# executable.

$(GENMODULE): $(wildcard $(SRCDIR)/tools/genmodule/*.[ch]) | makedirs
	@$(ECHO) Building genmodule...
	@$(CALL) $(MAKE) $(MKARGS) -C $(SRCDIR)/tools/genmodule SRCDIR=$(SRCDIR) TOP=$(TOP) $(GENMODULE)

tools: $(GENMODULE)
