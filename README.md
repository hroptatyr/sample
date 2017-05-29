sample
======

Produce a sample of lines from files.  The sample size is either fixed
or proportional to the size of the file.  Additionally, the header and
footer can be included in the sample.


Red tape
--------

- no dependencies other than a POSIX system and a C99 compiler.
- licensed under [BSD3c][1]


Features
--------

- proportional sampling of streams and files
- header and footer can be included in the sample
- reservoir sampling (fixed sample size) of streams and files
- reservoir sampling preserves order


Motivation
----------

Tools like [paulgb's subsample][2] or [earino's fast_sample][3]
usually do the trick and everyone seems to agree (judged by github
stars).  However, both tools have two short-comings: they try to make
sense of the line data semantically, and secondly, they use their
language's built-in routines to produce random numbers.

The first issue is such a major problem that their bug trackers are
full of reports.  `subsample' needs lines to be UTF-8 strings and
`fast_sample` wants CSV files whose correctness is checked along the
way.  `sample` on the other hand does not care about the line's
content, all it needs are those line breaks at the end.

The second issue is far more pressing when line sampling becomes the
bottleneck in your workflow.  Throwing enough I/O power at the tools
and they show their true slowness: `subsample` can go through just
over 1M lines per second and `fast_sample` doubles that.  `sample`
uses the very fast [PCG family][4] to obtain its randomness.  This
(and the lack of line processing) catapults performance into the range
of 60+M lines per second.


Examples
--------

To get 10 random words from the `words` file:

    $ sample -N 10 -H 0 /usr/share/dict/words
    ...
    benzopyrene
    calamondins
    cephalothorax
    copulate
    garbology's
    Kewadin
    Peter's
    reassembly
    Vienna's
    Wagnerism's
    ...

The `-H 0` produces 0 lines of header output which defaults to 5.

For proportional sampling use `-R|--rate`:

    $ wc -l /usr/share/dict/words
    305089
    $ sample -R 1% /usr/share/dict/words | wc -l
    3080

which is close to the true result bearing in mind that by default the
header and footer of the file is printed as well.

Sampling with a rate of 0 replaces awkward scripts that use multios
and `head` and `tail` to produce the same result.

    $ sample -R 0 /usr/share/dict/words
    A
    AA
    AAA
    Aachen
    aah
    ...
    Zyuganov
    Zyuganov's
    zyzzyva
    zyzzyvas
    ZZZ


  [1]: http://opensource.org/licenses/BSD-3-Clause
  [2]: https://github.com/paulgb/subsample
  [3]: https://github.com/earino/fast_sample
  [4]: http://www.pcg-random.org/
