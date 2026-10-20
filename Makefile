CC ?= gcc
CFLAGS += -O3 -Wall -Wextra -std=c11 -I/home/linuxbrew/.linuxbrew/include -I/usr/include
LDFLAGS ?= -L/usr/lib64 -L/usr/lib -l:libvulkan.so.1
PREFIX ?= /usr/local

all: vkcc vkc++ vkgcc vkg++

vkcc: vkcc.c vk_shader_spv.h
	$(CC) $(CFLAGS) vkcc.c $(LDFLAGS) -o vkcc

vkc++: vkcc
	ln -sf vkcc vkc++

vkgcc: vkcc
	ln -sf vkcc vkgcc

vkg++: vkcc
	ln -sf vkcc vkg++

check: all vkcc_check
	./vkcc_check

vkcc_check: vkcc_check.c vkcc
	$(CC) -O2 -Wall vkcc_check.c -o vkcc_check

test: all
	./test_suite.sh

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 vkcc $(DESTDIR)$(PREFIX)/bin/
	ln -sf vkcc $(DESTDIR)$(PREFIX)/bin/vkc++
	ln -sf vkcc $(DESTDIR)$(PREFIX)/bin/vkgcc
	ln -sf vkcc $(DESTDIR)$(PREFIX)/bin/vkg++

clean:
	rm -f vkcc vkc++ vkgcc vkg++ vkcc_check /tmp/vkcc_*
