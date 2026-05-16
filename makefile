CC = gcc
CFLAGS = -O2 -Wall -Wextra -pthread

TARGET = teste
SRC = teste.c

all:
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)