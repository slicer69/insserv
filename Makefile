#
# Makefile for compiling insserv tool
#
# Author: Werner Fink,  <werner@suse.de>
#

INITDIR  =	/etc/init.d
INSCONF  =	/etc/insserv.conf
#DESTDIR =	/tmp/root
#DEBUG	 =	-DDEBUG=1 -Wpacked
DEBUG	 =
ISSUSE	 =	-DSUSE
DESTDIR	 =
VERSION	 =	1.13.0
DATE	 =	$(shell date +'%d%b%y' | tr '[:lower:]' '[:upper:]')

#
# Architecture
#
ifdef RPM_OPT_FLAGS
	  COPTS = -g $(RPM_OPT_FLAGS)
else
	   ARCH = $(shell uname -i)
ifeq ($(ARCH),i386)
	  COPTS = -g -O3 -mcpu=i586 -mtune=i686
else
	  COPTS = -g -O2
endif
endif
	 CFLAGS = -W -Wall $(COPTS) $(DEBUG) $(LOOPS) -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
		  $(ISSUSE) -DINITDIR=\"$(INITDIR)\" -DINSCONF=\"$(INSCONF)\" -pipe
	  CLOOP = -falign-loops=0
	LDFLAGS = -Wl,-O,3,--relax
	   LIBS =
ifdef USE_RPMLIB
	 CFLAGS += -DUSE_RPMLIB=1
	LDFLAGS += -Wl,--as-needed
	   LIBS += -lrpm
endif
	     CC = gcc
	     RM = rm -f
	  MKDIR = mkdir -p
	  RMDIR = rm -rf
   INSTBINFLAGS = -m 0700
	INSTBIN = install $(INSTBINFLAGS)
   INSTSRPFLAGS = -m 0700
	INSTSRP = install $(INSTSRPFLAGS)
   INSTDOCFLAGS = -c -m 0444
	INSTDOC = install $(INSTDOCFLAGS)
   INSTCONFLAGS = -c -m 0644
	INSTCON = install $(INSTDOCFLAGS)
	   LINK = ln -sf
#
	SDOCDIR = $(DESTDIR)/usr/share/man/man8
	SBINDIR = $(DESTDIR)/sbin
	CONFDIR = $(DESTDIR)/etc
	 LSBDIR = $(DESTDIR)/lib/lsb
      USRLSBDIR = $(DESTDIR)/usr/lib/lsb

#
# Determine if a library provides a specific function
# First argument is the function to test, the second
# one is the library its self.
#
	  CTEST = $(CC) -nostdinc -fno-builtin -o /dev/null -xc
    cc-function = $(shell printf 'void *$(1)();\nint main(){return($(1)(0)?0:1);}'|$(CTEST) - -l$(2:lib%=%) > /dev/null 2>&1 && echo $(1))

#
# The rules
#

TODO	=	insserv insserv.8

all:		$(TODO)

insserv:	insserv.o listing.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

listing.o:	listing.c listing.h config.h .system
	$(CC) $(CFLAGS) $(CLOOP) -c $<

insserv.o:	insserv.c listing.h config.h .system
	$(CC) $(CFLAGS) $(CLOOP) -c $<

listing.h:	.system

config.h:	ADRESSES  = ^\#\s*if\s+defined\(HAS_[[:alnum:]_]+\)\s+&&\s+defined\(_ATFILE_SOURCE\)
config.h:	FUNCTIONS = $(shell sed -rn '/$(ADRESSES)/{ s/.*defined\(HAS_([[:alnum:]_]+)\).*/\1/p; }' listing.h)
config.h:	listing.h
	@echo '/* Generated automatically by running make -- do not edit */'  > config.h
	@echo '#ifndef CONFIG_H' >> config.h
	@echo '#define CONFIG_H' >> config.h
	@for def in $(foreach func,$(FUNCTIONS),$(call cc-function,$(func),libc)); do \
	    echo "#define HAS_$$def"; \
	 done >> config.h
	@echo '#endif' >> config.h

ifeq ($(ISSUSE),-DSUSE)
insserv.8:	insserv.8.in .system
	sed -r '\!@@(ELSE|NOT)_SUSE@@!,\!@@END_SUSE@@!d;\!@@(BEGIN|END)_SUSE@@!d' < $< > $@
else
insserv.8:	insserv.8.in .system
	sed -r '\!@@BEGIN_SUSE@@!,\!@@(ELSE|END)_SUSE@@!d;\!@@(NOT|END)_SUSE@@!d' < $< > $@
endif

.system:	SYSTEM=$(shell cat .system 2> /dev/null)
.system:	.force
	@test "$(SYSTEM)" = "$(ISSUSE)$(DEBUG)" || echo "$(ISSUSE)$(DEBUG)" > .system

.force:

.PHONY:		clean
clean:
	$(RM) *.o *~ $(TODO) config.h .depend.* .system

ifneq ($(MAKECMDGOALS),clean)

-include	.depend.listing .depend.insserv

.depend.listing::	listing.c listing.h
	@$(CC) $(CFLAGS) -M listing.c >$@ 2>/dev/null

.depend.insserv::	insserv.c listing.h
	@$(CC) $(CFLAGS) -M insserv.c >$@ 2>/dev/null

endif

install:	$(TODO)
	$(MKDIR)   $(SBINDIR)
	$(MKDIR)   $(SDOCDIR)
	$(MKDIR)   $(CONFDIR)
ifeq ($(ISSUSE),-DSUSE)
	$(MKDIR)   $(LSBDIR)
	$(MKDIR)   $(DESTDIR)/usr/lib
	$(MKDIR)   $(USRLSBDIR)
endif
	$(INSTBIN) insserv        $(SBINDIR)/
	$(INSTDOC) insserv.8      $(SDOCDIR)/
	$(INSTCON) insserv.conf   $(CONFDIR)/
ifeq ($(ISSUSE),-DSUSE)
	$(INSTCON) init-functions $(LSBDIR)/
	$(INSTSRP) install_initd  $(USRLSBDIR)/
	$(INSTSRP) remove_initd   $(USRLSBDIR)/
endif

#
# Make distribution
#
FILES	= README         \
	  COPYING        \
	  CHANGES        \
	  Makefile       \
	  listing.c      \
	  listing.h      \
	  insserv.8.in   \
	  insserv.c      \
	  insserv.conf   \
	  init-functions \
	  remove_initd   \
	  install_initd  \
	  tests/suite    \
	  insserv-$(VERSION).lsm

dest:	clean
	$(MKDIR) insserv-$(VERSION)/tests
	@echo -e "Begin3\n\
Title:		insserv tool for boot scripts\n\
Version:	$(VERSION)\n\
Entered-date:	$(DATE)\n\
Description:	Used for enabling of installed boot scripts\n\
x 		by scanning comment headers which are LSB conform.\n\
Keywords:	boot service control, LSB\n\
Author:		Werner Fink <werner@suse.de>\n\
Maintained-by:	Werner Fink <werner@suse.de>\n\
Primary-site:	sunsite.unc.edu /pub/Linux/system/daemons/init\n\
x		@UNKNOWN insserv-$(VERSION).tar.bz2\n\
Alternate-site:	ftp.suse.com /pub/projects/init\n\
Platforms:	Linux with System VR2 or higher boot scheme\n\
Copying-policy:	GPL\n\
End" | sed 's@^ @@g;s@^x@@g' > insserv-$(VERSION).lsm
	for file in $(FILES) ; do \
	    case "$$file" in \
	    tests/*) cp -p $$file insserv-$(VERSION)/tests/ ;; \
	    *)	     cp -p $$file insserv-$(VERSION)/ ;; \
	    esac; \
	done
	tar -cps -jf  insserv-$(VERSION).tar.bz2 insserv-$(VERSION)/
	$(RMDIR)    insserv-$(VERSION)
	set -- `find insserv-$(VERSION).tar.bz2 -printf '%s'` ; \
	sed "s:@UNKNOWN:$$1:" < insserv-$(VERSION).lsm > \
	insserv-$(VERSION).lsm.tmp ; \
	mv insserv-$(VERSION).lsm.tmp insserv-$(VERSION).lsm
