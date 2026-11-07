// The parts of lib/pulleys_mesh the game engine touches. Sending prints.
#pragma once
#include <Arduino.h>

namespace pulleys {
static constexpr uint8_t CHANNEL_COUNT        = 32;
static constexpr uint8_t MESH_CUE_NAME_MAX    = 63;
static constexpr uint8_t MESH_CUE_FLAG_ENTER  = 0x01;
static constexpr uint8_t MESH_COMPONENT_COUNT = 8;
static constexpr uint8_t MESH_COLOR_UNKNOWN   = 0xFF;
static constexpr uint8_t MESH_COLOR_COUNT     = 6;
static constexpr uint8_t MESH_FLOOD_MAX_BODY  = 241;
enum : uint8_t { MESH_MSG_GAME_INFO = 0xA2, MESH_MSG_GAME_PARAM = 0xA3 };

struct MeshEvent { uint8_t channel = 0, mode = 0, magnitude = 0, flags = 0; uint16_t originId = 0; };
struct MeshConfigRow { uint16_t id; uint8_t channel; };

bool mesh_send_cue(const char* name, uint8_t flags = 0);
bool mesh_send_flood(uint8_t msgType, const void* body, uint8_t len, uint8_t copies = 6, uint16_t gapMs = 160);
void mesh_send_components(const uint8_t* charge, const uint8_t* color, uint16_t decay, bool urgent);
}  // namespace pulleys
