#include <stdio.h>
#include <pigpio.h>
#include <signal.h>

#define PP_BUTTON 4
#define SKIP_BUTTON 27
#define BACK_BUTTON 22
#define ADC_ADDR 0x4b

volatile int running = 1;

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

    }

    printf("Terminating Program\n");
    i2cClose(i2cHandle);
    gpioTerminate();

    return 0;
}