# Controls

This is the authoritative default-control reference for the native PC port of
BattleTanx: Global Assault. All gameplay bindings can be changed from
**F10 → Controllers**.

## Keyboard and mouse

| PC input | N64 input / function |
|---|---|
| W / A / S / D | Analog stick: drive and steer |
| X | A button / primary fire |
| Z | B button |
| Left Shift | Z trigger |
| Enter | Start / pause |
| Q / E | L / R shoulder |
| Arrow keys | D-pad |
| I / J / K / L | C-up / C-left / C-down / C-right |
| F10 | Open or close the native PC settings overlay |

Keyboard input is combined with the first assigned controller, so a player may
switch between or simultaneously use keyboard, mouse, and controller input.

## Controller

SDL-compatible game controllers are assigned to N64 controller ports in
discovery order. The defaults are:

| Controller input | N64 input / function |
|---|---|
| Left stick | Analog stick: drive and steer |
| A button | N64 A / primary fire |
| X button | N64 B |
| Left trigger | N64 Z trigger |
| Start | Start / pause |
| Left / right shoulder | N64 L / R |
| D-pad | N64 D-pad |
| Right stick | C buttons |

The port supports up to four controllers, per-player device assignment,
deadzones, sensitivity, axis inversion, rumble strength, and complete
controller rebinding in **F10 → Controllers**.

### Measured generic N64 USB pad on Windows

For the red transparent `0079:0006` pad with the raw layout below, download
[generic-n64-windows.txt](controllers/generic-n64-windows.txt) beside
`BattleTanxGA.exe`, then launch from that folder in PowerShell:

```powershell
$previousProfile = $env:SDL_GAMECONTROLLERCONFIG_FILE
$env:SDL_GAMECONTROLLERCONFIG_FILE = (Resolve-Path '.\generic-n64-windows.txt').Path
try {
    & '.\BattleTanxGA.exe'
} finally {
    $env:SDL_GAMECONTROLLERCONFIG_FILE = $previousProfile
}
```

An existing SDL_GAMECONTROLLERCONFIG mapping can override this file; remove
that competing value from the launch shell if needed.

This uses the existing default bindings: physical A/B become SDL A/X, L/R
become SDL shoulders, Z becomes left trigger, C buttons become signed
right-stick axes, and the stick and D-pad retain their roles. If you changed
bindings, check them in **F10 → Controllers**. Use **F10** for settings; this
pad has no measured Back/Select button and Z remains Z. Keep the independent
turret aiming mod off for the original C-button behavior: it also uses the
right stick and this profile supplies C buttons through those axes.

Only use the file if your zero-based raw inputs match: A/B/Start/Z/L/R =
buttons 5/4/9/8/6/7; C up/right/down/left = buttons 0/1/2/3; stick = axes 0/1;
D-pad = hat 0. Native Windows SDL reported 4 axes, 12 buttons and 1 hat.
Other clones share VID/PID and CRCs, so the profile is opt-in and is not added
to an automatic database. Launch normally without this variable to stop using
it. It does not change saved bindings, deadzones or system-wide settings.

Measurements supplied by MatyMeatBoy on 2026-11-11: user-recorded physical
assignments, native SDL identification and virtual SDL mapping checks. CRCs
`8630`, `8e4f`, `79be` were observed with SDL2 2.32.10, SDL2 2.26.3 and SDL3
3.4.18 respectively; the port itself uses SDL2. Mapping tests do not replace
physical gameplay validation in BattleTanx GA, which has not been performed
with this pad.

## Native settings menu

- Press **F10** to open the settings overlay.
- Press **F10** again, select **Close**, or select the window **X** to close it.
- After it closes, **F10** opens it again; it is a toggle, not a one-use key.

### Joining online play

Open **F10 → Online**, select **Refresh public lobbies**, then select **Join**
beside a listed dedicated server. Refresh and Join are explicit opt-in network
actions; merely opening F10 or the Online tab contacts nothing. The game
relaunches into an authoritative client and returns to the selected server with
the same seat after a recoverable disconnect. Players do not host matches from
this tab; a supervised dedicated authority owns the canonical game.
- Use **Apply & Save** to persist changed bindings and settings.
- Opening settings releases mouse capture, so it is safe to use while the
  independent-turret mod is enabled.

The overlay supports mouse, keyboard, and controller navigation. Its
**Controllers** tab contains keyboard and per-controller bindings. Its
**Mods** tab contains the optional gameplay changes.

## Independent turret aiming mod

This optional mod is disabled by default and only affects player-controlled
vehicles with rotating turrets.

Enable it from **F10 → Mods → Independent dual-stick turret aiming**. This is
also described as **Aim-Down-Turret** mode in the player-facing documentation.

### Mouse and keyboard

- Hold **right mouse button** to enter turret-aim mode and capture relative
  mouse movement.
- Move the mouse horizontally to rotate the turret.
- Press **left mouse button** to fire the primary turret weapon.
- Continue using **W A S D** to control the hull. While aim mode is held, W/S
  move along the hull heading and A/D turn the hull; aiming east does not force
  a north-facing hull to turn east.
- Release **right mouse button** to release the cursor and restore the selected
  retail movement behavior.

### Controller

- Use the **right stick** to rotate the turret.
- Use the **left stick** for hull movement.
- Right-stick turret sensitivity and horizontal inversion are configurable per
  player in **F10 → Controllers**.

Rounds and weapons fired from the turret follow turret aim. Mines, gun buddies,
bouncing betties, and other deployables retain their original behavior.

## Developer key

**F1 is not the normal settings key.** It opens RT64's renderer inspector only
when the game was launched with `BTGA_RT64_DEVELOPER=1`. Ordinary players should
use **F10** for settings.

## Restoring defaults

Open **F10 → Controllers** and use **Restore defaults**, then **Apply & Save**.
The default settings are stored per user; the game installation and ROM are not
modified.
