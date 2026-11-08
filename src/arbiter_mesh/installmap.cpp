#include "installmap.h"
#include "installcfg.h"
#include <Arduino.h>

MappingSession& installmap_state() { static MappingSession session; return session; }
void installmap_tick() {
    auto& session = installmap_state();
    bool active = session.active;
    auto phase = session.phase;
    session.tick(millis());
    if (active != session.active || phase != session.phase) installcfg_note_mapping_change();
}
bool installmap_open(uint64_t owner, uint16_t bridge) {
    installmap_tick();
    bool active = installmap_state().active;
    if (!installmap_state().open(owner, bridge, millis())) return false;
    if (!active) installcfg_note_mapping_change();
    return true;
}
bool installmap_close(uint64_t owner, uint16_t bridge) {
    if (!installmap_state().close(owner, bridge)) return false;
    installcfg_note_mapping_change(); return true;
}
void installmap_reset() { installmap_state().reset(); installcfg_note_mapping_change(); }
bool installmap_on_event(const pulleys::MeshEvent& ev) {
    installmap_tick();
    auto& session = installmap_state();
    if (!session.active) return false;
    if (ev.originType == pulleys::MESH_ORIGIN_SENSOR && pulleys::mesh_event_is_pull(ev)) {
        auto phase = session.phase;
        uint8_t count = session.count;
        session.pull(ev.originId, millis());
        if (phase != session.phase || count != session.count) installcfg_note_mapping_change();
    }
    return true;
}
bool installmap_assign(uint8_t channel) {
    installmap_tick();
    auto& session = installmap_state();
    if (!session.active || session.phase != MappingSession::PICK || !session.count || channel < 1 || channel > 24) return false;
    uint8_t added = 0;
    for (uint8_t i = 0; i < session.count; ++i)
        if (installcfg_channel_of(session.ids[i]) < 0) ++added;
    if (installcfg_row_count() + added > pulleys::MESH_CONFIG_MAX_ROWS) return false;
    for (uint8_t i = 0; i < session.count; ++i)
        installcfg_set_channel(session.ids[i], channel);
    session.assigned = channel;
    installcfg_note_mapping_change();
    return true;
}
