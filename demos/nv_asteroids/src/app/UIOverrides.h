#pragma once

#include <string>

struct UIData;

// Developer aid (deviation, not in the binary): "-set field=value" changes a UIData setting from the command line
// (several allowed), e.g. "-set enableFog=0 -set shadowLodBias=-1.5". Returns false (and logs) for an unknown name or
// an unparsable value. Only plain bool / int / float settings are listed.
bool ApplyUIOverride(UIData& ui, const std::string& assignment);
