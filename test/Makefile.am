# Help the Developers and yourself. Just use the C locale and settings
# for the compilation. They can still be overriden by make LANG=<whatever>
# but that is general a not very good idea
include $(top_builddir)/version.mk

LANG = C
LC_ALL = C

AM_CFLAGS = $(EXTRA_CFLAGS)
AM_CPPFLAGS = -D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600 -D_BSD_SOURCE
AM_CPPFLAGS += -DTEST

EXTRA_DIST = $(BUILT_SOURCES) $(TESTS)
TESTS =
TEST_EXTENSIONS =
BUILT_SOURCES =
dt_tests =
bin_tests =
batch_tests =
check_PROGRAMS =
CLEANFILES = $(check_PROGRAMS)

## summon our helper
include clitoris.am
AM_CLIT_LOG_FLAGS = -v --builddir $(top_builddir)/src
TESTS_ENVIRONMENT = root=$(top_srcdir) AEIOU_TRDIR=$(top_builddir)/src


TESTS += sample_01.clit
TESTS += sample_02.clit
TESTS += sample_03.clit
TESTS += sample_04.clit
TESTS += sample_05.clit
TESTS += sample_06.clit
TESTS += sample_07.clit
TESTS += sample_08.clit
TESTS += sample_09.clit
TESTS += sample_10.clit
TESTS += sample_11.clit
TESTS += sample_12.clit
TESTS += sample_13.clit
TESTS += sample_14.clit
TESTS += sample_15.clit
TESTS += sample_16.clit
TESTS += sample_17.clit
TESTS += sample_18.clit
TESTS += sample_19.clit
TESTS += sample_20.clit
TESTS += sample_21.clit
TESTS += sample_22.clit
TESTS += sample_23.clit
TESTS += sample_24.clit
TESTS += sample_25.clit
TESTS += sample_26.clit
TESTS += sample_27.clit
TESTS += sample_28.clit

## Makefile.am ends here
