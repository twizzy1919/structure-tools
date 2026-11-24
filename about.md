# Structure Tools

Editor tools that build level structure for you.

## Auto

Open the **Edit** tab and press **Auto**.

- Nothing selected: the whole level is scanned.
- Something selected: only the selection is structured.

**Style: Pillars.** Each platform gets a pillar with the correct line blocks. Wide platforms get line blocks on the outer sides and a plain filler block in the middle. The direction is read from the lines on each block: a top line grows the pillar down, a bottom line grows it up.

Pillars stop after *Max Pillar Height* blocks (default 5), at the ground (down only), or at the first object in the way. Ctrl+Z undoes a whole run in one step.
