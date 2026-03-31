PROG = royals_scores
SRCS = main.c
OBJS = $(SRCS:.c=.o)

LDLIBS  = -lcurl -lcjson -laxparameter -lpthread
CFLAGS += -Wall -Wextra -O2 -std=c11

.PHONY: all clean

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(PROG)
