#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <pigpio.h>
#include <signal.h>
#include <pthread.h>

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
volatile bool is_playing = false;
pthread_mutex_t is_playing_mutex = PTHREAD_MUTEX_INITIALIZER;

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

    while (!WindowShouldClose() && running) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            running = 0;
        }

        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

        bool current_playing;
        pthread_mutex_lock(&is_playing_mutex);
        current_playing = is_playing;
        pthread_mutex_unlock(&is_playing_mutex);

        if (GuiButton((Rectangle){ 85, 70, 250, 100 }, is_playing ? "Pause" : "Play")) {
            pthread_mutex_lock(&is_playing_mutex);
            is_playing = !is_playing;
            pthread_mutex_unlock(&is_playing_mutex);
        }

        EndDrawing();
    }

    running = 0;
    pthread_join(gpio_t, NULL);
    CloseWindow();

    return 0;
}