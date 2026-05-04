# PlatformerDX

A small 2D platformer final-project game built with C++ and Direct2D, part of the DirectX family.

## Features

- Direct2D rendering through a Win32 game window
- Fixed-timestep game loop
- Player movement, double jumping, gravity, and tile collision
- Coins, checkpoint respawn, enemies, spikes, moving platforms, and a locked exit
- Three rounds with different colors, layouts, and increasing enemy speed
- Round timer with per-round best-time tracking
- Text level loading from `levels/level1.txt`
- Camera follow and HUD

## Level Symbols

- `#`: solid block
- `P`: player spawn
- `C`: coin
- `E`: enemy
- `K`: checkpoint
- `^`: spike hazard
- `M`: moving platform
- `X`: exit door

## Controls

- `A` / `D` or Left / Right: move
- `Space`, `W`, or Up: jump; press again in the air to double jump
- `R`: restart after game over or win
- `Esc`: quit

## Build

Open `PlatformerDX.vcxproj` in Visual Studio and build the `x64` Debug configuration.

You can also use CMake from a Visual Studio developer prompt:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

The Visual Studio project sets the debugger working directory to the project folder so the game can load `levels/level1.txt`.
