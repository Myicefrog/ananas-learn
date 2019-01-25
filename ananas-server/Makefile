CC = g++
STD = -std=c++14
CFLAGS = -g -Wall
SRC = $(wildcard *.cc)
OBJ = $(patsubst %cc, %o, $(SRC))
BIN = ana
all:$(OBJ)	
	$(CC) $(OBJ) -lpthread -o $(BIN) 

%.o:%.cc	
	$(CC) -I../ -I../ananas -I../ananas/net -I../ananas/util  $(STD) $(CFLAGS) -c $< -o $@ 

clean:
	rm  $(BIN) $(OBJ)
