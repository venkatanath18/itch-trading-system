# Data

Nasdaq publishes free full-day sample files of TotalView-ITCH 5.0 at
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/> (checked 1 Nov 2026). Each `.gz`
file has a `.md5sum` file next to it. The files aren't committed to git: they
are large, and they are Nasdaq's.

| File | Trading day | Size (gzipped) |
|---|---|---|
| `12302019.NASDAQ_ITCH50.gz` | 30 Dec 2019 | 3.5 GB |
| `07302019.NASDAQ_ITCH50.gz` | 30 Jul 2019 | 3.7 GB |
| `10302019.NASDAQ_ITCH50.gz` | 30 Oct 2019 | 3.9 GB |
| `08302019.NASDAQ_ITCH50.gz` | 30 Aug 2019 | 4.1 GB |
| `01302019.NASDAQ_ITCH50.gz` | 30 Jan 2019 | 4.8 GB |
| `03272019.NASDAQ_ITCH50.gz` | 27 Mar 2019 | 5.5 GB |
| `01302020.NASDAQ_ITCH50.gz` | 30 Jan 2020 | 5.6 GB |

**Which day.** Develop on `12302019`, the smallest (a quiet holiday-week day).
Take the final numbers on a busier day, such as `10302019`: 30 Oct 2019 was
an FOMC announcement day, so the strategy also meets a news event.

## Download and check

From the `p6-trading-system` folder:

```bash
python3 tools/download_itch.py 12302019
```

It downloads `data/raw/12302019.NASDAQ_ITCH50.gz` in 64 MB byte ranges over 8
connections (one connection to the Nasdaq server ran at about 0.25 MB/s on
1 Nov 2026; eight together reached about 19 MB/s), resumes if interrupted,
writes the file's MD5 to `data/raw/12302019.NASDAQ_ITCH50.gz.md5`, and
decompresses it to `data/itch/12302019.NASDAQ_ITCH50`. Standard library only.

**Checking the file.** The server lists a `.md5sum` file next to each day, but
on 1 Nov 2026 every one of them answered 404, so there was no published hash
to compare with. The script tries it anyway and compares if it can. What it
always checks is gzip's own CRC-32 and length, stored in the file, which
`gzip -d` verifies while decompressing: a corrupted download fails there. The
MD5 we computed is recorded in `results/day_summary.md` so another download
can be compared with it.

**Disk space.** Decompressed, a day is several times larger than the `.gz`:
30 Dec 2019 is 3.5 GB compressed and 8.25 GB decompressed. Check free space
first. The decompressed file is replayed, so decompression doesn't count in
the timings.

**Memory.** On a machine with less memory than the decompressed day (the 8 GB
M1 used here), the file can't stay in the page cache, and reading it through
`mmap` makes every page a page fault (see `results/parser.md`). The programs
read it in 256 MB chunks into one reused buffer instead.

## Format

- Each message is a 2-byte big-endian length followed by the message. The
  message's first byte is its type.
- The first message of the day is a System Event (type `S`). Use it as a quick
  check that you're reading the framing correctly.
- The field layouts are in the
  [ITCH 5.0 specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf).

## Layout

```
data/
├── README.md   this file, the only file here that git tracks
├── raw/        the .gz files and the MD5 we computed for each
├── itch/       decompressed files
└── derived/    written by the programs: sizing profile, parameters, exports for Python
```
