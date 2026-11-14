// Downloadable content packages (dlc.cpp).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

std::vector<std::array<uint8_t, 0x134>> DlcList();                // XCONTENT_DATA for each marketplace package
bool DlcMount(const std::string& name, std::string& directory);   // unpacks once; the folder to serve
