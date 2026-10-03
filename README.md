# 🐍 Cyber Snake: Neon Garden

**Cyber Snake: Neon Garden** is a visually stunning, cyberpunk-themed 2D Snake game built using C++ and the SDL2 library. 

Forget the boring green blocks—this version features a neon-drenched aesthetic, procedural sound effects, and a high-energy atmosphere.

## ✨ Features
- **Cyberpunk Visuals**: Neon glow effects, CRT scanlines, glitch text, and a procedurally generated city skyline background.
- **Dynamic Gameplay**: Multiple speed settings (Slow, Normal, Fast, Insane) and customizable snake skins (White, Green, Blue).
- **Integrated Audio**: Fully procedural music and sound effects—no external `.wav` or `.mp3` files required.
- **Variety of Fruit**: Eat different types of fruits (Apple, Cherry, Banana, Grape, Orange) for varying point values.
- **Smooth Experience**: Fullscreen support and responsive controls.

## 🎮 Controls
- **Movement**: `W` `A` `S` `D` or **Arrow Keys**
- **Pause**: `P` or `Space`
- **Menu/Back**: `Esc`
- **Confirm/Play Again**: `Enter`
- **Fullscreen**: `F11`

## 🚀 Getting Started

### Prerequisites
To compile and run this game, you need the **SDL2** development libraries installed on your Linux system.

**On Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install libsdl2-dev
```

**On Fedora:**
```bash
sudo dnf install SDL2-devel
```

### Compilation
Use `g++` to compile the source code. Link the SDL2 library using `-lSDL2`.

```bash
g++ snake.cpp -o snake -lSDL2
```

### Running the Game
```bash
./snake
```

## 🛠️ Technical Details
- **Language**: C++
- **Graphics API**: SDL2 (Hardware Accelerated)
- **Audio**: SDL2 Audio Callback (Procedural Waveform Generation)
- **Rendering**: Custom neon bloom and glow shaders implemented via texture blending.
