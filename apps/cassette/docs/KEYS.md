# Cassette: keyboard map

The deck is driven from a Bluetooth or USB keyboard; every key below also has its control on the screen. The same map
is in the comment block of `src/ui/cassette_ui.hpp` and is handled in `CassetteUi` (`src/ui/cassette_ui.cpp`).

Esc is "back" everywhere, as in every app: a box, a picker or a text field closes first; on the recorder Esc ends the
app (during the record countdown it cancels the countdown). The header's Esc button does the same as the key.

## Recorder

| Key | Does |
|---|---|
| R | REC |
| Space | PLAY / PAUSE |
| X | STOP |
| , (comma) | REW: held, it scans backwards at 5x (the press and the release both reach the deck) |
| . (period) | FWD: held, it scans forwards at 5x |
| L | the recordings list |
| S | the settings |
| Esc | ends the app (cancels a running countdown) |

A held deck key is drawn pressed until it is released.

The PLAY / PAUSE key shows the current state, latched (drawn pressed down) like REC while recording; what a press does
does not change (it plays, pauses and resumes). The same at every screen size:

| State | The key shows | Latched |
|---|---|---|
| Stopped, nothing playing or recording (also the record countdown) | PLAY | no |
| A take playing (normal speed, or while REW / FWD is held) | PLAY | yes |
| A take paused | PAUSE | yes |
| Recording (REC latched) | PAUSE (what a press does) | no |
| A recording paused | PAUSE | yes |

## Recordings list

| Key | Does |
|---|---|
| Up / Down | move the selection |
| Enter | PLAY the selected take |
| N | RENAME (the text box) |
| T | TRIM (the trim and split page) |
| Delete | DELETE (asks in the confirm box) |
| S | the settings |
| Esc | back to the recorder |

## Trim and split

| Key | Does |
|---|---|
| Left / Right | move the marker 0.1 s (with Shift 1 s) |
| Up | zoom in |
| Down | zoom out |
| I | SET IN |
| O | SET OUT |
| Enter | SPLIT at the marker; REMOVE while a split flag is selected |
| Tab | select the next split flag (the marker moves to it) |
| Space | PLAY |
| S | SAVE (the save box) |
| Esc | back to the list, nothing saved |

## Save box and confirm box

| Key | Does |
|---|---|
| Left / Right, Tab | move between the buttons (a white outline shows the one chosen) |
| Enter | choose; without a move: SAVE AS NEW FILES in the save box, CANCEL in the confirm box |
| Esc | cancel |

## Settings

| Key | Does |
|---|---|
| Up / Down | move |
| Enter, Right | change or open the row (a switch flips, a choice opens its list, a text row its box); on a slider row Left / Right step it |
| Esc | back (a choice list first, then the recorder) |
| L | the recordings list |
