#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "./src/raygui.h"
#include "./src/raygui/styles/dark/style_dark.h"

int main() {
    InitWindow(800, 480, "Spotify Pi Thing");
    GuiLoadStyleDark();
    SetTargetFPS(60);
    
    bool showMessageBox = false;

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

        if (GuiButton((Rectangle){ 24, 24, 120, 30 }, "#191#Show Message")) showMessageBox = true;

        if (showMessageBox) {
            int result = GuiMessageBox((Rectangle){ 85, 70, 250, 100 },
                "#191#Message Box", "Hi! This is a message!", "Nice;Cool");

            if (result >= 0) {
                showMessageBox = false;
            }
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}