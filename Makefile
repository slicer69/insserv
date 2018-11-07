#
# Makefile for compiling insserv tool
#
# Author: Werner Fink,  <werner@suse.de>
#

PACKAGE	 =	insserv
INITDIR  =	/etc/init.d
INSCONF  =	/etc/insserv.conf
#DESTDIR =	/tmp/root
#DEBUG	 =	-DDEBUG=1 -Wpacked
DEBUG	 =
#ISSUSE	 =	-DSUSE
DESTDIR	 =
VERSION	 =	1.18.0
TARBALL  =	$(PACKAGE)-$(VERSION).tar.xz
DATE	 =	$(shell date +'%d%b%y' | tr '[:lower:]' '[:upper:]')
CFLDBUS	 =	$(shell pkg-config --cflags dbus-1)

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
	 CFLAGS = -W -Wall -Wunreachable-code $(COPTS) $(DEBUG) $(LOOPS) -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
		  $(ISSUSE) -DINITDIR=\"$(INITDIR)\" -DINSCONF=\"$(INSCONF)\" -pipe 
	  CLOOP = # -falign-loops=0
	LDFLAGS = -Wl,-O,3,--relax
	   LIBS =
ifdef USE_RPMLIB
	 CFLAGS += -DUSE_RPMLIB=1
	LDFLAGS += -Wl,--as-needed
	   LIBS += -lrpm
endif
	   LIBS += $(shell pkg-config --libs dbus-1)
	     CC ?= gcc
	     RM = rm -f
	  MKDIR = mkdir -p
	  RMDIR = rm -rf
   INSTBINFLAGS = -m 0755
	INSTBIN = install $(INSTBINFLAGS)
   INSTSRPFLAGS = -m 0755
	INSTSRP = install $(INSTSRPFLAGS)
   INSTDOCFLAGS = -c -m 0644
	INSTDOC = install $(INSTDOCFLAGS)
   INSTCONFLAGS = -c -m 0644
	INSTCON = install $(INSTCONFLAGS)
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

insserv:	insserv.o listing.o systemd.o map.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

listing.o:	listing.c insserv.c listing.h config.h .system
	$(CC) $(CFLAGS) $(CLOOP) $(CFLDBUS) -c $<

map.o:	map.c listing.h config.h .system
	$(CC) $(CFLAGS) $(CLOOP) $(CFLDBUS) -c $<

insserv.o:	insserv.c map.o listing.h systemd.h config.h .system
	$(CC) $(CFLAGS) $(CLOOP) $(CFLDBUS) insserv.c -c 

systemd.o:	systemd.c map.o listing.h systemd.h config.h .system
	$(CC) $(CFLAGS) $(CLOOP) $(CFLDBUS) -c $<

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

.PHONY:		clean distclean
distclean: clean
clean:
	$(RM) *.o *~ $(TODO) config.h .depend.* .system

ifneq ($(MAKECMDGOALS),clean)

-include	.depend.listing .depend.insserv

.depend.listing::	listing.c listing.h
	@$(CC) $(CFLAGS) -M listing.c >$@ 2>/dev/null

.depend.insserv::	insserv.c listing.h
	@$(CC) $(CFLAGS) -M insserv.c >$@ 2>/dev/null

endif

check: insserv
ifeq ($(ISSUSE),-DSUSE)
	issuse=true tests/common
#	issuse=true tests/suse
else
	tests/common
endif

install:	$(TODO) check
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
	  systemd.c      \
	  systemd.h      \
	  insserv.8.in   \
	  insserv.c      \
	  insserv.conf   \
	  init-functions \
          map.c          \
	  remove_initd   \
	  install_initd 

SVLOGIN=$(shell svn info | sed -rn '/Repository Root:/{ s|.*//(.*)\@.*|\1|p }')
ifeq ($(MAKECMDGOALS),upload)
override TMP:=$(shell mktemp -d $(PACKAGE)-$(VERSION).XXXXXXXX)
override SFTPBATCH:=$(TMP)/$(VERSION)-sftpbatch
override LSM=$(TMP)/$(PACKAGE)-$(VERSION).lsm

$(LSM):	$(TMP)/$(PACKAGE)-$(VERSION)
	@echo -e "Begin3\n\
Title:		insserv tool for boot scripts\n\
Version:	$(VERSION)\n\
Entered-date:	$(DATE)\n\
Description:	Used for enabling of installed boot scripts\n\
x 		by scanning comment headers which are LSB conform.\n\
Keywords:	boot service control, LSB\n\
Author:		Werner Fink <werner@suse.de>\n\
Maintained-by:	Werner Fink <werner@suse.de>\n\
Primary-site:	http://download.savannah.gnu.org/releases/sysvinit/\n\
x		@UNKNOWN $(PACKAGE)-$(VERSION).tar.xz\n\
Alternate-site:	ftp.suse.com /pub/projects/init\n\
Platforms:	Linux with System VR2 or higher boot scheme\n\
Copying-policy:	GPL\n\
End" | sed 's@^ @@g;s@^x@@g' > $(LSM)

dest: $(LSM)
endif

upload: $(SFTPBATCH)
	@sftp -b $< $(SVLOGIN)@dl.sv.nongnu.org:/releases/sysvinit
	mv $(TARBALL) $(LSM) .
	rm -rf $(TMP)

dist: $(TARBALL).sig


$(TARBALL).sig: $(TARBALL)
	@gpg -q -ba --use-agent -o $@ $<

$(TARBALL): clean
	mkdir -p insserv/tests
	cp $(FILES) insserv/
	cp tests/* insserv/tests/
	@tar --xz --owner=nobody --group=nobody -cf $(TARBALL) insserv/
	rm -rf insserv

distclean: clean
	rm -f $(TARBALL) $(TARBALL).sig

