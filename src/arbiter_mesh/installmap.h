#pragma once
#include "mapping_session.h"
#include <pulleys_mesh.h>

MappingSession& installmap_state();
void installmap_tick();
bool installmap_on_event(const pulleys::MeshEvent& ev);
// Preflights capacity before changing any sensor. The caller checks ownership.
bool installmap_assign(uint8_t channel);
bool installmap_open(uint64_t owner, uint16_t bridge);
bool installmap_close(uint64_t owner, uint16_t bridge);
void installmap_reset();
