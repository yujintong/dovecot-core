noinst_LTLIBRARIES = \
	libdict.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-var-expand

base_sources = \
	dict.c \
	dict-file.c \
	dict-redis.c \
	dict-fail.c \
	dict-transaction-memory.c

libdict_la_SOURCES = \
	$(base_sources)

headers = \
	dict.h \
	dict-private.h \
	dict-transaction-memory.h

# Internally, the dict methods yield via lua_yieldk() as implemented in Lua
# 5.3 and newer.
if DLUA_WITH_YIELDS
noinst_LTLIBRARIES += libdict_lua.la

libdict_lua_la_SOURCES = \
	dict-lua.c \
	dict-iter-lua.c \
	dict-txn-lua.c
libdict_lua_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(LUA_CFLAGS) \
	-I$(top_srcdir)/src/lib-lua
libdict_lua_la_LIBADD =
libdict_lua_la_DEPENDENCIES = \
	libdict.la

headers += \
	dict-lua.h \
	dict-lua-private.h
endif

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)

test_programs = \
	test-dict

noinst_PROGRAMS = $(test_programs)

test_libs = \
	libdict.la \
	../lib-settings/libsettings.la \
	../lib-dns/libdns.la \
	../lib-var-expand/libvar_expand.la \
	../lib-test/libtest.la \
	../lib/liblib.la

test_dict_SOURCES = test-dict.c
test_dict_LDADD = $(test_libs) $(DLLIB)
test_dict_DEPENDENCIES = $(test_libs)

check-local:
	for bin in $(test_programs) $(check_PROGRAMS); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done
