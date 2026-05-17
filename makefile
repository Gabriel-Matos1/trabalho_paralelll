CC = gcc
CFLAGS = -O2 -fgnu89-inline -Wall -Wextra -pthread

TARGET = teste
SRC = teste.c

all:
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)