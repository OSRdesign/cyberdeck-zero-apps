# Home Assistant: board tests (owner protocol, about 10 minutes)

Phase 1, version 0.1.0 (draft). The owner runs these on the deck (Pi Zero 2 W, 640x480, Bluetooth keyboard and
touch) against the real Home Assistant (`http://homeassistant.local:8123`, 2026.10.4). Nothing here was run on a board
or against a real Home Assistant before this protocol: the app was tested against a simulated and a fake server only.

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
