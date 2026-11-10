# Cassette: user guide

Cassette is a sound recorder that looks like a tape deck. You plug a USB sound card or a USB headset into a
CyberDeck board, press REC, and the sound goes into a WAV file in the `Recordings` folder. You can listen again, cut
a take with trim and split, and watch a level meter while you record.

This guide describes version 0.1.0 as it is in the source. Pictures are rendered on a PC from the app's test scenes
(a made-up sound source: the tones and levels are not real audio); the two captures at the end of
[Screen sizes](#screen-sizes-and-layouts) come from real boards. How to refresh the pictures:
[MAKING-SCREENSHOTS.md](MAKING-SCREENSHOTS.md).

Contents:
[What you need](#what-you-need) |
[First run](#first-run) |
[The recorder screen](#the-recorder-screen) |
[Recording](#recording) |
[Listening while you record](#listening-while-you-record-monitor) |
[Playing](#playing) |
[The recordings list](#the-recordings-list) |
[Trim and split](#trim-and-split) |
[Settings](#settings) |
[Take number](#take-number) |
[Screen sizes](#screen-sizes-and-layouts) |
[Keys](#keyboard-shortcuts) |
[Files](#files-and-where-they-live) |
[Crash safety](#crash-safety) |
[Troubleshooting](#troubleshooting) |
[Limits](#limits)

## What you need

- **A USB sound card or a USB headset with a microphone.** Cassette records from any sound card that Linux sees as a
  capture device. It finds the card by its name and USB identity, never by its number, so a card that gets a different
  number after a reboot is still the same card for Cassette.
- A CyberDeck board with the launcher and a screen of at least 480x320 (portrait 320x480 in PDA mode). Square
  screens such as the HackBerry's 720x720 and 480x480 have their own layout.
- Your user must be in the `audio` group. The board images do this for the first user; otherwise run
  `sudo usermod -aG audio $USER` and log out and in again.
- Nothing else. There is no network code in the app: nothing you record leaves the board.

**Cards used so far** (named by the owner; the source does not hold the results of the board tests, so what each
card does is described only where the code says it): the PCM2902 USB sound card with a microphone, the UC02 USB-C
headset, the Jabra Evolve 65, the Link 370 headset, the Apple USB-C dongle. Cards differ in what they offer, and the
app adapts: some have a level control, some have auto gain or a mute switch, some can also play sound out (needed for
live monitoring), some record only at 16000 Hz (see [Troubleshooting](#troubleshooting)).

## First run

1. Plug the card in, then open **Cassette** from the home grid (category Tools).
2. Cassette picks the first capture card it finds and opens it at once. The level meter moves when you make a sound,
   even before you record anything. The folder `~/Recordings` is created if it is not there.
3. If you have more than one card, open Settings (the gear button or the **S** key) and choose the card in the
   **Input** row. Cassette remembers your choice, and the card's own level, auto gain and mute for each card.
4. Press **REC** (red key, or **R**) to start a take, **STOP** (or **X**) to end it.

If no card is found, the cassette window says *No sound input found. Plug in a USB sound card or headset.* REC is
dimmed. Plug a card in: Cassette finds it by itself within a few seconds.

![No sound input](img/no-input.png)

With no take on the board yet (first start, or after the last take was deleted), the deck holds a **blank
cassette** with an empty label. REC works as usual; the label fills in when the first take exists.

![Blank cassette](img/blank-cassette.png)

## The recorder screen

The recorder is the first page. This is the landscape layout at 640x480 (the deck's screen) while recording; the
numbers are explained below. The other layouts hold the same parts in another order (see
[Screen sizes](#screen-sizes-and-layouts)).

![The recorder screen with numbered regions](img/annot-recorder-deck.png)

| No. | Region | What it shows |
|---|---|---|
| 1 | **Cassette window** | The cassette with its label (the take's name, `SIDE A`, the format such as `WAV 48k`). The two hubs turn while the deck runs: counterclockwise while recording and playing. They stand still when stopped or paused. With no card, the window shows the message about no sound input instead. |
| 2 | **Counter** | The time of the take: while recording, how long the take is; while playing or paused, the position in the take. It reads 00:00 when the deck is idle. The **PEAK** lamp beside it is explained below. |
| 3 | **Recordings** and **Settings** buttons | Open the recordings list (key **L**) and the settings (key **S**). |
| 4 | **Info strip** | The take's name in capitals, the format and the time left on the card (for example `TAKE 007 · 48k/16 WAV · 12 h 40 left`); while a take plays it reads `PLAYING TAKE 006 · 05:44 / 12:08`. At the right: the **LIM** and **LOW CUT** lamps (lit when those switches in Settings are on) and, while the deck works, a red **REC** or green **PLAY** word with a lamp. A notice replaces the strip for a few seconds (a take saved, the card unplugged, a refusal). On narrow screens this is a small box next to the counter. |
| 5 | **Level meter** | 15 bars from -40 to 0 dBFS, one row per channel (a mono card has one row, a stereo card two: L over R). Details below. |
| 6 | **CARD** and **SOFT** gain | CARD is the sound card's own level (shown only when the card has a level control). SOFT is gain added by Cassette, 0 to +24 dB. Each has a minus and a plus button, 1 dB a step. |
| 7 | **The five keys** | REW, PLAY / PAUSE, FWD, STOP, REC. |

### The level meter

- The scale marks -40, -20, -10 and 0. The bars go from green through amber to red at the top. Under the scale a
  bracket marks the **TARGET** zone, -18 to -6 dBFS. Aim for your loudest words to reach the TARGET zone and not
  go above it.
- A thin **peak hold** mark stays on the highest bar for 1.5 s and then falls back.
- The **PEAK** lamp lights red when a sample reaches -1 dBFS or higher and stays on for 1 s. That is the warning that
  the recording may be clipping. Turn the level down.
- The meter shows the live input when the deck is idle, counting down or recording. While a take plays or is paused it
  shows the **played** level on L and R (a mono take on both rows); the PEAK lamp stays off then.
- The **MON** lamp (bottom left of the meter, under IN) is explained in the next-but-one section.

| Level in the TARGET zone | Level too low |
|---|---|
| ![Recording with the level in the target zone](img/recording.png) | ![Recording with a low level](img/recording-low.png) |
| Good. | The bars stop in the green: raise CARD (or SOFT). |

### CARD and SOFT

- **CARD** changes the sound card's own recording level (its mixer). Cassette stores it per card and puts it back
  when the app starts, because the card's mixer resets at every reboot. With no level control on the card, CARD is not
  drawn and SOFT takes the full width.
- **SOFT** multiplies the signal in software, 0 to +24 dB. Use CARD first, SOFT when CARD is at its top. SOFT also makes the card's hiss louder (see
  [Troubleshooting](#troubleshooting)).

![A card without a level control](img/no-card-level.png)

### The five keys

| Key | Does | Notes |
|---|---|---|
| **REW** | Hold: scan backwards at 5x (or jump back 5 s, see Settings) | Needs a take loaded. Dimmed while recording. A **5x** badge shows on the cassette window while it scans. |
| **PLAY / PAUSE** | Plays the loaded take; pauses and resumes | While recording, it pauses and resumes the **same** take. |
| **FWD** | Hold: scan forwards at 5x (or jump 5 s) | Same as REW. |
| **STOP** | Stops recording or playing | Also cancels the record countdown. |
| **REC** | Starts a take (after the countdown, if one is set) | Dimmed with no card, and while a take records. Pressing REC during playback stops the playback and starts recording. |

The PLAY / PAUSE key shows what is going on: it reads **PLAY** and is not pressed when you are stopped; **PLAY**
pressed in while a take plays (also while you hold REW or FWD); **PAUSE** pressed in while a take or a recording is
paused; **PAUSE** not pressed while recording (that is what a press would do). REC stays pressed in while a take
records or is paused.

## Recording

1. Check the meter and the gain. Say something at the loudest level you expect.
2. Press **REC**. The hubs start to turn and the counter runs. The take is a new file; its name is on the label.
3. **PLAY / PAUSE** pauses the take (the counter stops, the hubs stop); press it again to go on in the same file.
4. **STOP** ends the take. It becomes the loaded take (the one PLAY plays). A short notice says what happened.

**Record timer.** In Settings, *Record timer* can be Off (default), 3 s, 10 s or 20 s. With a timer, REC starts a
countdown over the cassette window (3, 2, 1 ...). The meter stays live. A tap on the window, STOP or Esc cancels it:
nothing is written. At zero the take starts. The timer makes no sound.

![The record countdown](img/countdown.png)

**Names and numbers.** Each take is a file named `take_006_20261108-1105.wav`: the prefix (`take` by default),
the take number (at least 3 digits), and the date and time. The take number only goes up and **never repeats**, even
after you delete takes (see [Take number](#take-number) to move it). Until the board's clock is set from the
network, the date reads `nodate`: a wrong date in a name would be worse than none. On screen the name shows as
`Take 006`. You can change the prefix in Settings (*File prefix*: letters, digits, `-` and `_`, 1 to 24 characters)
or rename a take in the list.

**Long takes: auto-split.** A WAV file cannot hold more than 4 GiB the usual way. With *Auto-split* at **At 4 GiB**
(default) a take that reaches 4 GiB goes on in a new file with `_p2` added to the name, then `_p3`. With **Off**, the
take is one RF64 file (a WAV variant for big files; most players read it). For scale: 4 GiB is about 12 hours of mono
audio at 48 kHz and 16 bit (a stereo card fills it in about 6).

**When a take ends by itself.** Cassette ends a take cleanly, keeps what was recorded, and shows a notice when:
the free space falls under 200 MB, the card is unplugged, or the disk reports an error. If audio could not be written
in time (a slow SD card or a card that stalled), a red `lost 0.3 s` is shown at the end of the info strip during the
take, and the take's journal keeps the figure.

![Lost audio shown in red](img/recording-lost-audio.png)

**Esc.** On the recorder, Esc ends the app. A take that records is stopped and kept first. Cassette does **not**
record in the background: the app owns the screen while it records.

**Mono and stereo.** Cassette records the channels the card gives (one or two); Settings > Channels shows which. A
stereo card gets two meter rows.

![Stereo meter](img/recording-stereo.png)

**Low cut and Limiter.** Two switches in Settings. *Low cut 80 Hz* removes rumble and wind noise (a high-pass filter).
*Limiter* is a simple peak limiter without look-ahead. Both are off by default; when on, small `LIM` and `LOW CUT`
lamps show in the info strip of the landscape layout. They change the recording.

**Sound server.** If another program (the sound server of a desktop board) holds the card, Cassette records through the
server instead and says so: *Recording through the sound server: the card is in use*, and `(server)` appears next to
the format. When the card is opened directly and a sound server runs, Cassette tells you the other apps cannot use the
card while it records.

![Recording through the sound server](img/recording-server.png)

## Listening while you record: Monitor

**Monitor** plays the microphone into the headphones of the same card, so you hear yourself while you speak.
Settings > **Monitor** is a switch (off by default); **Monitor level** is its volume, 0 to 100 % in steps of 5
(default 50 %), dimmed while Monitor is off.

- Both rows appear **only when the selected card can also play sound** (a headset can, a bare microphone card cannot).
- It runs on the recorder screen: idle, during the countdown, while recording, and while a recording is paused.
- You hear yourself about 50 ms late. That is fine for checking the voice, not for singing in time.
- What you hear is what is recorded (after the gain and the filters), at the monitor level. The monitor level never
  changes the recording.
- **Use headphones.** With speakers the microphone hears the monitor and you get an echo or a howl. This is why
  Monitor is off by default.
- **It is off during playback.** The monitor stops when you play, scan or preview a take and while the trim page is
  open; it comes back when playback ends.
- If the monitor cannot start (for example another program holds the card's output) the notice *Monitor unavailable*
  shows and the monitor stays off. The recording is not affected.
- **Sidetone on a Jabra.** A headset such as the Jabra Evolve 65 has its own hardware sidetone: you hear yourself
  even with Monitor off. That is not an ALSA control and Cassette cannot change it. With Monitor on you get a second,
  later copy on top: leave Monitor off for that headset. Monitor is meant for cards without sidetone (a cheap USB
  dongle, the UC02 headset).

The **MON lamp** shows whether the monitor is playing right now: lit green while it runs, dark when the switch is off,
during playback, or after a failure. It is drawn only when the card has a playback side.

![Recording with the MON lamp lit](img/recording-mon-lit.png)

## Playing

The deck plays the **loaded take**: the newest take at start, the take you just recorded, or the one you loaded from
the list. Its name is on the cassette label.

- **PLAY / PAUSE** plays the take from where the tape is (after STOP, or a take that ended, that is the start).
  Pause keeps the position.
- **REW and FWD, held:** the take scans at 5x (like fast tape: the sound is audible, higher in pitch), with a
  **5x** badge. Let go and normal play continues, or the pause continues if you were paused. The hubs turn at
  3x speed: FWD the same way as in play, REW the other way round.
- **Jump mode:** in Settings, *REW / FWD* can be **Jump 5 s**: every tap moves 5 s, and a held key repeats about
  three times a second.
- **STOP** stops and rewinds to the start. The counter reads 00:00.
- **End of the take:** when playback reaches the end the deck stops by itself and the hubs stand still. Scanning
  forward to the end ends the take the same way; scanning back to the start also stops the deck. PLAY then plays from
  the start.
- **Blank cassette:** with no take, PLAY, REW and FWD are dimmed.
- REC during playback stops it and starts a new take (or its countdown).

The sound goes out of the card's own playback side when it has one, otherwise the default device. If the board has
more than one output, Settings > **Output** lets you choose.

| Playing | Paused | REW held |
|---|---|---|
| ![Playing a take](img/playing.png) | ![Paused](img/play-paused.png) | ![Scanning backwards at 5x](img/play-rew.png) |
| PLAY pressed in, the meter shows the played level on L and R | PAUSE pressed in, the meter falls away | 5x badge, REW and PLAY pressed in |

If the playback device is lost, the deck pauses and the notice *The sound output was lost* shows.

## The recordings list

Press the list button (or **L**). The takes are listed newest first. Each row has the take number on a small sticker,
the name, when it was made (`today 11:05`, `yesterday 18:20`, a date, or `nodate`), its length and size, and its length
again in large figures at the right. A take that was repaired after a power cut says
`repaired`; a take that cannot be read says `damaged`. A strip shows how many takes there are, the folder and the free space.

![The recordings list](img/list.png)

Four actions: **LOAD**, **RENAME**, **TRIM**, **DELETE**. A tap selects a row.

- **LOAD** puts the selected take into the deck and returns to the recorder. The cassette in the window drops out,
  the new one falls in with a small bounce, and its name is written onto the label in handwriting. It takes about
  0.9 s; keys are ignored meanwhile except Esc, which ends the animation at once. Then PLAY plays that take. LOAD is
  dimmed while a take records, a recording is paused or the countdown runs.

  | 0 ms | 190 ms | 420 ms | 750 ms |
  |---|---|---|---|
  | ![0 ms](img/load-0000ms.png) | ![190 ms](img/load-0190ms.png) | ![420 ms](img/load-0420ms.png) | ![750 ms](img/load-0750ms.png) |
  | the old cassette is still there | the window is empty | the new cassette falls in | landed, the name is half written |

- **Preview:** double-tap a row (or press Enter) to listen to it without loading it. Double-tap the same row again to
  stop and rewind. Closing the list stops the preview. A preview never changes the loaded take.

  ![Previewing a take](img/list-preview.png)

- **RENAME** opens a text box to change the **prefix** of the name (the first part of the file name); the take
  number, the date and the time stay. The prefix takes letters, digits, `-` and `_`, 1 to 24 characters. With no
  keyboard connected the launcher shows its on-screen keyboard. If the name is not allowed, a notice says *Not
  renamed* and why. The loaded take stays loaded under its new name.

  ![Rename](img/rename.png)

- **TRIM** opens the [trim and split page](#trim-and-split) for the take. It is refused while a take records.
- **DELETE** asks first (a box with DELETE and CANCEL; Enter without moving means CANCEL). Deleting
  the loaded take empties the deck and the blank cassette returns at once. A take that plays or is open for trimming
  cannot be deleted.

  ![Delete asks first](img/confirm-delete.png)

## Trim and split

Open it from the list with **TRIM** (key **T**). It cuts a take without changing the audio: Cassette copies the sound
as it is, nothing is decoded or converted.

![The trim and split page](img/trim.png)

What you see: the top line with the take, IN, OUT, the kept length (LEN) and the number of splits; a thin **overview**
strip of the whole take (the part to keep is amber, the cut part grey, the white frame is the part shown below); the
**waveform** of the window under a fixed red **marker** (the play position); the **IN** and **OUT** handles (a chip
at the edge when they are outside the window); and any number of numbered green **split flags**.

- **Moving the marker:** drag the waveform sideways, use the POSITION buttons (0.1 s a step), or the Left / Right
  keys (Shift: 1 s). Flags and handles are not dragged.
- **ZOOM** from the whole take down to a window of about 2 s (the stepper shows the window's length; Up / Down keys).
  The waveform scrolls with the marker.
  The first time you open a take, Cassette builds its waveform overview (the `.peaks` file); for a long take that
  may take a moment.
- **SET IN / SET OUT** (keys I and O) set the start and end of the kept part at the marker. They are refused if IN
  would not be before OUT. Moving IN or OUT removes split flags that fall outside.
- **SPLIT** (Enter) puts a split flag at the marker, between IN and OUT. Add as many as you like. A tap on a flag
  selects it (white); then the key reads **REMOVE** and removes it. Tab selects the next flag.
- **PLAY** (Space) plays the kept part from the marker (or from IN) and stops at OUT.
- **SAVE** (S) opens the save box. Esc goes back to the list and saves nothing.

![Zoomed in, a flag selected](img/trim-zoomed.png)

**The save box** says what is kept (`Keep 02:58.7 to 10:28.7, cut at 2 splits: 3 files.`) and lists the files that
will be written: one file for each part between IN, the split flags and OUT, with its length.

![Save box](img/save-box.png)

- **SAVE AS NEW FILES** (the default; Enter without moving): the original take stays. The new files are named after
  it with `-1`, `-2` ... added (`take_006_20261108-1105-1.wav`). Edit numbers are never reused for the same take, even
  if you deleted a result.
- **REPLACE ORIGINAL**: asks once more (*Delete the original take?* "It is deleted only after every new file is
  written and synced."). A single trimmed part takes the original's name. With several parts the original is removed
  after all parts are written.

![Replace asks again](img/confirm-replace.png)

Saving is safe against a power cut: each result is written to a temporary file, synced, then renamed into place. A
crash leaves the original or the finished results, never half a file. A notice tells the result (*Saved 3 new files*,
*Saved, original replaced*, or *Save failed* and the reason).

## Settings

Open with the gear button or **S**. Changes apply at once and are saved at once. Rows with a switch flip when you
tap them; rows with a choice open a list; sliders step with Left / Right. Esc closes a choice list first, then goes
back to the recorder.

![Settings, top](img/settings-top.png)

The card's own rows depend on what the selected card has:

| Row | What it does | Default | Appears |
|---|---|---|---|
| **Input** | The capture card. Cassette remembers it. | the first card | always; cannot change while a take records |
| **Input level** | The card's recording level (same as CARD on the recorder). | what the card has | only when the card has a level control |
| **Auto gain (card)** | The card's own automatic gain, if it has one. | as set on the card, then remembered | only when the card has it |
| **Mic mute (card)** | Mutes the card's microphone. | off | only when the card has it |
| **Format** | Shows what the take is: rate, depth, channels (for example `WAV 48 kHz 16-bit`). Read-only: WAV is the only format in this version. | WAV | always |
| **Record timer** | Off, 3 s, 10 s, 20 s countdown before REC starts. | Off | always |
| **REW / FWD** | *Fast scan 5x* (hold) or *Jump 5 s* (tap). | Fast scan 5x | always |
| **Channels** | Mono or Stereo, what the card records. Read-only. | (the card) | always |
| **Low cut 80 Hz** | High-pass filter against rumble and wind. | off | always |
| **Limiter** | Peak limiter. | off | always |
| **Monitor** | Hear the microphone in the card's headphones. | off | only when the card can play sound |
| **Monitor level** | Its volume, 0 to 100 % in steps of 5. | 50 % | with Monitor; dimmed while Monitor is off |
| **Sync every** | How often the take is written safely to the card: 5, 10, 20 or 30 s. See [Crash safety](#crash-safety). | 10 s | always |
| **Folder** | Shows where takes are kept. The list offers the current folder and `~/Recordings`; for another folder edit `folder` in `settings.json` (see Files). Cannot change while a take records. | `~/Recordings` | always |
| **File prefix** | The first part of new take names (1 to 24 of letters, digits, `-`, `_`). | `take` | always |
| **Take number** | The number the next take gets. See below. | one more than the last take | always; dimmed while a take records |
| **Auto-split** | *At 4 GiB* (new file) or *Off (one RF64 file)*. | At 4 GiB | always |
| **Output** | The playback device. | automatic | only when there is more than one |

The SOFT gain is set on the recorder screen (it is also stored). Where the rows do not fit the screen the list
scrolls: drag it, or use Up / Down.

![A choice row opens its list](img/settings-picker.png)

![Settings with Monitor](img/settings-monitor.png)

![Settings, bottom](img/settings-bottom.png)

## Take number

**Settings > Take number** sets where the next take counts from. The row reads `Next: 007`. Tap it to open the page:
the number large, steppers **-1 / +1**, **-10 / +10**, **-100 / +100**, **Reset to 1** and **Set**. Esc closes the
page without changing anything.

![The take number page](img/take-number.png)

- The range is 1 to 9999.
- **Only the next take changes.** Existing files are never renamed.
- A number that the same prefix already uses in the folder is **skipped**: the next free number is taken, and the
  row shows it. So *Reset to 1* with takes 001 to 003 in the folder gives `Next: 004`.
- The number is stored in the file `.take-counter` in the recordings folder and survives a restart. The row is
  dimmed while a take records.

## Screen sizes and layouts

The recorder has four layouts, chosen by the shape of the screen:

| Screen | Layout |
|---|---|
| landscape (480x320, 640x480, 800x480) | cassette window on the left, meter and buttons on the right, info strip, steppers, keys |
| portrait (320x480, 480x800, the narrow PDA shell) | counter row with the info box, meter, steppers, keys, stacked |
| square (width / height from 0.9 to 1.1, such as 720x720 and 480x480) | one column: window, counter row, meter across the full width, CARD and SOFT side by side, keys |

![Layouts side by side](img/layouts.png)

| 480x320 | 640x480 | 320x480 portrait | 800x480 |
|---|---|---|---|
| ![480x320](img/recorder-landscape.png) | ![640x480](img/size-640x480.png) | ![320x480](img/size-320x480.png) | ![800x480](img/size-800x480.png) |

| 720x720 square | 480x480 square | 720x720 with a stereo card |
|---|---|---|
| ![720x720](img/size-720x720.png) | ![480x480](img/size-480x480.png) | ![720x720 stereo](img/square-stereo.png) |

On a square screen the other pages (settings, list, trim, boxes) keep the landscape layout, scaled. The LIM and LOW
CUT lamps are shown only in the landscape strip.

Real captures from boards: the HackBerry (720x720, square) and a Pi 3A+ (480x800 portrait).

| HackBerry, 720x720 | Pi 3A+, portrait |
|---|---|
| ![HackBerry](img/real-hackberry-720.png) | ![Pi 3A+](img/real-pi3a-portrait.png) |

## Keyboard shortcuts

Every key has a control on the screen too. A Bluetooth or USB keyboard is the main way to type text; the launcher's
on-screen keyboard appears only when no keyboard is connected. **Esc** goes back everywhere (a box, a list or a text
field closes first); on the recorder it ends the app, and during the countdown it cancels the countdown. The Esc
button in the header does the same.

**Recorder**

| Key | Does |
|---|---|
| R | REC |
| Space | PLAY / PAUSE |
| X | STOP |
| , (comma) | REW (held: scan backwards 5x) |
| . (period) | FWD (held: scan forwards 5x) |
| L | recordings list |
| S | settings |
| Esc | ends the app (cancels a countdown first) |

**Recordings list**

| Key | Does |
|---|---|
| Up / Down | move the selection |
| Enter | preview (again on it: stop and rewind) |
| L | LOAD |
| N | RENAME |
| T | TRIM |
| Delete | DELETE (asks first) |
| S | settings |
| Esc | back to the recorder |

**Trim and split**

| Key | Does |
|---|---|
| Left / Right | marker 0.1 s (Shift: 1 s) |
| Up / Down | zoom in / out |
| I / O | SET IN / SET OUT |
| Enter | SPLIT, or REMOVE while a flag is selected |
| Tab | select the next flag |
| Space | PLAY |
| S | SAVE |
| Esc | back to the list, nothing saved |

**Save and confirm boxes:** Left / Right or Tab move between buttons; Enter chooses (with no move: SAVE AS NEW FILES
in the save box, CANCEL in the confirm box); Esc cancels.

**Settings:** Up / Down move; Enter or Right change or open a row; on a slider Left / Right step it; L the list; Esc
back (a choice list first, then the recorder).

The full map is in [KEYS.md](KEYS.md).

## Files and where they live

| What | Where |
|---|---|
| The takes | `~/Recordings/` (setting *Folder*): `<prefix>_<number>_<YYYYMMDD-HHMM>.wav`, for example `take_006_20261108-1105.wav` |
| A second part of a long take | `take_006_20261108-1105_p2.wav` |
| A trim or split result | the source name plus `-1`, `-2` ...: `take_006_20261108-1105-1.wav` |
| The take counter | `~/Recordings/.take-counter` (the last number handed out) |
| One journal per take | `~/Recordings/.journal/<take>.json`, open while the take records |
| Edit numbers already used | `~/Recordings/.edits/` (so a deleted `-2` is never handed out again) |
| The waveform overview of a take | `<take>.wav.peaks` next to the take, built the first time the trim page opens it; Cassette checks it against the take's size and time on every use and rebuilds it if they changed. Safe to delete. |
| Leftover temporary files | `.tmp-*` in the folder, from a save that was cut short; removed at the next start |
| The settings | `~/.config/cassette/settings.json`; also each card's level, auto gain and mute. Delete it to reset everything. |

The WAV files are standard (Broadcast Wave). Any player or editor opens them. Copy them off the board with `scp`
or a USB stick. Files you put in the folder yourself show in the list if they are WAV files.

## Crash safety

- While a take records, Cassette writes it in large blocks and every **Sync every** seconds (10 s by default) it forces
  the data onto the SD card and rewrites the file header. So at any moment the file on the card is a valid WAV up to
  the last sync.
- **Power cut or crash:** you lose at most the sound since the last sync, about 10 s with the default (5 to 30 s,
  the setting). At the next launch Cassette finds takes whose journal says "open" or whose header sizes are
  placeholders, repairs the header from the file's real length (a partial last frame is cut off) and shows a notice
  such as *Recovered take_006... after an interrupted recording (2 min kept, about 8 s at the end lost)*. The
  list marks the take `repaired`. The audio itself is not rewritten, and repair can be interrupted and run again.
- **Card unplugged during a take:** the take is closed and kept, a notice says *The sound card was unplugged: the
  take was closed and kept*. When you plug it back the notice *The sound card is back* shows and the card's settings
  are applied again. A new take is needed: the one that was running is not continued.
- **Disk full:** the take stops cleanly when under 200 MB are free, and the take is kept.
- **Trim, split, replace** use temporary files and renames (see above): a power cut gives the old file or the new
  ones, never half a file.

## Troubleshooting

**"No sound input found."** Cassette sees no capture card. Plug the card in again; check that the user is in the
`audio` group (see above); on a terminal, `/usr/share/APPLaunch/bin/cassette-probe` lists the cards, what each offers and its mixer controls,
and opens each for 2 seconds (listen-only, it writes nothing). Run it with `--no-open` to list only.

**The meter does not move.** Check the right card is chosen in Settings > Input, that *Mic mute (card)* is not on, and
that the headset's own mute is off. Raise CARD. Some headsets in call profile deliver a weak signal: use SOFT.

**"The card is in use" / "Recording through the sound server".** Another program (on desktop boards, the sound
server) holds the card. Cassette records through the server, at 48000 Hz 16 bit. The take is fine. If you want the
direct card, close the other program. The other way round: while Cassette records directly, other programs get
silence from that card, and Cassette says so.

**"Monitor unavailable".** The card's output could not be opened for monitoring (often another program has it). Turn
Monitor off, or close the other program. Recording is not affected.

**Lost audio.** A red `lost 0.3 s` in the strip means the card or the SD card could not keep up, and a short notice
*Some audio was lost: the card or the disk could not keep up* appears. Try a faster SD card, a different
USB port or cable, or less load on the board.

**Hiss and noise from a cheap headset.** The card's amplifier makes noise at high gain, and SOFT amplifies it as
well. First raise CARD, then use SOFT only as much as you need. The headsets used with Cassette so far give a weak
level (peaks around -17 to -7 dBFS at full card level in the first tests), so speak close to the microphone. *Low cut
80 Hz* removes the low rumble, not the hiss.

**A 16 kHz headset.** A USB headset in its call profile can offer only 16000 Hz (for example the Jabra Evolve 65).
Cassette records at the card's own rate: 48000 Hz if it offers it, else 44100 Hz, else the highest rate at or below
48000 Hz. The strip then reads `16k/16 WAV`, Settings > Format reads `WAV 16 kHz 16-bit`, and playback runs at the right
speed on other outputs too. A 16 kHz take sounds like a phone call: that is the card, not the app.

**"Unsupported card".** The card offers neither 16-bit nor 24-bit capture. It is not usable.

**"This take cannot be played" / "damaged".** The file is not a valid PCM WAV file. Copy it off the board and
check it on a PC.

**Settings were not saved.** The notice *The settings could not be saved: ...* names the reason (a read-only or full
card). Delete `~/.config/cassette/settings.json` to start with the defaults.

## Limits

Not in this version (0.1.0):

- **WAV only.** FLAC and Opus are planned; today the Format row only shows the take's rate, depth and channels.
- **No Bluetooth audio.** Cassette records from sound cards that Linux presents as capture devices (USB, or a board's
  own input). It has no Bluetooth audio code. (A Bluetooth *keyboard* works as usual.)
- **No recording in the background.** Leaving the app stops the take (and keeps it).
- **No merge.** Trim and split cut a take into parts; there is no way to join takes in the app.
- The record timer makes no sound.
- Playback plays the app's own WAV takes.
- Not yet measured on the boards: how much the turning hubs cost a Pi Zero 2 W, and how every card behaves. The
  board test list is in [`../tests/BOARD-TESTS.md`](../tests/BOARD-TESTS.md).
