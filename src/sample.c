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
#include <sys/resource.h>
#include <assert.h>
#include "pcg_basic.h"
#include "nifty.h"

#if defined BUFSIZ
# undef BUFSIZ
#endif	/* BUFSIZ */
#define BUFSIZ	(65536U)

static size_t nheader = 5U;
static size_t nfooter = 5U;
static long long unsigned int rate = UINT32_MAX / 10U;
static size_t nfixed;
/* limit for VLAs */
static size_t stklmt;


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
	double u = (double)runif32() / 0x1.p32;
	double lambda = log((double)n / (double)d);
	return (unsigned int)(log1p(-u) / lambda);
}

static inline __attribute__((pure, const)) size_t
min_z(size_t z1, size_t z2)
{
	return z1 <= z2 ? z1 : z2;
}


/* buffer */
static char *buf;
static size_t zbuf;
/* reservoir */
static char *rsv;
static size_t zrsv;
/* compactify */
static uint8_t *comp;
static size_t *idir;

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

static void
compactify(size_t *restrict off, const size_t m, const size_t n)
{
/* helper for reservoir sampling
 * compact M lines into N whose offsets are in OFF */
	size_t o = 0U;

	if (UNLIKELY(comp == NULL)) {
		comp = malloc(m * sizeof(*comp));
		if (UNLIKELY(comp == NULL)) {
			return;
		}
		idir = malloc(n * sizeof(*idir));
		if (UNLIKELY(idir == NULL)) {
			free(comp);
			comp = NULL;
			return;
		}
	}

	/* prep compactifier, this is a radix sort */
	memset(comp, 0, m * sizeof(*comp));
	/* prep IDIR buffer */
	for (size_t i = 0U; i < n; i++) {
		idir[i] = i;
	}
	for (size_t i = n; i < m; i++) {
		idir[pcg32_boundedrand(n)] = i;
	}
	/* now sort him */
	for (size_t i = 0U; i < n; i++) {
		comp[idir[i]] = 1U;
	}

	/* ... now move them lines
	 * we calculate streaks of lines and move them in bulk */
	for (size_t i = 0U, beg = 0U, end; beg < m; beg = end + 1U) {
		size_t len;

		/* find first line to move, COMP[] is guaranteed to have
		 * N matches */
		for (; !comp[beg]; beg++);
		for (end = beg + 1U; end < m && comp[end]; end++);

		memmove(rsv + o, rsv + off[beg], len = off[end] - off[beg]);

		/* resolve into lines again */
		for (size_t j = beg, bof = off[beg]; j < end; j++) {
			off[i++] = o + off[j] - bof;
		}
		o += len;
	}
	off[n] = o;
	return;
}

static int
sample_0(int fd)
{
	(void)fd;
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
	/* fill of BUF, also used as offset of end-of-header in HDR mode */
	size_t nbuf = 0U;
	/* index into BUF to the beginning of the last unprocessed line */
	size_t ibuf = 0U;
	/* number of octets read per read() */
	ssize_t nrd;
	/* offsets to footer */
	size_t _last[stklmt];
	size_t *last = _last;
#define LAST(x)		last[(x) % (nfooter + 1U)]
#define FIRST(x)	((x) > nfooter ? LAST(x) : 0U)
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

	if (nfooter >= countof(last)) {
		/* better do it with heap space */
		last = malloc((nfooter + 1U) * sizeof(*last));
	}

	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		/* calc next round's NBUF already */
		nbuf += nrd;

		switch (state) {
		case EVAL:
			if (rate > UINT32_MAX) {
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
				if (runif32() < rate) {
					fwrite(buf + o, sizeof(*buf),
					       ibuf - o, stdout);
					noln++;
				}
			}
			goto wrap;

		wrap:
			if (LIKELY(nbuf < zbuf / 2U)) {
				/* we've got enough buffer, use, him */
				break;
			} else if (UNLIKELY(!ibuf)) {
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
			nfln = 0U;
		case TAIL:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));) {
				/* keep track of footers */
				LAST(nfln) = ibuf;
				ibuf = ++x - buf;

				if (++nfln > nfooter && rate) {
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
				LAST(nfln) = ibuf;
				ibuf = ++x - buf;

				nfln++;

			sample:
				/* sample */
				if (runif32() < rate) {
					const size_t this = LAST(nfln + 0U);
					const size_t next = LAST(nfln + 1U);

					fwrite(buf + this, sizeof(*buf),
					       next - this, stdout);
					noln++;
				}
			}
			goto over;

		over:
			/* beef buffer overrun handling */
			with (const size_t frst = FIRST(nfln)) {
				if (LIKELY(nbuf < zbuf / 2U)) {
					/* just read more stuff */
					break;
				} else if (UNLIKELY(!frst || frst == ibuf)) {
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
				memmove(buf, buf + frst, nbuf - frst);
				for (size_t i = 0U,
					     n = min_z(nfooter + 1U, nfln);
				     i < n; i++) {
					last[i] -= frst;
				}
				nbuf -= frst;
				ibuf -= frst;
			}
			/* keep track of last footer */
			LAST(nfln) = ibuf;
			break;
		}
	}
	if (noln > nheader ||
	    !rate && nfln > nfooter) {
		fwrite("...\n", 1, 4U, stdout);
	}
	/* fast forward footer if there wasn't enough lines */
	if (nfln > nfooter) {
		const size_t beg = LAST(nfln - nfooter - 0U);
		const size_t end = LAST(nfln - nfooter - 1U);
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	} else if (nfln) {
		const size_t beg = last[0U];
		const size_t end = last[nfln];
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	}		
	if (last != _last) {
		free(last);
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
	/* nfixed buffer oversampling */
	size_t nfxd = 0U;
	/* number of octets read per read() */
	ssize_t nrd;
	/* offsets to footer */
	size_t _last[stklmt / 3U];
	size_t *last = _last;
	/* reservoir lines
	 * we store offsets into RSV buffer in LRSV, which is 3 times the
	 * size of NFIXED to concentrate the memmove()ing
	 * the kill count for each cell is stored in LGAP */
	size_t _lrsv[stklmt / 3U];
	size_t *lrsv = _lrsv;
	const size_t mult = 4U;
	/* 3 major states, HEAD BEEF/CAKE and TAIL */
	enum {
		EVAL,
		HEAD,
		BEEF,
		FILL,
		BEXP,
	} state = EVAL;

#define MEMZCPY(tgt, off, tsz, src, len)				\
			do {						\
				const size_t nul = (off) + (len);	\
				if (nul > (tsz)) {			\
					size_t nuz = (tsz);		\
					char *tmp;			\
					while ((nuz *= 2U) < nul);	\
					tmp = realloc((tgt), nuz);	\
					if (UNLIKELY(tmp == NULL)) {	\
						return -1;		\
					}				\
					/* otherwise assign */		\
					(tgt) = tmp;			\
					(tsz) = nuz;			\
				}					\
				memcpy((tgt) + (off), (src), (len));	\
			} while (0)

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
	if (nfooter >= countof(_last)) {
		/* do him with heap space */
		last = malloc((nfooter + 1U) * sizeof(*last));
	}
	if (mult * nfixed >= countof(_lrsv)) {
		/* do her with heap space */
		lrsv = malloc((mult * nfixed + 1U) * sizeof(*lrsv));
	}

	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		/* calc next round's NBUF already */
		nbuf += nrd;

		switch (state) {
		case EVAL:
			if (!nheader) {
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
			if (LIKELY(nbuf < zbuf / 2U)) {
				/* no need for buffer juggling */
				break;
			} else 	if (UNLIKELY(!ibuf)) {
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

		beef:
			state = BEEF;
			/* take on the reservoir */
			MEMZCPY(rsv, 0U, zrsv, buf + lrsv[0U],
				LAST(nfln - nfooter) - lrsv[0U]);
			lrsv[nfixed] = LAST(nfln - nfooter);
			for (size_t i = nfixed + 1U; i > 0; i--) {
				lrsv[i - 1U] -= lrsv[0U];
			}

			nfxd = nfixed;

			/* we need one more sample step because the
			 * condition above that got us here goes one
			 * step further than it should, lest we print
			 * the ellipsis when there's exactly
			 * nheader + nfoooter lines in the buffer */
		case BEEF:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ibuf = x - buf + 1U, nfln++) {
				/* current line length */
				const size_t y =
					LAST(nfln - nfooter + 1U) -
					LAST(nfln - nfooter + 0U);

				/* keep track of footers */
				LAST(nfln) = ibuf;

				if (nfln >= 4U * nfixed) {
					/* switch to gap sampling */
					goto bexp;
				}

				/* keep with probability nfixed / nfln */
				if (pcg32_boundedrand(nfln) >= nfixed) {
					continue;
				}

				if (UNLIKELY(nfxd >= mult * nfixed)) {
					/* condense lrsv */
					compactify(lrsv, nfxd, nfixed);
					nfxd = nfixed;
				}

				/* bang this line */
				MEMZCPY(rsv, lrsv[nfxd], zrsv,
					buf + LAST(nfln - nfooter), y);
				/* and memorise him */
				lrsv[nfxd + 1U] = lrsv[nfxd] + y;
				nfxd++;
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
				LAST(nfln) = ibuf;
			}
			for (const char *x;
			     nfln >= gap &&
				     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
				) {
				/* current line length */
				const size_t y =
					LAST(nfln - nfooter + 1U) -
					LAST(nfln - nfooter + 0U);

				/* keep track of footers */
				LAST(nfln) = ibuf;

				if (UNLIKELY(nfxd >= mult * nfixed)) {
					/* condense lrsv */
					compactify(lrsv, nfxd, nfixed);
					nfxd = nfixed;
				}

				/* bang this line */
				MEMZCPY(rsv, lrsv[nfxd], zrsv,
					buf +  LAST(nfln - nfooter), y);
				/* and memorise him */
				lrsv[nfxd + 1U] = lrsv[nfxd] + y;
				nfxd++;

				ibuf = x - buf + 1U;
				nfln++;
				goto bexp;
			}
			goto over;

		over:
			/* beef buffer overrun */
			with (const size_t frst = FIRST(nfln)) {
				if (LIKELY(nbuf < zbuf / 2U)) {
					/* just read more stuff */
					break;
				} else if (UNLIKELY(!frst || frst == ibuf ||
						    nfln <= nfixed + nfooter)) {
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
				memmove(buf, buf + frst, nbuf - frst);
				for (size_t i = 0U,
					     n = min_z(nfooter + 1U, nfln);
				     i < n; i++) {
					last[i] -= frst;
				}
				nbuf -= frst;
				ibuf -= frst;
			}
			/* keep track of last footer */
			LAST(nfln) = ibuf;
			break;
		}
	}
	if (nfln >= nfixed + nfooter) {
		/* compactify to obtain the final result */
		compactify(lrsv, nfxd, nfixed);

		const size_t z = lrsv[nfixed];
		const size_t beg = LAST(nfln - nfooter - 0U);
		const size_t end = LAST(nfln - nfooter - 1U);

		if (nfln > nfixed + nfooter) {
			fwrite("...\n", 1, 4U, stdout);
		}
		fwrite(rsv + lrsv[0U], sizeof(*rsv), z - lrsv[0U], stdout);
		if (nfln > nfixed + nfooter) {
			fwrite("...\n", 1, 4U, stdout);
		}
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	} else if (nfln > nfooter) {
		const size_t beg = lrsv[0U];
		const size_t end = LAST(nfln - nfooter - 1U);
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	} else if (nfln) {
		const size_t beg = last[0U];
		const size_t end = last[nfln];
		fwrite(buf + beg, sizeof(*buf), end - beg, stdout);
	}
	if (last != _last) {
		free(last);
	}
	if (lrsv != _lrsv) {
		free(lrsv);
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
	/* nfixed buffer oversampling */
	size_t nfxd = 0U;
	/* number of octets read per read() */
	ssize_t nrd;
	/* reservoir lines
	 * we store offsets into RSV buffer in LRSV, which is 3 times the
	 * size of NFIXED to concentrate the memmove()ing */
	size_t _lrsv[stklmt / 2U];
	size_t *lrsv = _lrsv;
	const size_t mult = 4U;
	/* major states */
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
	if (mult * nfixed >= countof(_lrsv)) {
		/* do her with heap space */
		lrsv = malloc((mult * nfixed + 1U) * sizeof(*lrsv));
	}

	/* deal with header */
	while ((nrd = read(fd, buf + nbuf, zbuf - nbuf)) > 0) {
		/* calc next round's NBUF already */
		nbuf += nrd;

		switch (state) {
		case EVAL:
			if (!nheader) {
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
			if (LIKELY(nbuf < zbuf / 2U)) {
				/* just read some more */
				break;
			} else if (UNLIKELY(!ibuf)) {
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
				/* keep track of lines */
				lrsv[nfln] = ibuf;
				nfln++;
				ibuf = ++x - buf;

				if (nfln >= nfixed) {
					goto beef;
				}
			}
			goto over;

		beef:
			state = BEEF;
			/* take on the reservoir */
			MEMZCPY(rsv, 0U, zrsv, buf + lrsv[0U], ibuf - lrsv[0U]);
			lrsv[nfixed] = ibuf;
			for (size_t i = nfixed + 1U; i > 0; i--) {
				lrsv[i - 1U] -= lrsv[0U];
			}

			nfxd = nfixed;

			/* we need one more sample step because the
			 * condition above that got us here goes one
			 * step further than it should, lest we print
			 * the ellipsis when there's exactly
			 * nheader + nfoooter lines in the buffer */
		case BEEF:
			for (const char *x;
			     (x = memchr(buf + ibuf, '\n', nbuf - ibuf));
			     ibuf = x - buf + 1U, nfln++) {
				/* current line length */
				const size_t y = x - buf + 1U - ibuf;

				if (nfln >= 4U * nfixed) {
					/* switch to gap sampling */
					goto bexp;
				}

				/* keep with probability nfixed / nfln */
				if (pcg32_boundedrand(nfln) >= nfixed) {
					continue;
				}

				if (UNLIKELY(nfxd >= mult * nfixed)) {
					/* condense lrsv */
					compactify(lrsv, nfxd, nfixed);
					nfxd = nfixed;
				}

				/* bang this line */
				MEMZCPY(rsv, lrsv[nfxd], zrsv, buf + ibuf, y);
				/* and memorise him */
				lrsv[nfxd + 1U] = lrsv[nfxd] + y;
				nfxd++;
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
				/* current line length */
				const size_t y = x - buf + 1U - ibuf;

				if (UNLIKELY(nfxd >= mult * nfixed)) {
					/* condense lrsv */
					compactify(lrsv, nfxd, nfixed);
					nfxd = nfixed;
				}

				/* bang this line */
				MEMZCPY(rsv, lrsv[nfxd], zrsv, buf + ibuf, y);
				/* and memorise him */
				lrsv[nfxd + 1U] = lrsv[nfxd] + y;
				nfxd++;

				ibuf = x - buf + 1U;
				nfln++;
				goto bexp;
			}
			goto over;

		over:
			/* beef buffer overrun */
			if (LIKELY(nbuf < zbuf / 2U)) {
				/* we'll risk reading some more */
				break;
			} else if (UNLIKELY(!ibuf || nfln <= nfixed)) {
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
			memmove(buf, buf + ibuf, nbuf - ibuf);
			nbuf -= ibuf;
			ibuf -= ibuf;
			break;
		}
	}
	if (nfln > nfixed) {
		/* compactify to obtain the final result */
		compactify(lrsv, nfxd, nfixed);

		fwrite("...\n", 1, 4U, stdout);
		fwrite(rsv + lrsv[0U], sizeof(*rsv),
		       lrsv[nfixed] - lrsv[0U], stdout);
		fwrite("...\n", 1, 4U, stdout);
	} else if (nfln == nfixed) {
		/* we ran 0 steps through beef */
		const size_t z = lrsv[nfixed];

		fwrite(rsv + lrsv[0U], sizeof(*rsv), z - lrsv[0U], stdout);
	} else if (ibuf > lrsv[0U]) {
		fwrite(buf + lrsv[0U], sizeof(*buf), ibuf - lrsv[0U], stdout);
	}
	if (lrsv != _lrsv) {
		free(lrsv);
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
	} else if (!rate && !nfooter && !nheader) {
		sample = sample_0;
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

		rate = (long long unsigned int)(0x1.p32 * x);
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

	/* obtain stack limits */
	with (struct rlimit lmt) {
		if (getrlimit(RLIMIT_STACK, &lmt) < 0) {
			/* yeah right */
			break;
		}
		stklmt = lmt.rlim_cur / sizeof(stklmt) / 2U;
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
	if (comp != NULL) {
		free(comp);
	}
	if (idir != NULL) {
		free(idir);
	}

out:
	yuck_free(argi);
	return rc;
}

/* sample.c ends here */
