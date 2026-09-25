CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDLIBS  = -lm

HIST_BIN = test_histogram_offline
RING_BIN = test_ring_offline
TURN_BIN = test_turnaround_offline

all: $(HIST_BIN) $(RING_BIN) $(TURN_BIN)

$(HIST_BIN): histogram.c test_histogram_offline.c histogram.h
	$(CC) $(CFLAGS) -o $@ histogram.c test_histogram_offline.c $(LDLIBS)

$(RING_BIN): ring_spsc.c test_ring_offline.c ring_spsc.h
	$(CC) $(CFLAGS) -o $@ ring_spsc.c test_ring_offline.c $(LDLIBS)

$(TURN_BIN): turnaround.c test_turnaround_offline.c turnaround.h histogram.c histogram.h
	$(CC) $(CFLAGS) -o $@ turnaround.c test_turnaround_offline.c histogram.c $(LDLIBS)

run: $(HIST_BIN) $(RING_BIN) $(TURN_BIN)
	./$(HIST_BIN)
	./$(RING_BIN)
	./$(TURN_BIN)

clean:
	rm -f $(HIST_BIN) $(RING_BIN) $(TURN_BIN)

.PHONY: all run clean