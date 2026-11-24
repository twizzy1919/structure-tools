# Structure Tools

Editor tools that build level structure for you.

## Menu

Open the **Edit** tab and press the small **Auto** button. The menu has:

- **Style**: Pillars, Enclose or Shapes.
- **Direction** (Pillars): Auto reads the lines on each block. Down or Up forces one direction.
- **Max height** (Pillars): how many blocks a pillar can have.
- **Structure**: runs the chosen style.
- **Show ID**: shows the object ID of the selected blocks.

## Pillars

Nothing selected scans the whole level, otherwise only the selection. Wide platforms get line blocks on the outer sides and a plain filler in the middle. 45° and 2x1 slopes are supported, and default blocks (ID 1) copy the direction of the blocks beside them. Lines are hidden where a pillar touches another block or slope.

## Enclose

Nothing selected scans the whole level; with a selection only the selection is enclosed. The mod adds the fewest blocks needed so every exposed edge has a line. Outer corners are randomly a block or a 45° slope (see *Corner Slope Chance* in settings). Structures that touch merge into one, and one-cell gaps between them are filled. Existing blocks whose lines would become seams are swapped for the right variant (press Ctrl+Z twice to undo those).

## Shapes

Same selection rules as Enclose. Every group of nearby blocks or slopes (within *Shape Link Distance* cells) gets one chunky blob around it: a padded rectangle with each corner cut by a diagonal of 45° slopes, filled with plain blocks and outlined with the right line pieces. Padding and cut sizes are random, so every run looks different. Run it on a fresh level: running it again wraps the old shapes in new ones.

Ctrl+Z undoes a whole run.
