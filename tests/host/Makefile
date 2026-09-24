# Host unit tests for the hardware-independent MasterController core.
#   make -C tests/host        build and run every test
# Needs any C11 gcc (MinGW on Windows is fine).

ifeq ($(origin CC),default)
CC      := gcc
endif
SRC     := ../../Core/Src
INC     := ../../Core/Inc
CFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion \
           -Wno-sign-conversion -fno-strict-aliasing -I$(INC) -I.
EXE     := $(if $(filter Windows_NT,$(OS)),.exe,)

CORE    := $(SRC)/mc_txq.c $(SRC)/mc_line.c $(SRC)/mc_router.c
TESTS   := test_core test_app

.PHONY: all run clean
all: run

test_core$(EXE): test_core.c $(CORE) mc_test.h
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORE)

test_app$(EXE): test_app.c fake_platform.c fake_platform.h $(CORE) $(SRC)/mc_app.c mc_test.h
	$(CC) $(CFLAGS) -o $@ test_app.c fake_platform.c $(CORE) $(SRC)/mc_app.c

run: $(addsuffix $(EXE),$(TESTS))
	./test_core$(EXE)
	./test_app$(EXE)

clean:
	rm -f $(addsuffix $(EXE),$(TESTS))
