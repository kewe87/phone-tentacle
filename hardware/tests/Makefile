# Host unit tests for the firmware's pure-C parts. Usage: make -C hardware/tests
SRC = ../hid_bridge/src
CFLAGS = -std=c11 -Wall -Wextra -g -fsanitize=address,undefined

# libsodium (apt libsodium-dev, brew libsodium) for the firmware signature.
# mbedtls 3.x (4.x dropped the ECP API): Homebrew's mbedtls@3, an older Homebrew mbedtls, or the system one (apt libmbedtls-dev)
BREW := $(shell brew --prefix 2>/dev/null)
MBEDTLS := $(firstword $(wildcard $(BREW)/opt/mbedtls@3 $(BREW)/opt/mbedtls) /usr)
SODIUM := $(firstword $(wildcard $(BREW)/opt/libsodium) /usr)

test: test_main.c $(SRC)/bplist/bplist.c $(SRC)/video/gop.c $(SRC)/airplay/keystream.c $(SRC)/cli/cli_proto.c $(SRC)/hid/pace.c $(SRC)/log/logring.c $(SRC)/airplay/mdns.c $(SRC)/airplay/audio.c test_setup.c $(SRC)/setup/setup_text.c $(SRC)/ui/confirm.c test_pair.c $(SRC)/cli/pair_gate.c test_ota.c $(SRC)/ota/ota_gate.c $(SRC)/ota/ota_sig.c test_kbd.c $(SRC)/hid/kbd_map.c test_web.c $(wildcard $(SRC)/web/*.c)
	$(CC) $(CFLAGS) -Istubs -I$(MBEDTLS)/include -I$(SODIUM)/include -o $@ $^ -L$(MBEDTLS)/lib -L$(SODIUM)/lib -lmbedx509 -lmbedcrypto -lsodium -lm
	./test

clean:
	rm -f test

.PHONY: clean
