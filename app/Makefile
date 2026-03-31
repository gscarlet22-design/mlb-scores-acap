PROG = mlb_scores
SRCS = main.c cJSON.c
OBJS = $(SRCS:.c=.o)

SYSROOT  = /opt/axis/acapsdk/sysroots/aarch64
GLIB_INC = $(SYSROOT)/usr/include/glib-2.0 \
           $(SYSROOT)/usr/lib/glib-2.0/include \
           $(SYSROOT)/usr/include

LDLIBS  = -lcurl -laxparameter -lpthread
CFLAGS += -Wall -Wextra -O2 -std=c11 -I. \
          $(addprefix -I,$(GLIB_INC))

.PHONY: all clean

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(PROG)
