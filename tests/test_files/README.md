# Assembling Klaus2m5's 6502 test suites

Test suites are available on Klaus2m5 repository [1]. Assembly files should be patched in order to generate the right binary images [2]:
```ASM
org zero_page
```
should be replaced by
```ASM
org 0
ds zero_page
```
and then assembled
```sh
$ ./as65  -l -m -w -h0 -v  6502_65C02_functional_tests/6502_interrupt_test.a65
```
For furher information on the suites read their headers.

[1] https://github.com/Klaus2m5/6502_65C02_functional_tests

[2] https://github.com/Klaus2m5/6502_65C02_functional_tests/issues/11
