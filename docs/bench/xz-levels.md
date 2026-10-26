# xz levels: dictionary, ratio, speed, memory (liblzma 5.8.4)

Measured 2026-10-24 on the first 128 MiB of `binaries.bin`, `text.bin`, `vmlike.bin` (the samples of tools/bench_full.py), x86-64 Linux, 32 cores
shared with other jobs (speeds are indicative; T8 runs of a 128 MiB slice have only 2-3 blocks at the high levels, so they cannot scale).
Tool: a small C driver over `lzma_stream_encoder_mt` (the same call as src/xz.cpp), ratio = input / output, MB/s of input, peak memory = max RSS minus the
in/out buffers. "preset" = the xz preset's dictionary (1/2/4/4/8/8/16/32/64 MiB), "ours" = the table of src/xz.cpp (1/2/4/8/16/16/32/64/64 MiB);
the block size is 3 x dictionary (cap 192 MiB) in both. Levels 1-3 are identical in both columns.

### binaries (first 128 MiB)

| level | dict preset -> ours | ratio preset (T1) | ratio ours (T1) | MB/s T1 preset | MB/s T1 ours | MB/s T8 preset | MB/s T8 ours | peak MiB T1 p/o | peak MiB T8 p/o |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 -> 1 MiB | 3.20 | 3.20 | 2.9 | 2.9 | 22.8 | 22.8 | 13/13 | 108/108 |
| 2 | 2 -> 2 MiB | 3.47 | 3.47 | 2.6 | 2.6 | 13.0 | 13.0 | 25/25 | 201/201 |
| 3 | 4 -> 4 MiB | 3.72 | 3.72 | 1.9 | 1.9 | 8.1 | 8.1 | 48/48 | 375/375 |
| 4 | 4 -> 8 MiB | 3.95 | 4.23 | 1.1 | 1.1 | 6.3 | 4.1 | 63/125 | 504/710 |
| 5 | 8 -> 16 MiB | 4.30 | 4.48 | 0.9 | 0.9 | 4.1 | 2.5 | 125/246 | 709/712 |
| 6 | 8 -> 16 MiB | 4.32 | 4.50 | 0.9 | 0.8 | 4.6 | 2.4 | 125/244 | 711/711 |
| 7 | 16 -> 32 MiB | 4.50 | 4.56 | 0.9 | 0.9 | 2.2 | 1.4 | 246/483 | 711/878 |
| 8 | 32 -> 64 MiB | 4.56 | 4.89 | 0.8 | 0.8 | 1.3 | 0.9 | 483/827 | 878/828 |
| 9 | 64 -> 64 MiB | 4.89 | 4.89 | 0.8 | 0.8 | 0.9 | 0.9 | 827/827 | 827/828 |

### text (first 128 MiB)

| level | dict preset -> ours | ratio preset (T1) | ratio ours (T1) | MB/s T1 preset | MB/s T1 ours | MB/s T8 preset | MB/s T8 ours | peak MiB T1 p/o | peak MiB T8 p/o |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 -> 1 MiB | 8.15 | 8.15 | 9.7 | 9.7 | 61.4 | 61.4 | 12/12 | 98/98 |
| 2 | 2 -> 2 MiB | 8.59 | 8.59 | 6.8 | 6.8 | 42.0 | 42.0 | 24/24 | 187/187 |
| 3 | 4 -> 4 MiB | 9.13 | 9.13 | 4.3 | 4.3 | 18.7 | 18.7 | 46/46 | 357/357 |
| 4 | 4 -> 8 MiB | 9.24 | 9.97 | 3.1 | 3.0 | 13.6 | 7.4 | 62/123 | 485/695 |
| 5 | 8 -> 16 MiB | 10.85 | 13.10 | 2.1 | 1.8 | 5.3 | 3.7 | 122/239 | 693/693 |
| 6 | 8 -> 16 MiB | 11.28 | 13.59 | 1.5 | 1.4 | 5.5 | 3.2 | 122/239 | 693/692 |
| 7 | 16 -> 32 MiB | 13.59 | 16.66 | 1.4 | 1.5 | 3.2 | 2.4 | 239/470 | 693/857 |
| 8 | 32 -> 64 MiB | 16.66 | 16.82 | 1.4 | 1.4 | 2.5 | 1.2 | 470/810 | 857/809 |
| 9 | 64 -> 64 MiB | 16.82 | 16.82 | 1.4 | 1.4 | 1.4 | 1.3 | 809/809 | 809/809 |

### vmlike (first 128 MiB)

| level | dict preset -> ours | ratio preset (T1) | ratio ours (T1) | MB/s T1 preset | MB/s T1 ours | MB/s T8 preset | MB/s T8 ours | peak MiB T1 p/o | peak MiB T8 p/o |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 -> 1 MiB | 5.12 | 5.12 | 7.3 | 7.3 | 44.4 | 44.4 | 13/13 | 105/105 |
| 2 | 2 -> 2 MiB | 5.26 | 5.26 | 6.3 | 6.3 | 35.3 | 35.3 | 25/25 | 194/194 |
| 3 | 4 -> 4 MiB | 5.61 | 5.61 | 4.8 | 4.8 | 24.5 | 24.5 | 47/47 | 364/364 |
| 4 | 4 -> 8 MiB | 5.79 | 6.13 | 3.3 | 3.0 | 15.4 | 9.6 | 64/123 | 493/703 |
| 5 | 8 -> 16 MiB | 6.24 | 6.34 | 2.9 | 2.3 | 8.5 | 4.7 | 123/244 | 702/703 |
| 6 | 8 -> 16 MiB | 6.27 | 6.37 | 2.3 | 2.0 | 8.9 | 4.2 | 124/243 | 702/704 |
| 7 | 16 -> 32 MiB | 6.37 | 6.49 | 1.9 | 1.8 | 3.8 | 2.1 | 243/481 | 703/869 |
| 8 | 32 -> 64 MiB | 6.49 | 6.66 | 1.8 | 1.6 | 2.1 | 1.6 | 480/820 | 870/820 |
| 9 | 64 -> 64 MiB | 6.66 | 6.66 | 1.6 | 1.7 | 1.6 | 1.6 | 820/821 | 821/820 |

## What the larger dictionary brings
Text +20% ratio at level 5 (10.85 -> 13.10), vmlike +1.6%, binaries +4%; the price is 2x memory per thread and 10-40% speed (a bigger match
finder and fewer cache hits). Levels 8 and 9 hold 64 MiB dictionaries: ~0.8 GiB per thread, so the automatic thread count drops to 2-3 there
(memory budget 2.5 GiB on x64; 700 MiB and a dictionary shrunk until one thread fits ~900 MiB on x86).
Splitting into independent blocks (needed for output that does not depend on the thread count) costs ratio against one block: level 5 / 16 MiB,
single block vs 48 MiB blocks: text 9.46 MB vs 10.25 MB (+8%), vmlike 21.11 vs 21.18 MB (+0.3%), binaries 29.87 vs 29.94 MB (+0.2%).

## 7-Zip at the same dictionary (single thread, `7z a -t7z -m0=lzma2:d=16m -mx=5 -mmt=1`)
| sample | 7-Zip bytes / s | liblzma single block, same dict (bytes / s) | ours (3 blocks) |
|---|---|---|---|
| text | 9,453,771 / 78 | 9,455,468 / 75 | 10,245,668 |
| vmlike | 21,095,611 / 57 | 21,111,584 / 54 | 21,183,480 |
| binaries | 29,858,061 / 122 | 29,869,892 / 115 | 29,944,540 |
The coder is the same: ratio and speed equal to 7-Zip within 0.1% / 5%; the only difference is our block splitting.

## The 7-Zip binaries.bin "anomaly" (ratio 6.08 at -mx7/-mx9 against xz -9 4.89)
Not a filter: `7z l -slt` shows `Method = LZMA2:27`, i.e. a **128 MiB dictionary** (no BCJ/ARM64 filter), and the same file with `d=64m -mx9` gives
27,405,924 bytes (ratio 4.90, = xz -9). liblzma with a 128 MiB dictionary and one block gives 22,119,032 bytes (6.07) against 7-Zip's 22,086,809: the
sample has repeats 64-128 MiB apart, so only a dictionary larger than 64 MiB (and no block split) sees them. Memory for that is 1.4 GiB, not an option.

## x86 BCJ filter, chosen per block (shipped)
Every block (3 x dictionary) decides for itself, from its own bytes only:
1. one scan counts "plausible" CALL / JMP candidates: byte E8 or E9, offset top byte 00 or FF, |offset| < block size. Below 15 per 10,000 bytes
   (0.15%): no filter (the scan runs at ~0.5-1 GB/s, 0.25 s for 128 MiB of text, 0.1 s for noise);
2. otherwise a trial: the 3 256 KiB windows with the most candidates are compressed with preset 1 with and without BCJ; the filter is used only
   if it saves more than 1.5% on that sample (~0.3 s per such block).
Measured densities per 10,000 bytes (48 MiB blocks): text 0, noise 0, binaries 53-158, vmlike 40-69, ELF (/usr/bin + .so) 57-165, Wine PE DLLs 13-20
(mostly resources/stubs: below the threshold or rejected by the trial, BCJ would lose 1-11% there).
Level 5, 8 threads, 128 MiB samples (size in bytes / seconds, wall, machine shared with other jobs):
| sample | plain LZMA2 | per-block BCJ | change |
|---|---|---|---|
| binaries | 29,944,540 / 49.0 | 28,502,240 / 49.4 | -4.8% |
| vmlike | 21,183,480 / 27.6 | 20,485,732 / 29.9 | -3.3% |
| ELF x86-64 (/usr/bin, /usr/lib .so) | 32,946,556 / 54.1 | 31,476,016 / 52.5 | -4.5% |
| text | 10,245,668 / 41.5 | 10,245,644 / 36.9 | 0 (never chosen) |
| Wine PE32 DLLs | 15,225,044 / 40.1 | 15,225,020 / 35.9 | 0 (rejected by the trial) |
| noise (100 MB) | 100,004,876 / 54.9 | 100,005,048 / 50.9 | +0.0002% |
ARM64 loses on all samples (+0.8..+1.9%) and is not used. A block with fewer than 64 KiB never gets the filter. Memory is unchanged (the
filter keeps no state worth counting). The stream stays a normal .xz: blocks with different filter chains; xz, 7-Zip and Python read it.

## LZMA SDK 23.01 (public domain C, LzmaLib, one thread, level 5, 16 MiB; level 7, 32 MiB)
| sample | SDK L5 d16 bytes / s | liblzma single block L5 d16 | SDK L7 d32 | liblzma L7 d32 |
|---|---|---|---|---|
| text | 9,451,902 / 77 | 9,455,468 / 75 | 7,978,517 / 114 | 8,057,980 / 85 (ours, 3 blocks) |
| vmlike | 21,191,836 / 51 | 21,111,584 / 54 | 20,402,133 / 72 | |
| binaries | 29,852,994 / 126 | 29,869,892 / 115 | - | |
Same ratio and speed as liblzma (within 1-2%, both directions): the SDK brings nothing; liblzma additionally has the multi-threaded block encoder and
the .xz container. Not integrated.
