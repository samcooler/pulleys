// Drives the arbiter's game engine from stdin. See README.md.
#include <Arduino.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <pulleys_channel.h>
#include "components.h"
#include "gamestate.h"

void stubs_reset(uint32_t seed);

static void loopPass() {   // main.cpp's loop, the parts the game has
    gamestate_tick();
    components_tick();
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "load") {
            std::string path;
            uint32_t seed = 1;
            in >> path >> seed;
            stubs_reset(seed);
            std::ifstream f(path);
            std::stringstream body;
            body << f.rdbuf();
            components_init();
            String why;
            if (!gamestate_load(String(body.str()), why)) printf("err %s\n", why.c_str());
            loopPass();
        } else if (cmd == "pull") {
            int ch;
            in >> ch;
            pulleys::MeshEvent ev;
            ev.channel = (uint8_t)ch;
            components_on_mesh_event(ev);
            loopPass();
        } else if (cmd == "wait") {
            long ms;
            in >> ms;
            for (long t = 0; t < ms; t += 10) {
                g_millis += 10;
                loopPass();
            }
        } else if (cmd == "input") {
            std::string name;
            in >> name;
            gamestate_input(name.c_str());
            loopPass();
        } else if (cmd == "param") {
            std::string name;
            float v;
            in >> name >> v;
            printf("%s\n", gamestate_set_param(name.c_str(), v) ? "ok" : "no");
            loopPass();
        } else if (cmd == "state") {
            printf("state %s\n", gamestate_state());
        } else if (cmd == "goals") {
            printf("goals");
            uint8_t mask = components_goal_mask();
            for (uint8_t i = 0; i < 5; i++)
                if (mask & (1u << i)) printf(" %s", pulleys::color_name(gamestate_major_color(i)));
            printf("\n");
        }
        printf(".\n");
    }
    return 0;
}
