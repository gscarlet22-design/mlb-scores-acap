PROG = mlb_scores
SRCS = main.c cJSON.c
OBJS = $(SRCS:.c=.o)

LDLIBS  = -lcurl -laxparameter -lpthread
CFLAGS += -Wall -Wextra -O2 -std=c11 -I.

.PHONY: all clean

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(PROG)
