# Tests in every configuration

Written by `tools/reproduce.sh`. Unit tests, the randomized book comparison, the system tests
on a synthetic day, the fuzz smoke test and (outside the sanitizer builds) the no-allocation test.

| Configuration | Result |
|---|---|
| debug | 100% tests passed out of 172 |
| release | 100% tests passed out of 172 |
| asan | 100% tests passed out of 171 |
| tsan | 100% tests passed out of 171 |
