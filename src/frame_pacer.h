#pragma once

class GameWindow;

// Caps the game's swap rate: an explicit cap always wins, otherwise the unfocused cap from Settings applies
// while the window is in the background. Bedrock ticks independently of rendering, so blocking swaps is a
// pure CPU/GPU saving.
class FramePacer {
public:
    static void setCap(int fps);
    static int getCap();
    static int activeCap(GameWindow *window);
    static void afterSwap(GameWindow *window);
    static double measuredFps();
};
