#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

extern volatile bool is_playing;

void buttonPressed(int gpio, int level, uint32_t tick);

#endif