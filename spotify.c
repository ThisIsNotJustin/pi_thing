#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <pigpio.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#define PP_BUTTON 23
#define SKIP_BUTTON 27
#define BACK_BUTTON 22
#define ADC_ADDR 0x4b

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "./src/raygui.h"
#include "./src/raygui/styles/dark/style_dark.h"

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

volatile int running = 1;
volatile bool logged_in = false;
// transfer to SpotifyClient?
volatile bool is_playing = false;
// make a mutex for the entire SpotifyClient?
pthread_mutex_t is_playing_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct SongInfo {
    char title[128];
    char artist[128];
    char album[128];
    char url[256]; 
    int progress;
    int duration;
} SongInfo;

typedef enum SearchResults {
    playlist,
    album
} SearchResults;

typedef struct SpotifyClient {
    char access_token[256];
    char refresh_token[256];
    int refresh_timer;
    int refresh_timeout;
    int volume;
    bool is_playing;
    bool shuffle;
    char current_playing_id[256];
} SpotifyClient;

// spotify client constructor from token
// refresh token

// get song
// get volume
// change volume
// get playback state
// get shuffle state
// get liked state
// set playback state
// set shuffle state
// set liked state
// skip song
// previous song
// search
// play
// pause

void handle_sigint(int sig) {
    running = 0;
}

int readADC(int handle, int channel) {
    char controlByte = (channel << 4) | 0x80;
    char data[1];

    if (i2cWriteDevice(handle, &controlByte, 1) != 0) {
        return -1;
    }

    if (i2cReadDevice(handle, data, 1) != 1) {
        return -1;
    }

    return (int)data[0];
}

void buttonPressed(int gpio, int level, uint32_t tick) {
    static uint32_t lastTick = 0;
    if (tick - lastTick < 100000) return;
    lastTick = tick;

    if (level == PI_LOW) {
        pthread_mutex_lock(&is_playing_mutex);

        switch (gpio) {
            case PP_BUTTON:
                is_playing = !is_playing;
                printf("Status: %s\n", is_playing ? "Playing" : "Paused");
                break;

            case SKIP_BUTTON:
                printf("Skip Song\n");
                break;

            case BACK_BUTTON:
                printf("Previous Song\n");
                break;

            default:
                break;
        }
        pthread_mutex_unlock(&is_playing_mutex);
    }
}

void* gpio_thread_func(void* arg) {
    if (gpioInitialise() < 0) {
        fprintf(stderr, "Failed to initialize pigpio\n");
        return NULL;
    }

    gpioCfgSetInternals(gpioCfgGetInternals() | PI_CFG_NOSIGHANDLER);

    int i2cHandle = i2cOpen(1, ADC_ADDR, 0);
    if (i2cHandle < 0) {
        fprintf(stderr, "Failed to open I2C\n");
        gpioTerminate();
        return NULL;
    }

    gpioSetMode(PP_BUTTON, PI_INPUT);
    gpioSetPullUpDown(PP_BUTTON, PI_PUD_UP);
    gpioSetMode(SKIP_BUTTON, PI_INPUT);
    gpioSetPullUpDown(SKIP_BUTTON, PI_PUD_UP);
    gpioSetMode(BACK_BUTTON, PI_INPUT);
    gpioSetPullUpDown(BACK_BUTTON, PI_PUD_UP);

    gpioSetAlertFunc(PP_BUTTON, buttonPressed);
    gpioSetAlertFunc(SKIP_BUTTON, buttonPressed);
    gpioSetAlertFunc(BACK_BUTTON, buttonPressed);

    int prev = -1;
    while (running) {
        int values = readADC(i2cHandle, 0);
        if (values < 0) {
            fprintf(stderr, "Error reading ADC\n");
            break;
        }

        if (values != prev) {
            if (values > 0) {
                printf("Volume: %d\n", values);
                prev = values;
            }
        }

        time_sleep(0.1);
    }

    i2cClose(i2cHandle);
    gpioTerminate();
    return NULL;
}

int main() {
    signal(SIGINT, handle_sigint);

    pthread_t gpio_t;
    if (pthread_create(&gpio_t, NULL, gpio_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to start GPIO Thread\n");
        return 1;
    }

    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Spotify Pi Thing");
    GuiLoadStyleDark();
    SetTargetFPS(60);

    char client_id[256] = {0};
    char client_secret[256] = {0};

    // 0 - none, 1 - client id, 2 - client secret
    int activeText = 0;

    while (!WindowShouldClose() && running) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            running = 0;
        }

        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

        // if the user is already logged in, display app like normal
        // otherwise we need the user to give their client id, secret, 
        // and login with spotify
        if (logged_in) {
            bool current_playing;
            pthread_mutex_lock(&is_playing_mutex);
            current_playing = is_playing;
            pthread_mutex_unlock(&is_playing_mutex);

            if (GuiButton((Rectangle){ 85, 70, 250, 100 }, is_playing ? "Pause" : "Play")) {
                pthread_mutex_lock(&is_playing_mutex);
                is_playing = !is_playing;
                pthread_mutex_unlock(&is_playing_mutex);
            }
        } else {
            // text is roughly centered above rectangle
            // rectangle is centered plus padding between it and client secret text
            DrawText("Enter Spotify Client ID: ", SCREEN_WIDTH/2 - 125 + 5, SCREEN_HEIGHT/3 - 80, 20, WHITE);
            Rectangle clientIdInput = {SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/3 - 50, 250, 30};

            // text is roughly centered above rectangle
            // rectangle is centered plus padding between it and spotify login button
            DrawText("Enter Spotify Client Secret: ", SCREEN_WIDTH/2 - 125 - 15, SCREEN_HEIGHT/2 - 80, 20, WHITE);
            Rectangle clientSecretInput = {SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/2 - 50, 250, 30};

            if (CheckCollisionPointRec(GetMousePosition(), clientIdInput) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                activeText = 1;
            }

            if (CheckCollisionPointRec(GetMousePosition(), clientSecretInput) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                activeText = 2;
            }

            // no way to validate at the moment so just assuming this works
            if (GetTouchPointCount() > 0) {
                Vector2 touchPos = GetTouchPosition(0);

                if (CheckCollisionPointRec(touchPos, clientIdInput)) {
                    activeText = 1;
                }

                if (CheckCollisionPointRec(touchPos, clientSecretInput)) {
                    activeText = 2;
                }
            }

            GuiTextBox(clientIdInput, client_id, 128, activeText == 1);
            GuiTextBox(clientSecretInput, client_secret, 128, activeText == 2);

            // just simulating a successful login at the moment
            // should be about centered
            if (GuiButton((Rectangle){SCREEN_WIDTH/2 - 125, SCREEN_HEIGHT/2, 250, 40}, "Login with Spotify")) {
                if (strlen(client_id) > 0 && strlen(client_secret) > 0) {
                    logged_in = true;
                }
            }
        }

        EndDrawing();
    }

    running = 0;
    pthread_join(gpio_t, NULL);
    CloseWindow();

    return 0;
}