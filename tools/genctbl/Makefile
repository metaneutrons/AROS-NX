#   Copyright © 2025, The AROS Development Team. All rights reserved.
#   $Id$
#
#   Desc: Makefile to create genctbl
#

USER_CFLAGS := -Wall -Werror -Wunused -O2

-include $(TOP)/config/make.cfg
-include Makefile.deps

HOST_CC	       ?= gcc
HOST_STRIP     ?= strip
MECHO	       ?= echo
HOST_CFLAGS    ?= $(USER_CFLAGS)
GENCTBL        ?= genctbl
GENDIR         ?= ./
MKDIR          ?= mkdir
FETCH          ?= $(SRCDIR)/scripts/fetch.sh

# Character tables are source inputs, not a moving `latest` dependency.
# This version matches the sealed native-consumer UCD inputs. The complete
# archive is checked before extraction, also by the native producer bridge.
GENCTBL_UCD_VERSION := 16.0.0
GENCTBL_UCD_SHA256 := c86dd81f2b14a43b0cc064aa5f89aa7241386801e35c59c7984e579832634eb2
GENCTBL_UCD_READY := $(GENDIR)/ucd/.ucd-$(GENCTBL_UCD_VERSION)-ready

# Re-extract from the checked archive if an incremental build lost either
# generated input. A stamp alone is not evidence that both files still exist.
ifneq ($(words $(wildcard $(GENDIR)/ucd/UnicodeData.txt $(GENDIR)/ucd/SpecialCasing.txt)),2)
.PHONY : $(GENCTBL_UCD_READY)
endif

all : $(GENCTBL) $(GENDIR)/ucd/UnicodeData.txt $(GENDIR)/ucd/SpecialCasing.txt

$(PORTSSOURCEDIR) :
	@$(MKDIR) -p $@

$(GENDIR)/ucd :
	@$(MKDIR) -p $@

$(GENCTBL_UCD_READY) : $(SRCDIR)/tools/genctbl/Makefile | $(GENDIR)/ucd $(PORTSSOURCEDIR)
	@$(ECHO) "Preparing verified Unicode $(GENCTBL_UCD_VERSION) data..."
	@$(FETCH) -ao "https://www.unicode.org/Public/$(GENCTBL_UCD_VERSION)/ucd" \
	    -a UCD -s zip -l "$(PORTSSOURCEDIR)" -d "$(GENDIR)/ucd" -b "$(GENDIR)/ucd" \
	    -cs "UCD.zip=sha256:$(GENCTBL_UCD_SHA256)" -f
	@test -s "$(GENDIR)/ucd/UnicodeData.txt" -a -s "$(GENDIR)/ucd/SpecialCasing.txt"
	@touch "$@"

$(GENDIR)/ucd/UnicodeData.txt $(GENDIR)/ucd/SpecialCasing.txt : $(GENCTBL_UCD_READY)
	@test -s "$@"

$(GENCTBL) : genctbl.c $(SRCDIR)/tools/genctbl/Makefile $(GENMODULE_DEPS)
	@$(ECHO) "Compiling $(notdir $@)..."
	@$(HOST_CC) -g $(HOST_CFLAGS) -I$(GENINCDIR) -I$(TOP)/$(CURDIR) genctbl.c -o $@

clean :
	@$(RM) $(GENCTBL)

.PHONY : all clean
