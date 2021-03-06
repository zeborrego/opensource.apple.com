##
# Makefile for ruby

Project               = ruby
# Ruby will not compile with gcc3 if -O>2
CC_Optimize            = -Os
Extra_CC_Flags         = -no-cpp-precomp -fno-common -DHAVE_INTTYPES_H
Extra_Configure_Flags  = --with-sitedir=/usr/local/lib/ruby/site_ruby
GnuAfterInstall        = post-install

# It's a GNU Source project
include $(MAKEFILEPATH)/CoreOS/ReleaseControl/GNUSource.make
Install_Flags   = DESTDIR="$(DSTROOT)"
Install_Target  = install

MAJOR = 1
MINOR = 8
VERSION = $(MAJOR).$(MINOR)
SYSSTRING = `uname -p`-darwin

post-install:
	strip -x $(DSTROOT)/usr/bin/ruby
	cc $(CFLAGS) -dynamiclib \
		-install_name /usr/lib/libruby.$(MAJOR).dylib \
		-compatibility_version $(VERSION) -current_version $(VERSION) \
		-all_load -o $(DSTROOT)/usr/lib/libruby.$(MAJOR).dylib \
		$(OBJROOT)/libruby-static.a
	ln -s libruby.$(MAJOR).dylib $(DSTROOT)/usr/lib/libruby.dylib
	ln -s libruby.$(MAJOR).dylib $(DSTROOT)/usr/lib/libruby.$(VERSION).dylib
	rm $(DSTROOT)/usr/lib/libruby-static.a
	strip -x $(DSTROOT)/usr/lib/ruby/$(VERSION)/$(SYSSTRING)/*.bundle
	strip -x $(DSTROOT)/usr/lib/ruby/$(VERSION)/$(SYSSTRING)/*/*.bundle
	strip -x $(DSTROOT)/usr/lib/libruby.$(MAJOR).dylib
