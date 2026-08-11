CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -g

LLVM_CONFIG ?= llvm-config

ifeq ($(OS),Windows_NT)
    EXE = .exe
else
    EXE =
endif

LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags)
LLVM_LIBS   := $(shell $(LLVM_CONFIG) --libs core native analysis target)
LLVM_LDFLAGS:= $(shell $(LLVM_CONFIG) --ldflags --system-libs)

SRCS = Lexer.c Parser.c Sema.c CodeGen.c main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test

all: minic$(EXE)

minic$(EXE): $(OBJS)
	$(CC) $(OBJS) $(LLVM_LDFLAGS) $(LLVM_LIBS) -o minic$(EXE)

%.o: %.c
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

CodeGen.o: CodeGen.c CodeGen.h Parser.h
main.o: main.c CodeGen.h Lexer.h Parser.h Sema.h

test: Test/lexer_test$(EXE)
	./Test/lexer_test$(EXE)

Test/lexer_test$(EXE): Test/lexer_test.cpp Lexer.c Lexer.h
	g++ -Wall -Wextra -std=c++17 Test/lexer_test.cpp Lexer.c -o Test/lexer_test$(EXE) $(shell pkg-config --cflags --libs gtest gtest_main)

clean:
	rm -f $(OBJS) minic$(EXE) Test/lexer_test$(EXE)
