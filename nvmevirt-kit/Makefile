CFLAGS = -O2 -Wall -pthread -DHAVE_BLKZONED
replayer: replayer.c
	$(CC) $(CFLAGS) -o $@ $< -lrt -lm

.PHONY: test
test:
	@set -e; ulimit -c 0; test_bin=$$(mktemp /tmp/replayer-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -o "$$test_bin" test_replayer.c -lrt -lm \
	  -Wl,--wrap=aio_write,--wrap=aio_read,--wrap=aio_suspend,--wrap=aio_error,--wrap=aio_return \
	  -Wl,--wrap=ioctl,--wrap=pwrite,--wrap=pread; \
	"$$test_bin"; \
	if [ -n "$(TRACE)" ]; then "$$test_bin" --trace "$(TRACE)"; fi

clean:
	rm -f replayer
