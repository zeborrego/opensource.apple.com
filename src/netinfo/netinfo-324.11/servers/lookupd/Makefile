#
# Generated by the Apple Project Builder.
#
# NOTE: Do NOT change this file -- Project Builder maintains it.
#
# Put all of your customizations in files called Makefile.preamble
# and Makefile.postamble (both optional), and Makefile will include them.
#

NAME = lookupd

PROJECTVERSION = 2.8
PROJECT_TYPE = Tool

CLASSES = CacheAgent.m Config.m Controller.m DNSAgent.m Dyna.m LUAgent.m\
          LUArray.m LUCache.m LUDictionary.m LUServer.m MachRPC.m\
          MemoryWatchdog.m NILAgent.m Root.m Thread.m

HFILES = CacheAgent.h Config.h Controller.h DNSAgent.h Dyna.h LUAgent.h\
         LUArray.h LUCache.h LUCachedDictionary.h LUDictionary.h\
         LUGlobal.h LUPrivate.h LUServer.h MachRPC.h MemoryWatchdog.h\
		 NILAgent.h Root.h Thread.h lu_xdr.h sys.h

MFILES = Interactive.m lookup_proc.m lookupd.m

CFILES = lu_xdr.c nettoa.c sys.c

OTHERSRCS = Makefile.preamble Makefile Makefile.postamble lookup.defs\
            lookupd.8 lookupd.plist _lu_types.x

MAKEFILEDIR = $(MAKEFILEPATH)/pb_makefiles
CODE_GEN_STYLE = DYNAMIC
MAKEFILE = tool.make
NEXTSTEP_INSTALLDIR = /usr/sbin
WINDOWS_INSTALLDIR = /usr/etc
PDO_UNIX_INSTALLDIR = /usr/etc
LIBS = -lobjc -lresolv
DEBUG_LIBS = $(LIBS)
PROF_LIBS = $(LIBS)

FRAMEWORK_PATHS = -F$(DSTROOT)$(SYSTEM_LIBRARY_DIR)/PrivateFrameworks
FRAMEWORKS = -framework NetInfo

NEXTSTEP_OBJCPLUS_COMPILER = /usr/bin/cc
WINDOWS_OBJCPLUS_COMPILER = $(DEVDIR)/gcc
PDO_UNIX_OBJCPLUS_COMPILER = $(NEXTDEV_BIN)/gcc
NEXTSTEP_JAVA_COMPILER = /usr/bin/javac
WINDOWS_JAVA_COMPILER = $(JDKBINDIR)/javac.exe
PDO_UNIX_JAVA_COMPILER = $(JDKBINDIR)/javac

include $(MAKEFILEDIR)/platform.make

-include Makefile.preamble

include $(MAKEFILEDIR)/$(MAKEFILE)

-include Makefile.postamble

-include Makefile.dependencies
