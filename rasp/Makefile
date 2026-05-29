CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread -lm -ldl

TARGET = synth
SRC = engine.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
