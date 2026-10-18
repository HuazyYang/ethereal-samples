// Standalone portable-layout and pool-address validation; no graphics SDK needed.
// SPDX-License-Identifier: Apache-2.0
#include "../gvdb/slang/GVDBPortable.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace gvdb::slang;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        for (uint64_t index : {0ull, 1ull, 0xffffull, 0x10000ull, 0xffffffffull, 0xffffffffffffull}) {
            auto ref = makeReference(1, 9, index);
            require(referenceIndex(ref) == index && referenceLevel(ref) == 9 && referenceGroup(ref) == 1, "VBX reference encoding changed");
        }
        require(isNull({0xffffffffu, 0xffffffffu}), "null encoding");
        require(!isNull({0xffffffffu, 0}), "48-bit index must not be confused with 32-bit sentinel");
        LevelInfo pool{};
        pool.nodes = {128, 64, 4, 3};
        require(addressable(makeReference(0,0,3), pool, false), "last node is addressable");
        require(!addressable(makeReference(0,0,4), pool, false), "node beyond pool");
        require(!addressable(makeReference(0,0,0x100000000ull), pool, false), "serialized index truncation");
        pool.nodes = {0xffffffc0u,64,2,0};
        require(addressable(makeReference(0,0,0), pool, false), "exact 4-GiB endpoint");
        require(!addressable(makeReference(0,0,1), pool, false), "wrapped byte address");
        pool.nodes.y = 0;
        require(!addressable(makeReference(0,0,0), pool, false), "zero stride");
        require(scanBlockCount(0) == 0 && scanBlockCount(1) == 1 && scanBlockCount(256) == 1 && scanBlockCount(257) == 2 && scanBlockCount(UINT32_MAX) == 16777216u, "overflow-safe dispatch sizes");
        // Construct an on-disk node using byte offsets, independent of execution
        // layouts, then check that raw upload retains all VBX fields and masks.
        std::array<unsigned char,64> disk{};
        disk[0] = 3; disk[1] = 1; disk[2] = 7;
        const int32_t position[3] = {-17, 32, 91};
        std::memcpy(disk.data()+4, position, sizeof(position));
        const uint64_t parent = 0x0000123456780200ull;
        const uint64_t childList = 0x00000000000a0101ull;
        const uint64_t mask = 0x8000000000000001ull;
        std::memcpy(disk.data()+40, &parent, 8);
        std::memcpy(disk.data()+48, &childList, 8);
        std::memcpy(disk.data()+56, &mask, 8);
        SerializedNode node{};
        std::memcpy(&node, disk.data(), disk.size());
        require(node.words[0] == 0x00070103u && int32_t(node.words[1]) == -17, "VBX metadata or position offset");
        require(node.words[10] == uint32_t(parent) && node.words[11] == uint32_t(parent >> 32), "VBX parent offset");
        require(node.words[12] == uint32_t(childList) && node.words[14] == 1 && node.words[15] == 0x80000000u, "VBX child or mask offset");
        std::vector<UInt2> keys{{0xffffffffu,0},{0,1},{0,0},{0xffffffffu,0xffffffffu},{1,0}};
        std::sort(keys.begin(),keys.end(),keyLess);
        require(keys[0].x==0 && keys[0].y==0 && keys[2].x==0xffffffffu && keys[2].y==0 && keys[3].x==0 && keys[3].y==1, "unsigned 64-bit ordering");
        std::cout << "GVDB portable layout, reference, and address validation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
