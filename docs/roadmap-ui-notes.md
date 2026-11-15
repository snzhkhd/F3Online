# UI roadmap notes (future requests, not implemented)

Requests from the project owner, recorded in spirit; nothing here is built yet.

1. **Digit roller in character creation.** When a SPECIAL or skill number changes on the creation screen, the digits should
   animate like a roller / odometer (the original's digit windows) instead of switching instantly.
2. **Speech as floating text.** All NPC and player speech / messages appear as floating text above the heads, like the original
   game and like FOnline, not only in the message log.
3. **Fog of war with a visible border.** Like the original: the visible area needs a visible edge. The server already computes
   visibility, but the edge is not rendered on the client. For the MMO, consider a vision radius that depends on Perception.
4. **Object highlight / outline** on hover and selection (critters, items, containers, doors).
5. **The original's hotkeys** from `F3.ini` `[HotKeys]`: `S` = SneakToggle (stays as it is in our game: S currently opens the skill
   cursor), `U` = UseSkill (planned for a skill panel), `T` = WeaponLinkToggle, `W` = WeaponToggle, `A` = AttackModeSelect,
   `Ctrl+A` = AttackModeCycle, `C` character, `I` inventory, `P` = PipBoy, `Esc` menu.
6. **Move the whole UI onto the original `.gui` layouts** (see `docs/formats/gui.md` for the status of each layout).
7. **Original game in a window:** set `fullscreen = 0` in `Documents\F3\F3.ini`, section `[Graphics]` (`width` / `height` are there too).
