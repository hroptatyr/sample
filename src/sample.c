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
#include <math.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <assert.h>
#include "pcg_basic.h"
#include "nifty.h"

#if defined BUFSIZ
# undef BUFSIZ
#endif	/* BUFSIZ */
#define BUFSIZ	(65536U)

static size_t nheader = 5U;
static size_t nfooter = 5U;
static unsigned int rate = UINT32_MAX / 10U;
static size_t nfixed;


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

static inline unsigned int
runif32(void)
{
	return pcg32_random();
}

static unsigned int
rexp32(unsigned int n, unsigned int d)
{
	double u = (double)runif32() / (double)UINT32_MAX;
	double lambda = log((double)n / (double)d);
	return (unsigned int)(log1p(-u) / lambda);
}


/* buffer */
static char *buf;
static size_t zbuf;
/* reservoir */
static char *rsv;
static size_t zrsv;

static int
init_rng(uint64_t seed)
{
	uint64_t stid = 0ULL;

	if (!seed) {
		seed = time(NULL);
		seed <<= 20U;
		seed ^= getpid();
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
	/* 3 major states, HEAD BEEF/CAKE and TAIL */
	enum {
		EVAL,
		HEAD,
		BEEF,
		TAIL,
		CAKE,
	} state = EVAL;

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
		case EVAL:
			if (rate == UINT32_MAX) {
				/* oh they want everything printed */
				fwrite(buf, sizeof(*buf), nrd, stdout);
				nbuf = 0U;
				break;
			} else if (!nfooter && !nheader) {
				goto cake;
			} else if (!nheader) {
				goto tail;
			}
			/* otherwise let HEAD state decide */
			state = HEAD;
		case HEAD:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				const size_t o = ibuf;

				ibuf = ++x - buf;
				fwrite(buf + o, sizeof(*buf), ibuf - o, stdout);
				noln++;

				if (++nfln >= nheader) {
					if (UNLIKELY(!nfooter && !rate)) {
						/* that's it */
						return 0;
					} else if (!nfooter) {
						goto cake;
					}
					/* otherwise the most generic mode */
					goto tail;
				}
			}
			goto wrap;

		cake:
			fwrite("...\n", 1, 4U, stdout);
			state = CAKE;
		case CAKE:
			/* CAKE is the mode where we don't track tail lines */
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				const size_t o = ibuf;

				ibuf = ++x - buf;
				nfln++;

				/* sample */
				if (pcg32_random() < rate) {
					fwrite(buf + o, sizeof(*buf),
					       ibuf - o, stdout);
					noln++;
				}
			}
			goto wrap;

		wrap:
			if (UNLIKELY(!ibuf)) {
				/* great, try a resize */
				const size_t nuz = zbuf * 2U;
				char *tmp = realloc(buf, nuz);

				if (UNLIKELY(tmp == NULL)) {
					return -1;
				}
				/* otherwise assign and retry */
				buf = tmp;
				zbuf = nuz;
			} else if (LIKELY(ibuf < nbuf)) {
				memmove(buf, buf + ibuf, nbuf - ibuf);
			}
			nbuf -= ibuf;
			ibuf = 0U;
			break;

		tail:
			state = TAIL;
		case TAIL:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
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
				if (pcg32_random() < rate) {
					const size_t this = last[nftr];
					const size_t next = LAST(nftr + 1U);

					fwrite(buf + this, sizeof(*buf),
					       next - this, stdout);
					noln++;
				}
			}
			goto over;

		over:
			/* beef buffer overrun */
			with (const size_t nu = last[nftr]) {
				if (UNLIKELY(!nu)) {
					/* resize and retry */
					const size_t nuz = zbuf * 2U;
					char *tmp = realloc(buf, nuz);

					if (UNLIKELY(tmp == NULL)) {
						return -1;
					}
					/* otherwise assign and retry */
					buf = tmp;
					zbuf = nuz;
					break;
				}
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
sample_rsv(int fd)
{
/* generic sampler */
	/* number of lines read so far */
	size_t nfln = 0U;
	/* fill of BUF, also used as offset of end-of-header in HDR mode */
	size_t nbuf = 0U;
	/* index into BUF to the beginning of the last unprocessed line */
	size_t ibuf = 0U;
	/* skip up to this line */
	size_t gap = 0U;
	/* number of octets read per read() */
	ssize_t nrd;
	/* offsets to footer */
	size_t last[nfooter + 1U];
	/* reservoir lines */
	size_t lrsv[nfixed + 1U];
	/* 3 major states, HEAD BEEF/CAKE and TAIL */
	enum {
		EVAL,
		HEAD,
		BEEF,
		FILL,
		BEXP,
	} state = EVAL;

	with (char *tmp = realloc(buf, BUFSIZ)) {
		if (UNLIKELY(tmp == NULL)) {
			/* just bugger off */
			return -1;
		}
		/* otherwise swap ptrs */
		buf = tmp;
		zbuf = BUFSIZ;
	}
	with (char *tmp = realloc(rsv, BUFSIZ)) {
		if (UNLIKELY(tmp == NULL)) {
			/* just bugger off */
			return -1;
		}
		/* otherwise swap ptrs */
		rsv = tmp;
		zrsv = BUFSIZ;
	}
	/* clean up last and lrsv array */
	memset(last, 0, sizeof(last));
	memset(lrsv, 0, sizeof(lrsv));

	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		/* calc next round's NBUF already */
		nbuf += nrd;

		switch (state) {
		case EVAL:
			if (!nfixed && !nheader) {
				return 0;
			} else if (!nheader) {
				goto fill;
			}
			/* otherwise let HEAD state decide */
			state = HEAD;
		case HEAD:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				const size_t o = ibuf;

				ibuf = ++x - buf;
				fwrite(buf + o, sizeof(*buf), ibuf - o, stdout);

				if (++nfln >= nheader) {
					if (UNLIKELY(!nfixed)) {
						/* that's it */
						return 0;
					}
					/* otherwise the most generic mode */
					goto fill;
				}
			}
			goto wrap;

		wrap:
			if (UNLIKELY(!ibuf)) {
				/* great, try a resize */
				const size_t nuz = zbuf * 2U;
				char *tmp = realloc(buf, nuz);

				if (UNLIKELY(tmp == NULL)) {
					return -1;
				}
				/* otherwise assign and retry */
				buf = tmp;
				zbuf = nuz;
			} else if (LIKELY(ibuf < nbuf)) {
				memmove(buf, buf + ibuf, nbuf - ibuf);
				nbuf -= ibuf;
				ibuf = 0U;
			}
			break;

		fill:
			nfln = 0U;
			state = FILL;
		case FILL:
			for (const char *x;
			     nfln < nfixed &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     nfln++) {
				/* keep track of footers */
				lrsv[nfln] = ibuf;
				LAST(nfln) = ibuf;
				ibuf = ++x - buf;
			}
			for (const char *x;
			     nfln >= nfixed &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ) {
				LAST(nfln) = ibuf;
				ibuf = ++x - buf;

				if (++nfln >= nfixed + nfooter) {
					goto beef;
				}
			}
			goto over;

#define MEMZCPY(tgt, off, tsz, src, len)				\
			do {						\
				if ((len) > (tsz)) {			\
					size_t nuz = (tsz);		\
					char *tmp;			\
					while ((nuz *= 2U) < len);	\
					tmp = realloc((tgt), nuz);	\
					if (UNLIKELY(tmp == NULL)) {	\
						return -1;		\
					}				\
					/* otherwise assign */		\
					(tgt) = tmp;			\
					(tsz) = nuz;			\
				}					\
				memcpy((tgt) + (off), src, len);	\
			} while (0)

		beef:
			state = BEEF;
			/* take on the reservoir */
			MEMZCPY(rsv, 0U, zrsv, buf + lrsv[0U],
				LAST(nfln - nfooter) - lrsv[0U]);
			lrsv[nfixed] = LAST(nfln - nfooter) - lrsv[0U];
			for (size_t i = nfixed - 1U; i > 0; i--) {
				lrsv[i] -= lrsv[0U];
			}
			lrsv[0U] = 0U;

			/* we need one more sample step because the
			 * condition above that got us here goes one
			 * step further than it should, lest we print
			 * the ellipsis when there's exactly
			 * nheader + nfoooter lines in the buffer */
		case BEEF:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ibuf = x - buf + 1U, nfln++) {
				/* keep track of footers */
				LAST(nfln) = ibuf;

				/* keep with propability nfixed / nfln */
				if (pcg32_boundedrand(nfln) < nfixed) {
					/* drop a random sample from the tail */
					const size_t j =
						pcg32_boundedrand(nfixed);
					/* line length at J */
					const size_t z =
						lrsv[j + 1U] - lrsv[j + 0U];
					/* current line length */
					const size_t y =
						nfooter ?
						LAST(nfln - nfooter + 1U) -
						LAST(nfln - nfooter + 0U)
						: x - buf + 1U - ibuf;

					memmove(rsv + lrsv[j + 0U],
						rsv + lrsv[j + 1U],
						lrsv[nfixed] - lrsv[j + 1U]);

					for (size_t i = j + 1U;
					     i <= nfixed; i++) {
						lrsv[i - 1U] = lrsv[i] - z;
					}
					/* bang this line */
					MEMZCPY(rsv, lrsv[nfixed - 1U], zrsv,
						buf + LAST(nfln - nfooter), y);
					/* and memorise him */
					lrsv[nfixed] = lrsv[nfixed - 1U] + y;
				} else if (nfln > 4U * nfixed) {
					/* switch to gap sampling */
					goto bexp;
				}
			}
			goto over;

		bexp:
			gap = nfln + rexp32(nfln - nfixed, nfln);
			state = BEXP;
		case BEXP:
			for (const char *x;
			     nfln < gap &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ibuf = x - buf + 1U, nfln++) {
				/* every line could be our last, so keep
				 * track of them */
				/* keep track of footers */
				LAST(nfln) = ibuf;
			}
			for (const char *x;
			     nfln >= gap &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
				) {
				/* drop a random sample from the tail */
				const size_t j = pcg32_boundedrand(nfixed);
				/* line length at J */
				const size_t z = lrsv[j + 1U] - lrsv[j + 0U];

				/* keep track of footers */
				LAST(nfln) = ibuf;

				memmove(rsv + lrsv[j + 0U],
					rsv + lrsv[j + 1U],
					lrsv[nfixed] - lrsv[j + 1U]);

				for (size_t i = j + 1U;
				     i <= nfixed; i++) {
					lrsv[i - 1U] = lrsv[i] - z;
				}
				/* bang this line */
				with (const size_t y =
				      nfooter ?
				      LAST(nfln - nfooter + 1U) -
				      LAST(nfln - nfooter + 0U)
				      : x - buf + 1U - ibuf) {
					MEMZCPY(rsv, lrsv[nfixed - 1U], zrsv,
						buf + LAST(nfln - nfooter), y);
					/* and memorise him */
					lrsv[nfixed] = lrsv[nfixed - 1U] + y;
				}

				ibuf = x - buf + 1U;
				nfln++;
				goto bexp;
			}
			goto over;

		over:
			/* beef buffer overrun */
			with (const size_t nu = LAST(nfln)) {
				if (UNLIKELY(!nu)) {
					/* resize and retry */
					const size_t nuz = zbuf * 2U;
					char *tmp = realloc(buf, nuz);

					if (UNLIKELY(tmp == NULL)) {
						return -1;
					}
					/* otherwise assign and retry */
					buf = tmp;
					zbuf = nuz;
					break;
				} else if (nfln < nfixed + nfooter) {
					MEMZCPY(rsv, 0U, zrsv, buf, ibuf);
					break;
				}
				memmove(buf, buf + nu, nbuf - nu);
				for (size_t i = 0U; i < countof(last); i++) {
					last[i] -= nu;
				}
				nbuf -= nu;
				ibuf -= nu;
			}
			/* keep track of last footer */
			LAST(nfln) = ibuf;
			break;
		}
	}
	if (nfln > nfixed + nfooter) {
		const size_t z = lrsv[nfixed];
		const size_t beg = LAST(nfln - nfooter - 0U);
		const size_t end = LAST(nfln - nfooter - 1U);

		fwrite("...\n", 1, 4U, stdout);
		fwrite(rsv + lrsv[0U], sizeof(*rsv), z - lrsv[0U], stdout);
		fwrite("...\n", 1, 4U, stdout);
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	} else if (nfln > nfooter) {
		const size_t z = lrsv[nfln - nfooter];
		const size_t beg = LAST(nfln - nfooter - 0U);
		const size_t end = LAST(nfln - nfooter - 1U);

		fwrite(rsv + lrsv[0U], sizeof(*rsv), z - lrsv[0U], stdout);
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	} else {
		const size_t beg = last[0U];
		const size_t end = last[nfln];
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	}
	return 0;
}

static int
sample_rsv_0f(int fd)
{
/* generic sampler */
	/* number of lines read so far */
	size_t nfln = 0U;
	/* fill of BUF, also used as offset of end-of-header in HDR mode */
	size_t nbuf = 0U;
	/* index into BUF to the beginning of the last unprocessed line */
	size_t ibuf = 0U;
	/* skip up to this line */
	size_t gap = 0U;
	/* number of octets read per read() */
	ssize_t nrd;
	/* reservoir lines */
	size_t lrsv[nfixed + 1U];
	/* 3 major states, HEAD BEEF/CAKE and TAIL */
	enum {
		EVAL,
		HEAD,
		BEEF,
		FILL,
		BEXP,
	} state = EVAL;

	with (char *tmp = realloc(buf, BUFSIZ)) {
		if (UNLIKELY(tmp == NULL)) {
			/* just bugger off */
			return -1;
		}
		/* otherwise swap ptrs */
		buf = tmp;
		zbuf = BUFSIZ;
	}
	with (char *tmp = realloc(rsv, BUFSIZ)) {
		if (UNLIKELY(tmp == NULL)) {
			/* just bugger off */
			return -1;
		}
		/* otherwise swap ptrs */
		rsv = tmp;
		zrsv = BUFSIZ;
	}
	/* clean up lrsv array */
	memset(lrsv, 0, sizeof(lrsv));

	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		/* calc next round's NBUF already */
		nbuf += nrd;

		switch (state) {
		case EVAL:
			if (!nfixed && !nheader) {
				return 0;
			} else if (!nheader) {
				goto fill;
			}
			/* otherwise let HEAD state decide */
			state = HEAD;
		case HEAD:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				const size_t o = ibuf;

				ibuf = ++x - buf;
				fwrite(buf + o, sizeof(*buf), ibuf - o, stdout);

				if (++nfln >= nheader) {
					if (UNLIKELY(!nfixed)) {
						/* that's it */
						return 0;
					}
					/* otherwise the most generic mode */
					goto fill;
				}
			}
			goto wrap;

		wrap:
			if (UNLIKELY(!ibuf)) {
				/* great, try a resize */
				const size_t nuz = zbuf * 2U;
				char *tmp = realloc(buf, nuz);

				if (UNLIKELY(tmp == NULL)) {
					return -1;
				}
				/* otherwise assign and retry */
				buf = tmp;
				zbuf = nuz;
			} else if (LIKELY(ibuf < nbuf)) {
				memmove(buf, buf + ibuf, nbuf - ibuf);
				nbuf -= ibuf;
				ibuf = 0U;
			}
			break;

		fill:
			nfln = 0U;
			state = FILL;
		case FILL:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				/* keep track of footers */
				lrsv[nfln++] = ibuf;
				ibuf = ++x - buf;

				if (nfln >= nfixed) {
					goto beef;
				}
			}
			goto over;

#define MEMZCPY(tgt, off, tsz, src, len)				\
			do {						\
				if ((len) > (tsz)) {			\
					size_t nuz = (tsz);		\
					char *tmp;			\
					while ((nuz *= 2U) < len);	\
					tmp = realloc((tgt), nuz);	\
					if (UNLIKELY(tmp == NULL)) {	\
						return -1;		\
					}				\
					/* otherwise assign */		\
					(tgt) = tmp;			\
					(tsz) = nuz;			\
				}					\
				memcpy((tgt) + (off), src, len);	\
			} while (0)

		beef:
			state = BEEF;
			/* take on the reservoir */
			MEMZCPY(rsv, 0U, zrsv, buf + lrsv[0U], ibuf - lrsv[0U]);
			lrsv[nfixed] = ibuf - lrsv[0U];
			for (size_t i = nfixed - 1U; i > 0; i--) {
				lrsv[i] -= lrsv[0U];
			}
			lrsv[0U] = 0U;

			/* we need one more sample step because the
			 * condition above that got us here goes one
			 * step further than it should, lest we print
			 * the ellipsis when there's exactly
			 * nheader + nfoooter lines in the buffer */
		case BEEF:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ibuf = x - buf + 1U, nfln++) {
				/* keep with propability nfixed / nfln */
				if (pcg32_boundedrand(nfln) < nfixed) {
					/* drop a random sample from the tail */
					const size_t j =
						pcg32_boundedrand(nfixed);
					/* line length at J */
					const size_t z =
						lrsv[j + 1U] - lrsv[j + 0U];
					/* current line length */
					const size_t y = x - buf + 1U - ibuf;

					memmove(rsv + lrsv[j + 0U],
						rsv + lrsv[j + 1U],
						lrsv[nfixed] - lrsv[j + 1U]);

					for (size_t i = j + 1U;
					     i <= nfixed; i++) {
						lrsv[i - 1U] = lrsv[i] - z;
					}
					/* bang this line */
					MEMZCPY(rsv, lrsv[nfixed - 1U], zrsv,
						buf + ibuf, y);
					/* and memorise him */
					lrsv[nfixed] = lrsv[nfixed - 1U] + y;
				} else if (nfln > 4U * nfixed) {
					/* switch to gap sampling */
					goto bexp;
				}
			}
			goto over;

		bexp:
			gap = nfln + rexp32(nfln - nfixed, nfln);
			state = BEXP;
		case BEXP:
			for (const char *x;
			     nfln < gap &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ibuf = x - buf + 1U, nfln++);
			for (const char *x;
			     nfln >= gap &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
				) {
				/* drop a random sample from the tail */
				const size_t j = pcg32_boundedrand(nfixed);
				/* line length at J */
				const size_t z = lrsv[j + 1U] - lrsv[j + 0U];
				/* current line length */
				const size_t y = x - buf + 1U - ibuf;

				memmove(rsv + lrsv[j + 0U],
					rsv + lrsv[j + 1U],
					lrsv[nfixed] - lrsv[j + 1U]);

				for (size_t i = j + 1U;
				     i <= nfixed; i++) {
					lrsv[i - 1U] = lrsv[i] - z;
				}
				/* bang this line */
				MEMZCPY(rsv, lrsv[nfixed - 1U], zrsv,
					buf + ibuf, y);
				/* and memorise him */
				lrsv[nfixed] = lrsv[nfixed - 1U] + y;

				ibuf = x - buf + 1U;
				nfln++;
				goto bexp;
			}
			goto over;

		over:
			/* beef buffer overrun */
			if (UNLIKELY(!ibuf)) {
				/* resize and retry */
				const size_t nuz = zbuf * 2U;
				char *tmp = realloc(buf, nuz);

				if (UNLIKELY(tmp == NULL)) {
					return -1;
				}
				/* otherwise assign and retry */
				buf = tmp;
				zbuf = nuz;
				break;
			} else if (nfln <= nfixed) {
				break;
			}
			memmove(buf, buf + ibuf, nbuf - ibuf);
			nbuf -= ibuf;
			ibuf -= ibuf;
			break;
		}
	}
	if (nfln > nfixed) {
		const size_t z = lrsv[nfixed];

		fwrite("...\n", 1, 4U, stdout);
		fwrite(rsv + lrsv[0U], sizeof(*rsv), z - lrsv[0U], stdout);
		fwrite("...\n", 1, 4U, stdout);
	} else if (nfln == nfixed) {
		/* we ran 0 steps through beef */
		const size_t z = lrsv[nfixed];

		fwrite(rsv + lrsv[0U], sizeof(*rsv), z - lrsv[0U], stdout);
	} else {
		fwrite(buf + lrsv[0U], sizeof(*buf), ibuf - lrsv[0U], stdout);
	}
	return 0;
}

static int
sample(const char *fn)
{
	int(*sample)(int) = sample_gen;
	struct stat st;
	int rc = 0;
	int fd;

	if (nfixed) {
		if (!nfooter) {
			sample = sample_rsv_0f;
		} else {
			sample = sample_rsv;
		}
	}

	if (fn == NULL || fn[0U] == '-' && fn[1U] == '\0') {
		/* stdin ... *sigh* */
		fd = STDIN_FILENO;
		return sample(fd);
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
		rc = sample(fd);
	} else {
		rc = sample(fd);
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

	/* treat ttys specially */
	if (isatty(STDOUT_FILENO) && !argi->rate_arg) {
#if defined TIOCGWINSZ
		with (struct winsize ws) {
			if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
				/* proceed as usual */
				break;
			} else if (nheader + nfooter + 5 >= ws.ws_row) {
				/* not enough room */
				break;
			}
			/* otherwise switch to -N mode,
			 * leave some room for ellipsis, prompt and stuff */
			nfixed = ws.ws_row - (nheader + nfooter + 5);
		}
#endif	/* TIOCGWINSZ */
	}

	if (argi->rate_arg) {
		char *on;
		double x = strtod(argi->rate_arg, &on);

		if (x < 0.) {
			errno = 0, error("\
Error: sample rate must be non-negative");
			rc = 1;
			goto out;
		} else if (*on == '%' && x > 100.) {
			errno = 0, error("\
Error: sample rate in percent must be <=100");
			rc = 1;
			goto out;
		} else if (*on == '%') {
			x /= 100.;
		}

		if (x > 1.) {
			x = 1. / x;
		}

		rate = (unsigned int)((double)UINT32_MAX * x);
	}
	if (argi->fixed_arg) {
		char *on;
		nfixed = strtoul(argi->fixed_arg, &on, 0);
		if (!nfixed && !*on) {
			/* they want no randomness, just use zero rate */
			rate = 0U;
		} else if (!nfixed || *on) {
			errno = 0, error("\
Error: parameter to --fixed must be positive");
			rc = 1;
			goto out;
		}
	}

	with (uint64_t s = 0U) {
		if (argi->seed_arg) {
			char *on;
			s = strtoull(argi->seed_arg, &on, 0);
			if (!s || *on) {
				errno = 0, error("\
Error: seeds must be positive integers");
				rc = 1;
				goto out;
			}
		}

		/* initialise randomness */
		init_rng(s);
	}

	for (size_t i = 0U; i < argi->nargs + !argi->nargs; i++) {
		rc |= sample(argi->args[i]) < 0;
	}

	if (buf != NULL) {
		free(buf);
	}
	if (rsv != NULL) {
		free(rsv);
	}

out:
	yuck_free(argi);
	return rc;
}

/* sample.c ends here */
