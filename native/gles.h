#pragma once
#include <memory>
#include <string>

#include "gpu/backend.h"

// The GL ES 3.0 renderer (native/gles.cpp), the port's second backend. Created when the RENDERER setting asks for it and
// the driver gives a GL ES 3.0 context; nullptr there leaves the Vulkan backend to run as it always has.
std::unique_ptr<gpu::Backend> CreateGlesBackend();

namespace gpu {
// what the graphics report says when the GL ES renderer is the one drawing
void GlesGraphicsReport();
std::string GlesGraphicsInfo();
}  // namespace gpu
