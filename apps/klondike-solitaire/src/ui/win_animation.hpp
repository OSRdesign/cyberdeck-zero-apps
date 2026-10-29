/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike UI: the win animation, the classic cascade (the physics is core/win_sim.hpp, the drawing win_animation.cpp).
 *
 * The app calls start() when the last card reaches the foundations: the view layer gets the BoardView (its card objects,
 * their positions, the felt: board_view.hpp "win animation hooks"), and `done` is called exactly once, either when the
 * cascade has finished or when it was skipped (the app then shows the result dialog); with the animation preference Off
 * `done` comes at once (from a posted callback, never from inside start()). stop() cancels everything: the trail is
 * cleared, the table is put back as the game says, no `done` call after it (a new game was started, the dialog was
 * closed, the app is closing). LVGL thread only.
 *
 *   running()   the cascade is playing (keys, taps and the buttons skip it instead of acting)
 *   skip()      end it now: the trail clears, `done` is called (from a posted callback); safe inside any event
 *   After a natural end the last picture stays behind the result dialog until stop() (the app calls it when the dialog
 *   closes); a skip clears it at once.
 */
#pragma once

#include <functional>
#include <memory>

namespace kui {

class BoardView;

class WinAnimation {
public:
    virtual ~WinAnimation() = default;
    virtual void start(BoardView &board, std::function<void()> done) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual void skip() = 0;
};

std::unique_ptr<WinAnimation> make_win_animation();

} // namespace kui
