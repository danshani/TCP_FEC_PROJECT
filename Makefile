CPPFLAGS = -I include -Wall -Werror -pthread \
           -Wno-error=address-of-packed-member \
           -Wno-error=stringop-overread \
           -Wno-error=address

src = $(wildcard src/*.c)
obj = $(patsubst src/%.c, build/%.o, $(src))
headers = $(wildcard include/*.h)
apps = apps/curl/curl

lvl-ip: $(obj)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(obj) -o lvl-ip
	@echo
	@echo "lvl-ip needs CAP_NET_ADMIN:"
	sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip

build/%.o: src/%.c ${headers}
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

debug: CFLAGS+= -DDEBUG_SOCKET -DDEBUG_TCP -g -fsanitize=address
debug: lvl-ip

debug-tsan: CFLAGS+= -DDEBUG_SOCKET -DDEBUG_TCP -g -fsanitize=thread
debug-tsan: lvl-ip

apps: $(apps)
	$(MAKE) -C tools
	$(MAKE) -C apps/curl
	$(MAKE) -C apps/curl-poll

all: lvl-ip apps

test: debug apps
	@echo
	@echo "Networking capabilites are required for test dependencies:"
	@command -v arping >/dev/null || { echo "Missing dependency: arping (install: sudo apt install iputils-arping)"; exit 1; }
	@command -v tc >/dev/null || { echo "Missing dependency: tc (install: sudo apt install iproute2)"; exit 1; }
	@sudo setcap cap_net_raw=ep "$$(command -v arping)"
	@sudo setcap cap_net_admin=ep "$$(command -v tc)"
	@echo
	cd tests && ./test-run-all

clean:
	rm -f build/*.o build/fec-selftest build/fec-codec-selftest build/handshake-selftest build/fec-receiver-selftest lvl-ip

fec-test: tests/fec-selftest.c src/fec.c include/fec.h
	# Build and run the standalone GF(256) polynomial FEC self-test.
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/fec-selftest.c src/fec.c -o build/fec-selftest
	./build/fec-selftest

fec-codec-test: tests/fec-codec-selftest.c src/fec_codec.c src/fec.c include/fec_codec.h include/fec.h
	# Build and run the FEC codec (encoder/decoder) self-test.
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/fec-codec-selftest.c src/fec_codec.c src/fec.c -o build/fec-codec-selftest
	./build/fec-codec-selftest

handshake-test: tests/handshake-wireformat-selftest.c include/fec_frame.h include/fec_codec.h include/fec.h
	# Build and run the FEC handshake wire-format self-test.
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/handshake-wireformat-selftest.c -o build/handshake-selftest
	./build/handshake-selftest

fec-receiver-test: tests/fec-receiver-selftest.c src/fec_codec.c src/fec.c include/fec_codec.h include/fec.h
	# Build and run the receiver-side recovery self-test.
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/fec-receiver-selftest.c src/fec_codec.c src/fec.c -o build/fec-receiver-selftest
	./build/fec-receiver-selftest
