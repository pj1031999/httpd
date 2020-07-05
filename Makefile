CC		:= gcc

CPPFLAGS	:= -Wall -Wextra -Werror -DNDEBUG
CFLAGS		:= -MMD -std=gnu18 -march=native -O3 -fomit-frame-pointer -pipe

LDFLAGS		:= -Wl,-O3 -Wl,--as-needed -pthread -s
SRCS		:= httpd.c
OBJS		:= $(subst .c,.o,$(SRCS))
DEPS		:= $(subst .o,.d,$(OBJS))

PROG		:= httpd

all: $(PROG)

$(PROG): $(OBJS)

.PHONY: clean
clean:
	$(RM) $(OBJS) $(DEPS) $(PROG)

-include $(DEPS)

# vim: tabstop=8 shiftwidth=8 noexpandtab:
