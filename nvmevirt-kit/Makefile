CFLAGS = -O2 -Wall -pthread -DHAVE_BLKZONED
replayer: replayer.c
	$(CC) $(CFLAGS) -o $@ $< -lrt -lm
clean:
	rm -f replayer
