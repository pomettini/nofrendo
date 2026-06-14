# FamiCrank for Playdate

A port of the [Nofrendo](https://en.wikipedia.org/wiki/Nofrendo)
[NES](https://en.wikipedia.org/wiki/Nintendo_Entertainment_System) emulator
to the Panic® [Playdate®](https://play.date/). Plays NES games (and homebrew).
Mostly full speed, with a few slowdowns and minor glitches on demanding games.

**ROMs are not included.**

## Install

1. Download `FamiCrank.pdx` from this page.
2. Sideload via [play.date/account](https://play.date/account/sideload/).

## Adding ROMs

1. Launch FamiCrank once. If you have no ROMs yet, you'll see a message with the folder path. The folder has been created.
2. Connect your Playdate to your computer via USB.
3. On the Playdate, go to **Settings → System → Reboot into Data Disk Mode**. The device mounts as a USB drive on your computer.
4. Copy your `.nes` ROM files into the `Games/Shared/Emulation/nes/games/` folder on the mounted drive.
5. Eject the drive and reboot the Playdate.

## Saves

FamiCrank does not support saving (yet).

## Controls

| Playdate            | NES    |
| ------------------- | ------ |
| D-pad               | D-pad  |
| A                   | A      |
| B                   | B      |
| Crank pointing up   | Select |
| Crank pointing down | Start  |

## Options (found in Playdate system menu)

- **Frameskip**: `Auto` (default) keeps things as smooth as possible and adjusts on its own. You can also pick a fixed amount if you prefer.

## Credits

- **Nofrendo core**: Matthew Conte (1998–2000).
- **NES port base**: [nwagyu](https://github.com/nwagyu/nofrendo).
- **Playdate port**: [Giorgio Pomettini](https://www.giorgiopomettini.eu/).
- **Graphical assets**: [Noemi Frulio](https://noemifrulio.itch.io/).

Source code, roadmap and dev notes:
[github.com/pomettini/nofrendo](https://github.com/pomettini/nofrendo).

## License

[GPLv3](https://www.gnu.org/licenses/gpl-3.0.html). See `LICENSE` inside the `.pdx` bundle.

## Legal

FamiCrank for Playdate is a fan project. It is **not affiliated with, endorsed
by, or sponsored by Nintendo® Co., Ltd.** Nintendo® and Nintendo Entertainment
System® are trademarks of Nintendo; all rights belong to their respective
owners. Playdate® is a registered trademark of Panic® Inc., with whom this
project is also unaffiliated.

---

This Playdate port was developed with assistance from generative AI,
specifically [Claude Code](https://claude.com/claude-code). Use of generative AI in the upstream Nofrendo emulator has not been disclosed.
