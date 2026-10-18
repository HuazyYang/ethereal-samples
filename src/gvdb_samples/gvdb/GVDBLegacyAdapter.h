// Transition adapter for existing host-side GVDB scenes. No device operations.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortableData.h"
namespace gvdb { struct IGVDBVolume; }
namespace gvdb::slang {
// Call after the host pools and atlas map have been finalized. If legacy GPU
// topology modified them, fetchPools must be completed before calling.
bool packLegacyVolume(IGVDBVolume& source, PackedVolume& output, std::string& error);
}
