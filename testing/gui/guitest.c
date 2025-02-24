#include "raylib.h"
#include "buttons.h"

#define RAYGUI_IMPLEMENTATION
#include "./src/raygui.h"
#include "./src/raygui/styles/dark/style_dark.h"

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

int main() {
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Spotify Pi Thing");
    GuiLoadStyleDark();
    SetTargetFPS(60);
    
    bool showMessageBox = false;

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

        if (GuiButton((Rectangle){ 24, 24, 120, 30 }, "#191#Show Message")) showMessageBox = true;

        if (GuiButton((Rectangle){ 85, 70, 250, 100 }, is_playing ? "Pause" : "Play")) {
            is_playing = !is_playing;

        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}