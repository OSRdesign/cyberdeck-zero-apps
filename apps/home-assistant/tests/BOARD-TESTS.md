# Home Assistant: board tests (owner protocol)

## 0.2.0 (draft, study 053 P1): the tile dashboard, about 10 minutes

Two boards: one with a **keyboard** (the deck, Pi Zero 2 W, 640x480: K steps) and one **touch only** (the Pi 3A+ or
zero8: T steps). Against the real Home Assistant (`http://homeassistant.local:8123`, 2026.10.4), signed in as `deck`
(non-admin). Nothing here was run on a board or against a real Home Assistant before: the app was tested against a
simulated and a fake server only, in the PC harness.

Install over 0.1.0: copy `home-assistant_0.2.0_arm64.deb` to the board, then `sudo dpkg -i home-assistant_0.2.0_arm64.deb`.
The sign-in of 0.1.0 is kept (`~/.config/home-assistant/`): no new QR code. On a board without 0.1.0, pair first as in
0.1.0's T1 below. Remove with `sudo dpkg -r home-assistant`.

Before: in HA, have one dimmable light, one lock (or garage door), one scene, and two or three of them starred on the deck
(0.1.0's "Add to favourites", or the entity page's row in 0.2.0).

### P1. The dashboard (2 minutes, both boards)

1. Open **Home Assistant**. Expected: under the header a bar **Favourites | <your areas> | All**, and at its right a green
   dot and **Online**. Under it, your favourites as tiles: an icon in a circle at the left, the name, the state word in grey,
   and a small `›` at the top right on the tiles whose page has more than the icon's action (a dimmable light, a cover, a
   sensor, a thermostat; not a plain switch, scene or lock). A light that is on: its circle tinted amber, "On, 72 %"; off: grey, "Off" and the bulb
   glyph crossed. A lock: green "Locked". Columns: 2 on the deck; on the 3.5" Pi 3A+ panel 3, on an 800x480 panel 3.
2. A dimmable light has a full-width row under its row of tiles: its name, a slider, "72 %".
3. Do the icons look like the ones in Home Assistant's app for the same entities? Note any tile whose icon differs or is a
   letter (a letter means the icon name was not found).

### P2. Toggle and slider (2 minutes)

- K: press **Down** once (the first tile gets a blue outline and a lighter ground; the first key only shows the focus).
  Move with the arrows to a light, **Enter**: the word shows "..." then the new state; the lamp follows. Move **Down** from
  a dimmable light: the focus goes into its slider row; **Right** / **Left** change it by 10 %; the lamp follows within
  about a second. **Up** goes back to the light.
- T: tap a light's **icon**: it toggles ("..." then the state). Tap the slider row right of the knob: +10 %; drag the knob:
  the level follows the finger, the lamp a moment after you stop. Tap a tile's **name**: its page opens (Esc back).
- Tap or Enter a **scene**'s icon: "Done" for a moment; the scene runs.

### P3. Lock and garage confirmation (1 minute)

1. K: focus the lock's tile, Enter. T: tap the lock's icon. Expected: a box "Unlock Front door?" Yes / Cancel. Cancel:
   nothing happens.
2. Again, Yes: "..." then "Unlocked" (red circle). Lock it again the same way ("Lock Front door?").
3. A garage, gate or door cover: its icon asks "Open Garage door?" the same way.

### P4. Tabs and details (2 minutes)

1. K: **Page Down** / **Page Up** switch views; or **Up** from the first row (the active tab gets the outline), then
   **Left** / **Right**. T: tap an area's tab; or swipe sideways over the tiles. Each area shows its entities as tiles.
2. **All**: 0.1.0's list (FAVOURITES, ROOMS, Settings). Open Settings: "Tile action: Enter", "Tile page: Ctrl+O".
3. K: on a tile, **Ctrl+O** opens its page (only the keyboard can test this one). The page: the icon at the top, the word
   and "<area> - changed HH:MM", then right under it 7 mm rows (no gap): Brightness (a dimmable light; label whole,
   Left / Right 10 %), Turn on / off (or Lock / Unlock, Activate, Run, Open / Close) with a dot, Add to / Remove from
   favourites. Up / Down move, Enter acts, Esc back. T: tap a row to select it, tap it again to act; drag the slider.
   Star one entity there, Esc: it is a tile in Favourites now.

### P5. Offline and a start without a connection (2 minutes)

1. On Favourites, turn the board's Wi-Fi off. Expected within about 45 s: the bar says "Reconnecting..." (amber), the tiles
   go grey with their last words; after about 15 s more "Offline HH:MM" (red). No spinner.
2. With Wi-Fi still off, quit (Esc) and open the app again. Expected: the same tabs and tiles at once, grey, "..." as
   words, "Connecting..." then "Offline".
3. Wi-Fi on. Expected: "Online" within a minute, live words and colours back.

### What to send back (0.2.0)

Per step: pass, or what you saw instead (a photo helps for the look). Which tiles' icons differ from Home Assistant's app.
How the deck feels when switching tabs (any delay you notice). Optional:
`grep VmRSS /proc/$(pgrep -x home-assistant)/status` on Favourites and on your largest area (target under 25 MB), and
`ls -l ~/.config/home-assistant/` (`layout.json` and `names.json` are `-rw-------`).

---

## 0.1.0: the first protocol (pairing, lists, sign out; still valid)

Phase 1, version 0.1.0 (draft). The owner runs these on the deck (Pi Zero 2 W, 640x480, Bluetooth keyboard and
touch) against the real Home Assistant (`http://homeassistant.local:8123`, 2026.10.4). In 0.2.0 the 0.1.0 Home list is
the **All** tab, so "Home" below means All.

Install: copy `home-assistant_0.1.0_arm64.deb` to the deck, then `sudo dpkg -i home-assistant_0.1.0_arm64.deb` (it
depends on `libcurl4t64`, which the boards already have through `curl`; `sudo apt -f install` if dpkg says it is
missing). Remove afterwards with `sudo dpkg -r home-assistant`.

## T0. Prepare Home Assistant (on the PC, 2 minutes)

1. HA > Settings > People > Users > Add user: name `deck`, **Administrator off**, "Can only log in from the local
   network" on. Give it a password.
2. Star nothing yet. Note one light (ideally dimmable), one scene, one lock or a lock-like test entity, and one room
   (area) that holds them.

## T1. Pair by QR (2 minutes)

1. Open the **Home Assistant** tile. Expected: the Connect page: "Scan with your phone to connect to Home Assistant",
   a QR code (left; on top in PDA mode), `http://homeassistant.local:8123`, the row `Server  homeassistant.local >`, and "Waiting for the phone...".
2. Scan the QR code with the phone (same Wi-Fi). Expected: Home Assistant's login page on the phone. Log in as `deck`.
3. Expected on the phone: "Home Assistant answered the deck. You can close this page and look at the deck."
   On the deck: "Signing in...", then "Connected as deck" (green), then Home by itself after about 1.5 s.
4. If the phone cannot open the page: on the deck, open the Server row, type the HA IP address with its port
   (`192.168.68.178:8123`), Enter, and scan again. Tell me which.

Check over ssh: `ls -l ~/.config/home-assistant/` shows `credentials.json` with `-rw-------` (do **not** cat it: it
holds the sign-in), and `journalctl --user -u <launcher service> | grep home-assistant` shows no token.

## T2. Home, a room, favourites (3 minutes)

1. Home shows FAVOURITES ("None yet: open a device and add it"), then ROOMS with a count per room, then Settings. The
   line under the title says **Connected** (green).
2. Open your room (tap it twice, or arrows and Enter). Expected: its lights, switches, scenes, sensors ... with their
   state at the right; action rows carry a dot, sensors are grey, covers a chevron.
3. Tap the light's name twice (or Enter): it toggles; the row shows "..." then the new state. Check the lamp.
4. Press Right on the light (or tap twice on its state, right side): the entity page. Move the Brightness slider with
   Left / Right (10 % steps): the lamp follows within a second.
5. Down to "Add to favourites", Enter. Esc twice: the light is under FAVOURITES on Home.
6. In the room, run the scene (tap twice): the row shows "Done" for a moment; the scene runs.
7. With the keyboard, type a few letters in the room: the list filters ("FILTER: ..."); Esc clears the filter.

## T3. A lock (1 minute)

1. Tap the lock twice (or Enter). Expected: a box "Unlock Front door?" with Yes / Cancel. Cancel: nothing happens.
2. Again, Yes: the lock unlocks; the row says "Unlocked". Lock it again the same way ("Lock Front door?").

## T4. Offline and back (2 minutes)

1. On Home, turn the deck's Wi-Fi off (launcher Settings, or `nmcli radio wifi off` over a cable/serial session).
   Expected within about 45 s: "Reconnecting..." (amber), the values greyed with "last update HH:MM"; after three
   failed tries (about 15 s more) "Offline" (red). No spinner.
2. Wi-Fi on again. Expected: "Connected" within at most a minute (retries 2, 4, 8 ... 60 s apart), live values back.

## T5. Sign out, and the token gone in HA (1 minute)

1. Home > Settings: Server and Signed in as `deck`. Sign out, Yes. Expected: the Connect page with "Signed out.
   Scan the code to sign in again." and a new QR code.
2. On the PC: HA, log in as `deck` > profile (bottom left) > Security > Refresh tokens: the entry named
   `http://<deck address>:<port>/` is **gone**. Over ssh: `~/.config/home-assistant/credentials.json` is gone.
3. Esc leaves the app.

## Optional (if time)

- Revoke from HA instead (pair again, then delete the refresh token in HA's profile while the app is open): the app
  goes back to the Connect page with "Home Assistant refused this deck's sign-in ..." or "... no longer accepts ...",
  and never retries by itself.
- `grep VmRSS /proc/$(pgrep -x home-assistant)/status` on Home with a few favourites (target under 25 MB).

## What to send back

For each T: pass, or what you saw instead (the exact status line text when something fails). The phone browser and
its result at T1.2 / T1.3. Whether `homeassistant.local` worked from the phone.
