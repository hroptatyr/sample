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
#include <sys/stat.h>
#include <assert.h>
#include "nifty.h"

#if !defined BUFSIZ
# define BUFSIZ	(4096U)
#endif	/* !BUFSIZ */

static size_t nheader = 5U;
static size_t nfooter = 5U;


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
sample_gen(int fd)
{
/* generic sampler */
	/* number of lines read so far */
	size_t nfln = 0U;
	/* number of header lines printed */
	size_t nhdr = 0U;
	/* ring buffer index */
	size_t nftr = 0U;
	/* fill of BUF, also used as offset of end-of-header in HDR mode */
	size_t nbuf = 0U;
	/* number of octets read per read() */
	ssize_t nrd;

	with (char *tmp = realloc(buf, BUFSIZ)) {
		if (UNLIKELY(tmp == NULL)) {
			/* just bugger off */
			return -1;
		}
		/* otherwise swap ptrs */
		buf = tmp;
		zbuf = BUFSIZ;
	}
	size_t last[nfooter + 1U];
	memset(last, 0, sizeof(last));
	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		const size_t nunbuf = nbuf + nrd;

		for (const char *x;
		     (x = memchr(buf + nbuf, '\n', nunbuf - nbuf)); nfln++) {
			const size_t obuf = nbuf;

			nbuf = ++x - buf;
			if (nhdr++ < nheader) {
				goto wrln;
			}
			/* keep track of footers */
			last[nftr] = obuf;
			nftr = (nftr + 1U) % countof(last);
			continue;

		wrln:
			fwrite(buf + obuf, sizeof(*buf), nbuf - obuf, stdout);
		}
		/* keep track of last footer */
		last[nftr] = nunbuf;
	}
	if (LIKELY(nfln > nheader + nfooter)) {
		fwrite("...\n", 1, 4U, stdout);
		nftr++;
	} else {
		nftr = 0U;
	}
	/* fast forward footer if there wasn't enough lines */
	for (size_t i = nftr, this, next;
	     i <= nftr + nfooter &&
		     (this = last[(i + 0U) % countof(last)],
		      next = last[(i + 1U) % countof(last)],
		      this < next); i++) {
		fwrite(buf + this, sizeof(*buf), next - this, stdout);
	}
	return 0;
}

#if 0
static int
sample_strm(int fd)
{
}

static int
sample_file(int fd)
{
}
#endif

static int
sample(const char *fn)
{
	struct stat st;
	int rc = 0;
	int fd;

	if (fn == NULL || fn[0U] == '-' && fn[1U] == '\0') {
		/* stdin ... *sigh* */
		fd = STDIN_FILENO;
		return sample_gen(STDIN_FILENO);
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
