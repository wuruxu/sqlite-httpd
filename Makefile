BINARY := sqlitehttpd
HEADERS := internal-debug.h sqlite-http-req-struct.h sqlite-json-info.h
CFLAGS += -Wall -Wextra -I./ -Iexternal-libs/include -flto
LDFLAGS += -Lexternal-libs/lib/ -lmicrohttpd -lsqlite3 -levent -ljson-c -lz

CC ?= gcc

all: ${BINARY}

${BINARY}: sqlite-http-req.c.o sqlite-json-info.c.o ${BINARY}.c.o 
	${CC} -o $@ ${CFLAGS} ${LDFLAGS} $^

%.c.o: %.c ${HEADERS}
	${CC} -c -o $@ ${CFLAGS} $<

clean:
	rm -f ${BINARY} *.c.o

fresh: clean all

.PHONY: all clean fresh