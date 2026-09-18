<p align="center">
    <a href="https://github.com/Nexia360/Nexia360">
        <img height="256px" src="assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Nexia360 - Xbox 360 Emulator</h1>

Nexia360 is an Xbox 360 emulator for Windows, focused on **online multiplayer**,
**controller-first navigation**, and **quality-of-life features** for playing on a
PC the way the console played on a TV.

It is a fork of [Xenia Canary Netplay](https://github.com/AdrianCassar/xenia-canary),
which is itself a fork of [Xenia Canary](https://github.com/xenia-canary/xenia-canary)
and the original [Xenia](https://github.com/xenia-canary/xenia) project.

Come chat with us on [Discord](https://discord.gg/4sx2MdcUk3).

Discussing piracy or other illegal activities will get you banned.

---

## Features

Beyond upstream Xenia Canary, Nexia360 adds:

* **Nexia Hub networking** — a hosted matchmaking service for online play, alongside
  the existing LAN, Xbox Live (via a self-hosted server), and offline network modes.
  The mode can be switched at runtime from the Netplay menu.
* **Full controller navigation** — the entire emulator UI, including profile
  creation, settings, and dialogs, is navigable with a gamepad. The guide button
  opens the profile manager (short press) or the profile editor (long press).
* **On-screen keyboard** — text entry with a controller anywhere the emulator asks
  for input, including guest titles that request the Xbox keyboard.
* **Voice chat** — headset voice over netplay, with a configuration UI under Console.
* **Mousehook** — mouse-and-keyboard control for supported titles, configured through
  a UI (`Ctrl+Shift+M`) and stored in `mousehook.json`. Toggle in-game with `Ctrl+M`.
* **Title Update manager** — install multiple title updates per game into a library
  and pick which one to apply, without re-installing content.
* **XNA / XBLIG titles run** — Xbox Live Indie Games are recompiled onto a managed
  host rather than interpreted, so they render, play sound and take input. The XNA
  runtime they need is installed for you from **File → XNA Titles → Setup XNA…**.
* **Seamless title transitions** — a title that launches another one switches in
  place, without restarting the emulator: game to dashboard, dashboard to the
  Avatar Editor, a demo to the full game. The previous title is torn down and the
  next one mounted in its stead, and the launch data it was given is carried
  across.
* **Avatars** — avatars render for XNA titles and for Xbox 360 games that ask XAM
  for them, and there is a built-in avatar editor. With the console's own system
  update installed, **Profile → Avatar Editor** runs the real Xbox 360 Avatar
  Editor instead, with the full asset catalogue: every garment, the face textures,
  and the chin, nose and ear shapes.

## Downloads

* [Latest release](https://github.com/Nexia360/Nexia360/releases/latest)
* [Development builds](https://github.com/Nexia360/Nexia360/actions) — latest
  work-in-progress builds. A GitHub account is required to download artifacts.

Nexia360 requires 64-bit Windows and a GPU supporting Direct3D 12.

## Quickstart

1. Download the latest release and extract `Nexia360.exe`.
2. Launch it and use **File → Open** to select a game, or drag a game onto the window.
3. For online play, choose a network mode from the **Netplay** menu.

Nexia360 does not supply games, and cannot be used to obtain them. You must provide
your own legally dumped copies of games you own.

## Configuration

Settings are stored in `nexia360.config.toml`, created next to the executable on
first launch. Most options are also available in-app under **Console → Settings**.

## Building

See [building.md](docs/building.md) for setup information. Nexia360 builds with
CMake and Ninja on Windows using Visual Studio's toolchain. When contributing code,
check the [style guide](docs/style_guide.md) and run clang-format.

## Contributing

**For general rules and guidelines please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).**

Fixes and optimizations are always welcome. Netplay compatibility reports are
especially useful — if a game works or fails online, let us know on Discord.

## Disclaimer

The goal of this project is to experiment, research, and educate on the topic of
emulation of modern devices and operating systems. **It is not for enabling illegal
activity.** All information is obtained via reverse engineering of legally purchased
devices and games and information made public on the internet.

## License

Nexia360 is licensed under the BSD 3-Clause license. See [LICENSE](LICENSE).
It contains code from Xenia, Xenia Canary, and Xenia Canary Netplay, and retains
their copyright notices.
</content>
