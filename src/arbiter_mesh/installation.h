#pragma once

// Remote installation control. All handlers run from mesh_poll/the main loop;
// responses are paced so they cannot fill the mesh transmit queue at once.
void installation_init();
void installation_tick();
