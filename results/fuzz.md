# Stage 2: fuzzing the parser

Written by `tools/reproduce.sh`. The standalone mutation fuzzer (`tests/fuzz/standalone_main.cpp`)
ran 20000000 inputs mutated from the seeds in `tests/fuzz/seeds` (bit flips, random bytes,
truncation, broken length prefixes, inserted bytes, spliced seeds) through the parser, built with
AddressSanitizer and UndefinedBehaviorSanitizer, each input in a buffer of exactly its size.
No crash, no read past the end, no undefined behaviour, in 27 s.

CI also runs the libFuzzer build (`-DBUILD_FUZZER=ON`, clang on Linux) for two minutes on every push.
