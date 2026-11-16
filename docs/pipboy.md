# Pip-Boy: quests, friends, mail

Server part in `engine/server/Pip.cpp`, protocol in `engine/common/protocol_game.h` (block "Pip-Boy"), window in `engine/client/OrigUiPip.cpp`.

## Protocol
* `C2S_Pip = 36` (the last free id of the 20..39 client range): `op u8, num u32, a string, b string`. Ops (`gnet::PipOp`): Request, FriendAdd (a = name), FriendDel (a), MailSend (a = recipient, b = text), MailRead (num = mail id), MailDelete (num).
* `S2C_Pip = 142`: the complete snapshot (quests, friends with online flag, mail). It answers every op and is also pushed unasked after a quest / friend / mail change, at login, and when a friend logs in or out. The client keeps only the newest (`ClientCore::pip()`, `hasPip()`).
* Requests count against the per-client budget like all other requests (10 points, +1 every 5 ticks, silently dropped when empty), tests that fire bursts must pause (`pace()` in `GameTest.cpp`).

## Data
* Quests (`id`, `title`, `text`, `state` 0 active / 1 done / 2 failed) and friends are part of the character (`Persist.cpp`), max 100 / 50.
* Mail is per account, table `mail` of the database (`id` database wide, `sender`, `body`, `sent_at`, `is_read`; `docs/persistence.md`), max 50 messages of 400 chars, so it reaches offline players. Sending is audited (`mail_sent`). Quests and friends are rows of `quests` / `friends` of the saved character. Guests have no mailbox and cannot send.
* Lua (`scripts/main.lua`, demo quests "First steps" / "Survivor"): `c:quest_add(id, title, text)`, `c:quest_done(id)`, `c:quest_fail(id)`, `c:quest_state(id)` (-1 unknown, 0, 1, 2).
* Chat commands: `/friend add|del <name>`, `/friends`, `/mail <name> <text>`, `/inbox`, `/maildel <id>`, `/quests`. At login the player is told about unread mail ("press P").

## Client window (key P, Esc closes, `--open-pip` for tests)
Drawn over `f3_pipboy.tga` (1024x609 layout units, scaled to the window, centred): detail screen (37,27)-(577,286), list screen (37,312)-(577,593), the four plates on the right are the tabs (positions measured from the texture): **Alarm** = friends, **Tasks** = quests, **Maps** = empty (no automaps in the demo), **Downloads** = mail (unread count in the label). Click a list row to select (selecting an unread mail marks it read); the detail screen shows the text and buttons: Mark read / Delete / Reply (opens the chat line `/mail <name> `) for mail, Remove friend / Write mail for friends. Mouse wheel scrolls the list. Adding friends and writing mail goes through the chat commands. Ops travel as `UiCommand::PipOp`.
