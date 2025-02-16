CC = gcc
CFLAGS = -DRAYGUI_IMPLEMENTATION
LDFLAGS = -L/usr/local/lib -lcurl -lqrencode -lraylib -lGL -ldl -lrt -lX11 -lm -lpthread -lpigpio

SRC = spotify.c src/cjson/cJSON.c

all:
	$(CC) -o build/spotify $(SRC) $(CFLAGS) $(LDFLAGS)