# Fuzz + regression harness for the in-place bplist reader (src/bplist/bplist.c).
#   make repro     # portable regression run (any C compiler: gcc/clang/msvc)
#   make fuzz      # libFuzzer build (needs clang); then: ./bplist_fuzz -max_total_time=60 corpus/
HB  = ../../hid_bridge
BPL = $(HB)/src/bplist
CC  ?= cc
CFLAGS = -Wall -Wextra -g -I$(BPL)

repro: bplist_fuzz.c $(BPL)/bplist.c
	$(CC) $(CFLAGS) -DFUZZ_STANDALONE -o bplist_repro bplist_fuzz.c $(BPL)/bplist.c
	./bplist_repro

fuzz: bplist_fuzz.c $(BPL)/bplist.c
	clang -fsanitize=fuzzer,address,undefined $(CFLAGS) -o bplist_fuzz bplist_fuzz.c $(BPL)/bplist.c

clean:
	rm -f bplist_fuzz bplist_repro *.exe *.o

.PHONY: repro fuzz clean
