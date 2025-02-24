#include <stdio.h>
#include <pigpio.h>
#include <signal.h>
#include "buttons.h"

#define PP_BUTTON 23
#define SKIP_BUTTON 27
#define BACK_BUTTON 22
#define ADC_ADDR 0x4b

volatile int running = 1;
volatile bool is_playing = false;

void handle_sigint(int sig) {
    running = 0;
}

int readADC(int handle, int channel) {
    char controlByte = (channel << 4) | 0x80;
    char data[1];

    if (i2cWriteDevice(handle, &controlByte, 1) != 0) {
        fprintf(stderr, "Failed to write control byte\n");
        return -1;
    }

    if (i2cReadDevice(handle, data, 1) != 1) {
        fprintf(stderr, "Failed to read data\n");
        return -1;
    }

    return (int)data[0];
}

void buttonPressed(int gpio, int level, uint32_t tick) {
    static uint32_t lastTick = 0;
    if (tick - lastTick < 100000) return;

    lastTick = tick;
    if (level == PI_LOW) {
        switch (gpio) {
            case PP_BUTTON:
                printf("Play/Pause\n");
                is_playing = !is_playing;
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
    }
}

int main() {
    signal(SIGINT, handle_sigint);

    if (gpioInitialise() < 0) {
        fprintf(stderr, "Failed to initialize pigpio\n");
        return 1;
    }

    gpioCfgSetInternals(gpioCfgGetInternals() | PI_CFG_NOSIGHANDLER);

    int i2cHandle = i2cOpen(1, ADC_ADDR, 0);
    if (i2cHandle < 0) {
        fprintf(stderr, "Failed to open I2C\n");
        gpioTerminate();
        return 1;
    }

    gpioSetMode(PP_BUTTON, PI_INPUT);
    gpioSetPullUpDown(PP_BUTTON, PI_PUD_UP);
    gpioSetMode(SKIP_BUTTON, PI_INPUT);
    gpioSetPullUpDown(SKIP_BUTTON, PI_PUD_UP);
    gpioSetMode(BACK_BUTTON, PI_INPUT);
    gpioSetPullUpDown(BACK_BUTTON, PI_PUD_UP);

    int prev = -1;
    while (running) {
        int values = readADC(i2cHandle, 0);
        if (values < 0) {
            fprintf(stderr, "Error reading ADC\n");
            break;
        }

        if (values != prev) {
            if (values > 0) {
                printf("Volume Up\n");
            } else if (values == 0) {
                printf("Volume off\n");
            }

            prev = values;
        }

        gpioSetAlertFunc(PP_BUTTON, buttonPressed);
        gpioSetAlertFunc(SKIP_BUTTON, buttonPressed);
        gpioSetAlertFunc(BACK_BUTTON, buttonPressed);

        time_sleep(0.1);

    }

    printf("Terminating Program\n");
    i2cClose(i2cHandle);
    gpioTerminate();

    return 0;
}