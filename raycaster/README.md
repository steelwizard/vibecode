# Simple Raycaster

This directory contains a small first-person raycaster engine built with plain JavaScript and HTML canvas.

## Features

- Grid-map raycasting with DDA stepping.
- First-person wall rendering with simple distance shading.
- Improved movement + collision radius.
- Minimap with player direction.
- Enemies rendered as depth-sorted sprites.
- Shooting (hitscan) with enemy damage and hit feedback.
- Enemies chase the player when they see you and shoot from range.
- Multiple maps with automatic progression.
- Multiple wall tile types with different colors.
- Health pickup items that restore HP.
- Player health, damage flash, death screen, and win screen.

## Controls

- Move forward/backward: `W` / `S` or `Up` / `Down`
- Strafe: `A` / `D`
- Turn: `Q` / `E` or `Left` / `Right`
- Shoot: `Space` or left mouse button
- Restart after win/death: `R`

## Run

Open `index.html` in a browser, or run a static server from this folder:

```powershell
python -m http.server 8080
```

Then visit [http://localhost:8080](http://localhost:8080).
