# Structure Tools

Editor tools that build level structure for you.

## Menu

Open the **Edit** tab and press the small **Auto** button. The menu has:

- **Style**: Pillars or Enclose.
- **Direction** (Pillars): Auto reads the lines on each block. Down or Up forces one direction.
- **Max height** (Pillars): how many blocks a pillar can have.
- **Structure**: runs the chosen style.
- **Show ID**: shows the object ID of the selected blocks.

## Pillars

Nothing selected scans the whole level, otherwise only the selection. Wide platforms get line blocks on the outer sides and a plain filler in the middle. 45° and 2x1 slopes are supported, and default blocks (ID 1) copy the direction of the blocks beside them. Lines are hidden where a pillar touches another block or slope.

## Enclose

Select the blocks or slopes you want to close in, then press Structure. The mod adds the fewest blocks needed so every exposed edge has a line. Outer corners are randomly a block or a 45° slope (see *Corner Slope Chance* in settings).

Ctrl+Z undoes a whole run.
