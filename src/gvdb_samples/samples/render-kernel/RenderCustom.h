// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBSlangVolume.h>
namespace ethereal::slang
{
class ComputeRuntime;
}
gvdb::slang::RaycastOutput renderCustom(ethereal::slang::ComputeRuntime &runtime, gvdb::slang::SlangVolume &volume,
                                        gvdb::slang::RaycastParameters parameters);
