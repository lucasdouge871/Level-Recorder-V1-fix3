# v1.3.0

- **Slopes**: detected from the level and treated as real triangles. Cube/robot ride up them, get launched off the top, stick to downhill slopes and die on the wall side. The flying modes slide over them. Slope tilt comes from the object's flips and rotation.
- **Orbs**: all orb kinds are detected and drawn in their own colour. Yellow, pink and red orbs are planned with and clicked by the cube/robot, including clicks in mid-air.
- Jumps are planned from the level's real ground, so dropping off a block edge is simulated correctly.
- Overlay draws slopes as triangles and orbs as coloured circles.
- Gravity, green, black, dash and spider orbs are detected but not clicked yet.

# v1.2.0

- Auto Path now supports **ship, UFO, wave, swingcopter, ball, spider and robot** as well as cube.
- Cube / robot: the bot first checks what happens if it does nothing. **If a jump pad is next to a spike or block it no longer auto clicks** and lets the pad do the work. Spikes placed after a pad are still jumped.
- More hazard and block types: saws / animated hazards, ceiling spikes (upside-down gravity), spikes on blocks, blocks of any height.
- Flight modes plan with a look-ahead search over the level's real hitboxes and re-plan every few frames.
- The bot measures the real physics of each gamemode in-game (gravity, ufo tap strength, wave slope, floor and ceiling) and corrects its predictions.
- The predicted path of the hitbox trail now works in every gamemode. Jump pads are drawn in yellow.
- The bot releases the button at the start of every attempt so it never starts a run stuck holding.

# v1.1.0

- Auto Path now plans every jump from the level's real hitboxes: it simulates the player's hitbox (outer + inner) through a jump and picks the middle of the widest safe window, instead of using fixed distances.
- Spike groups (double/triple spikes) and landing on blocks are handled by the simulation.
- New hitbox overlay: level hitboxes, your hitbox and the small inner hitbox.
- New hitbox trail: where your hitbox has been, plus the predicted jump path.
- Show Hitboxes / Hitbox Trail toggles in the Auto Bot panel and in the mod settings.

# v1.0.0

- First release.
- Auto Macro Bot panel in the pause menu.
- Auto Path: look-ahead auto-clicker that learns from deaths (cube/robot style sections).
- Record, Replay, Save, Load, Clear.
- [Replay] button in the pause menu.
- Safe Mode (on by default) and in-game HUD.
