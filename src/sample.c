/*** sample.c -- output a sample of a file
 *
 * Copyright (C) 2016-2017 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of sample.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/
#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <assert.h>
#include "pcg_basic.h"
#include "nifty.h"

#if defined BUFSIZ
# undef BUFSIZ
#endif	/* BUFSIZ */
#define BUFSIZ	(65536U)

static size_t nheader = 5U;
static size_t nfooter = 5U;
static unsigned int rate = 10;


static void
__attribute__((format(printf, 1, 2)))
error(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputc(':', stderr);
		fputc(' ', stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}


/* buffer */
static char *buf;
static size_t zbuf;

static int
init_rng(uint64_t seed)
{
	uint64_t stid = 0ULL;

	if (!seed) {
		seed = time(NULL);
		stid = (intptr_t)&seed;
	}
	pcg32_srandom(seed, stid);
	return 0;
}

static int
sample_gen(int fd)
{
/* generic sampler */
	/* number of lines read so far */
	size_t nfln = 0U;
	/* number of output lines so far */
	size_t noln = 0U;
	/* ring buffer index */
	size_t nftr = 0U;
	/* fill of BUF, also used as offset of end-of-header in HDR mode */
	size_t nbuf = 0U;
	/* index into BUF to the beginning of the last unprocessed line */
	size_t ibuf = 0U;
	/* number of octets read per read() */
	ssize_t nrd;
	/* offsets to footer */
	size_t last[nfooter + 1U];
	/* 3 major states, HEAD BEEF and TAIL */
	enum {
		HEAD,
		BEEF,
		TAIL
	} state = HEAD;

	with (char *tmp = realloc(buf, BUFSIZ)) {
		if (UNLIKELY(tmp == NULL)) {
			/* just bugger off */
			return -1;
		}
		/* otherwise swap ptrs */
		buf = tmp;
		zbuf = BUFSIZ;
	}
	/* clean up last array */
	memset(last, 0, sizeof(last));

	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		/* calc next round's NBUF already */
		nbuf += nrd;

		switch (state) {
		case HEAD:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				const size_t o = ibuf;

				ibuf = ++x - buf;
				fwrite(buf + o, sizeof(*buf), ibuf - o, stdout);
				noln++;

				if (++nfln >= nheader) {
					goto tail;
				}
			}
			break;

		tail:
			state = TAIL;
		case TAIL:
			for (const char *x;
			     (x = memchr(buf +ibuf, '\n', nbuf - ibuf));) {
				/* keep track of footers */
				last[nftr] = ibuf;
				nftr = (nftr + 1U) % countof(last);
				ibuf = ++x - buf;

				if (++nfln > nheader + nfooter && rate) {
					goto beef;
				}
			}
			goto over;

		beef:
			fwrite("...\n", 1, 4U, stdout);
			state = BEEF;
			/* we need one more sample step because the
			 * condition above that got us here goes one
			 * step further than it should, lest we print
			 * the ellipsis when there's exactly
			 * nheader + nfoooter lines in the buffer */
			goto sample;
		case BEEF:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				/* keep track of footers */
				last[nftr] = ibuf;
				nftr = (nftr + 1U) % countof(last);
				ibuf = ++x - buf;

				nfln++;

#define LAST(x)		last[(x) % countof(last)]
			sample:
				/* sample */
				if (!pcg32_boundedrand(rate)) {
					const size_t this = last[nftr];
					const size_t next = LAST(nftr + 1U);

					fwrite(buf + this, sizeof(*buf),
					       next - this, stdout);
					noln++;
				}
			}
		over:
			/* beef buffer overrun */
			with (const size_t nu = last[nftr]) {
				memmove(buf, buf + nu, nbuf - nu);
				for (size_t i = 0U; i < countof(last); i++) {
					last[i] -= nu;
				}
				nbuf -= nu;
				ibuf -= nu;
			}
			/* keep track of last footer */
			last[nftr] = ibuf;
			break;
		}
	}
	if (LIKELY(nfln > nheader + nfooter)) {
		nftr++;
	} else {
		nftr = 0U;
	}
	if (noln > nheader ||
	    !rate && nfln > nheader + nfooter) {
		fwrite("...\n", 1, 4U, stdout);
	}
	/* fast forward footer if there wasn't enough lines */
	for (size_t i = nftr, this, next;
	     i <= nftr + nfooter &&
		     (this = LAST(i), next = LAST(i + 1U), this < next);
	     i++) {
		fwrite(buf + this, sizeof(*buf), next - this, stdout);
	}
	return 0;
}

static int
sample(const char *fn)
{
	struct stat st;
	int rc = 0;
	int fd;

	if (fn == NULL || fn[0U] == '-' && fn[1U] == '\0') {
		/* stdin ... *sigh* */
		fd = STDIN_FILENO;
		return sample_gen(fd);
	} else if (UNLIKELY((fd = open(fn, O_RDONLY)) < 0)) {
		error("\
Error: cannot open file `%s'", fn);
		return -1;
	} else if (UNLIKELY(fstat(fd, &st) < 0)) {
		error("\
Error: cannot stat file `%s'", fn);
		rc = -1;
	} else if (!S_ISREG(st.st_mode)) {
		/* fgetln/getline */
		rc = sample_gen(fd);
	} else {
		rc = sample_gen(fd);
	}

	close(fd);
	return rc;
}


#include "sample.yucc"

int
main(int argc, char *argv[])
{
	yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv)) {
		rc = 1;
		goto out;
	}

	if (argi->girdle_arg) {
		nheader = nfooter = strtoul(argi->girdle_arg, NULL, 0);
	}
	if (argi->header_arg) {
		nheader = strtoul(argi->header_arg, NULL, 0);
	}
	if (argi->footer_arg) {
		nfooter = strtoul(argi->footer_arg, NULL, 0);
	}
	if (argi->rate_arg) {
		char *on;
		double x = strtod(argi->rate_arg, &on);

		if (x <= 0.) {
			rate = 0;
		} else if (*on == '%') {
			rate = (unsigned int)(100. / x);
		} else if (x < 1.) {
			rate = (unsigned int)(1. / x);
		} else {
			rate = (unsigned int)x;
		}
	}

	/* initialise randomness */
	init_rng(0);

	for (size_t i = 0U; i < argi->nargs + !argi->nargs; i++) {
		rc |= sample(argi->args[i]) < 0;
	}

	if (LIKELY(buf != NULL)) {
		free(buf);
	}

out:
	yuck_free(argi);
	return rc;
}

/* sample.c ends here */
