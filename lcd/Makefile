CFLAGS ?= -O2 -Wall -Wextra
LDLIBS += -pthread

t1kfb: t1kfb.c pins.h
	$(CC) $(CFLAGS) -pthread -o $@ t1kfb.c $(LDLIBS)

install: t1kfb
	install -m 755 t1kfb /usr/local/bin/t1kfb
	install -m 644 t1kfb.service /etc/systemd/system/t1kfb.service
	systemctl daemon-reload

clean:
	rm -f t1kfb

.PHONY: install clean
