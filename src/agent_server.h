#pragma once

#include <memory>
#include <string>

class GameWindow;
class WindowCallbacks;
class JniSupport;

// Unix-socket control surface for driving the client from another process: one JSON object per line in,
// one per line out. Input is queued and replayed on the looper thread so it takes the same path as a real
// keyboard and mouse; frames are read back on the render thread right before the swap.
class AgentServer {
public:
    static void start(std::string const &path, std::shared_ptr<GameWindow> window, std::shared_ptr<WindowCallbacks> callbacks, JniSupport *jni);

    static void drain();

    static void onBeforeSwap(GameWindow *window);
};
