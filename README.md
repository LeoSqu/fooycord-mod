# Fooycord Geode mod

v0.1 does one thing: links your Geometry Dash account to your fooycord account from inside the game.

## Use

1. In fooycord, Settings, Profile, Geometry Dash account: type your GD username, hit Find. You get a code like `fooy-1b645e`.
2. In Geometry Dash, tap the green speech-bubble button on the main menu, type the code, hit Link.
3. fooycord picks it up within a few seconds.

The mod reads your account id and username from the game's own account manager, so there is nothing to type except the code.

## Server URL

Mod settings (the gear on the mod in Geode's mod list) has one setting, `fooycord server`. Default is `http://localhost:3000`, which is right when the server runs on the same PC. Point it at the real URL once fooycord is hosted.

## Building

Built by GitHub Actions with Geode's official build action (`.github/workflows/build.yml`). Every push to `main` produces a `fooycord.geode` artifact. Download it, drop it in `Geometry Dash/geode/mods/`, restart the game.

To build locally you need CMake, Visual Studio Build Tools (C++ workload), the Geode CLI and `geode sdk install`. Then:

```
geode build
```
