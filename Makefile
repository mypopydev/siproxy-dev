SRCS := main.c rs232.c telnet.c match.c queue.c mqtt.c
PROG := siproxy
CC   := gcc
CURDIR := $(shell pwd)

LIBS = paho-mqtt3a
LINKFLAGS = $(addprefix -l,$(LIBS))
LIBPATH = $(CURDIR)/paho-c/build/output/
LIBINCLUDE = $(CURDIR)/paho-c/src/
LIBPATHS = $(patsubst %,$(LIBPATH)/lib%.so, $(LIBS))

CFLAGS := -g -O0 -Wall -I$(LIBINCLUDE)
LDFLAGS:= -lrt -lpthread
OBJS   := $(SRCS:.c=.o)


$(PROG): $(OBJS)
	@$(MAKE) -C paho-c
	$(CC) $(CFLAGS) -I$(LIBINCLUDE) $(LDFLAGS) $(LINKFLAGS) -L$(LIBPATH) -o $@ $^

-include $(SRCS:.c=.d)

$.d:%.c
	@$(CC) -MM -I$(LIBINCLUDE) $(CFLAGS) $< | sed 's#\(.*\)\.o:#\1.o\1\1.d:#g' > $@


.PHONY: all
all: $(PROG)

clean:
	@$(MAKE) -C paho-c clean
	@rm -rf $(PROG) *.d *.o

