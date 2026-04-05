# Manifesto: Pulleys

## On invisible threads

Bluetooth Low Energy is a whisper. A device broadcasts its name into the dark — not to anyone in particular, just *outward*. Another device, listening, picks up the signal and measures how faint it is: a proxy for distance. We never ask "how far away are you?" We ask "how loud is your voice?" and infer intimacy from volume.

This is how Pulleys knows when a traveler visits a station. Not through coordinates or clocks, but through the quality of a signal fading in and out. The participant holding a puppet doesn't see any of this. They pull a rope. Something glows.

## On culture as light

A culture is two colors and a rhythm. That's it — six bytes of pigment and one byte of pulse. A red-and-gold beetle flickering at 2Hz. A blue-and-white hexabot breathing slowly at 0.4Hz. These are not chosen from a menu. They are born, randomly, when a device wakes up. Each traveler enters the world with a culture it didn't choose, like the rest of us.

When a traveler visits a station, the station absorbs a fraction of the traveler's culture. Its colors shift. Its rhythm nudges. The station doesn't become the traveler — it becomes something new that remembers the traveler passed through. Over the course of a night, a station might blend dozens of visitors into a color and rhythm no one designed.

This is the core aesthetic: **emergence from accumulation**. No single interaction is dramatic. But the forest, over time, becomes a record of everyone who moved through it.

## On proximity as intimacy

We define three zones: FAR, NEAR, and CLOSE. Only CLOSE triggers culture exchange. This means a puppet must actually be pulled *to* the station, not merely past it. The participant must commit to the encounter — hold the rope, wait for the moment of closeness. The RSSI threshold is a physical fact (radio waves attenuating through air, trees, bodies) that we've dressed up as a social one.

There's hysteresis in the zone boundaries — a buffer that prevents flickering at the edges. You have to really arrive to be counted, and you have to really leave to be forgotten. This is a small technical choice that creates a feeling: visits are *events*, not accidents.

## On ritual (future)

The travelers carry accelerometers. A puppet held still, or shaken vigorously, or spun in circles — these are gestures with no inherent meaning. But we can assign meaning: a spin could amplify the culture exchange, a shake could mutate the colors, stillness could deepen the blend. The participant doesn't need to know the rules. They'll discover, through play, that how they hold the puppet matters. The mechanics are hidden. The magic is felt.

This is where the project tips from installation into game: when people start experimenting with how they interact, not just whether they interact.

## On the forest at night

Twenty stations hanging in trees, connected by rope. A hundred tiny puppets with glowing LED matrices, each a different creature, each carrying a culture expressed as pulsing colored light. People spread through the dark, pulling ropes, watching colors shift, slowly realizing that the patterns they see are a consequence of everywhere the puppets have been.

The ropes are physical. The cultures are electromagnetic. The experience is somewhere in between — in the hands pulling, the eyes watching, the slow recognition that you're participating in something that's been going on without you, and will continue after you leave.

The forest remembers. The stations remember. You won't.

## Technical choices as artistic choices

- **No names in BLE advertisements** — devices are identified by their pattern, not a label. You recognize a traveler by its colors, not its name. This is a deliberate choice: anonymity serves the metaphor of cultural mixing without identity.
- **Random seed on boot** — cultures aren't designed, they're born. Every power cycle creates a new culture. Nothing is precious. This is an installation, not an archive.
- **10% blend ratio** — a single visit barely changes a station. It takes many travelers to transform it. This paces the experience across the whole night, not a single moment. Tunable.
- **Per-pixel phase offset** — the LED pattern isn't a flat color, it's a wave. Movement across the surface of the light creates a sense of life, of breathing. Two static colors would be a flag. Two oscillating colors are an organism.
