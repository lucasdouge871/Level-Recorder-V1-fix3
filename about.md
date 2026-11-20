# Auto Click Mod

**By Lucasdouge**

The **Auto Macro Bot** for Geometry Dash. It plays a path through your level, writes that path down, and lets you watch it run again with one button.

## How to use it

1. Start a level and open the **pause menu**.
2. Tap **Auto Bot** (bottom-left) to open the Auto Macro Bot panel.
3. Turn on **Auto Path**, then restart the attempt. The bot clicks for you and builds a path.
4. When the level is finished, the path is saved automatically.
5. Any time later, tap **Replay** in the pause menu to watch the bot run the saved path from the start.

## Features

- **Auto Path** - the bot reads the **real hitboxes** of every spike, saw, block and jump pad in the level and plans from those. It supports **cube, robot, ship, UFO, wave, swingcopter, ball and spider**.
  - *Cube / robot*: simulates your hitbox through a jump from many start points and presses jump in the middle of the widest safe window. It first checks what happens if it does **nothing**, so **when a jump pad is next to a spike or block it does not click** and lets the pad do the work. It learns from each death by nudging its timing.
  - *Ship / UFO / wave / swing / ball / spider*: every few frames it tries many click patterns against the level's hitboxes and keeps the one that survives longest with the most room to spare, then plans again. It **measures the real physics of each gamemode as you fly** (gravity, tap strength, wave slope, floor and ceiling) so it improves within a few seconds.
- **Slopes** - read as real triangles (tilt taken from the object's flips and rotation), not boxes. Cube/robot ride them, get launched off the top, stick to downhill slopes and die on the tall wall side; the flying modes slide over them.
- **Orbs** - all orb kinds are detected and drawn. **Yellow, pink and red orbs are used by the cube and robot**: the bot checks whether a jump needs an orb, and in mid-air a look-ahead search decides whether clicking it now is the way to survive (and double-checks that the orb really fired, retrying if not). Gravity, green, black, dash and spider orbs are drawn dimmer and are not clicked.
- **All kinds of hazards and blocks** - normal, double and triple spikes, saws and other animated hazards, spikes on blocks, ceiling spikes and blocks of any height.
- **Hitboxes + Hitbox Trail** - while the bot is active you can see the hitboxes it is using (red = hazards, blue = blocks, green = slopes, yellow = jump pads and your hitbox, coloured circles = orbs, cyan = the small inner hitbox that decides wall crashes), a fading trail of where your hitbox has been, and the path the bot plans to take (green = safe plan found, orange = best guess). Toggle both in the Auto Bot panel or the mod settings.
- **Record** - records your own clicks frame by frame so any gamemode can be replayed.
- **Replay** - plays the stored path back. Your input is locked while it runs.
- **Save / Load / Clear** - paths are stored per level in the mod's save folder.
- **HUD** - small status label while the bot is active.

## Safe Mode (on by default)

Runs that use the bot (Auto Path or Replay) are treated like test runs and **never save progress or completions**. You can switch this off in the mod settings, but please keep bot runs off the leaderboards.

## Notes

- Auto Path is a best-effort path finder, not magic. **Gravity / green / black / dash / spider orbs, orbs in the flying modes, slopes tilted at odd angles, moving or rotating hazards, triggers, dual mode and platformer mode are not handled** - in those parts you stay in control (or the bot may die there). Record your own run for them.
- The ship / UFO / wave / swing / ball / spider logic starts from estimated physics and corrects itself while flying, so the first second or two of each new section is the least accurate.
- Gravity pads and spider pads are left alone (the bot does not click near them).
- Turning the bot on mid-attempt gives a partial path. Restart the attempt after changing modes.
