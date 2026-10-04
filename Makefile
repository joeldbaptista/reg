include config.mk

SRC = util.c
OBJ = $(SRC:.c=.o)

all: reg regas

.c.o:
	$(CC) -c $(CFLAGS) $<

reg: vm.o $(OBJ)
	$(CC) -o $@ vm.o $(OBJ) $(LDFLAGS)

regas: as.o $(OBJ)
	$(CC) -o $@ as.o $(OBJ) $(LDFLAGS)

vm.o: vm.c reg.h util.h arg.h
as.o: as.c reg.h util.h arg.h
util.o: util.c util.h

clean:
	rm -f reg regas *.o out *.bin

.PHONY: all clean
