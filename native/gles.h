#pragma once
#include <memory>
#include <string>

#include "gpu/backend.h"

// The GL ES 3.0 renderer (native/gles.cpp), the port's second backend. Created when the RENDERER setting asks for it and
// the driver gives a GL ES 3.0 context; nullptr there leaves the Vulkan backend to run as it always has.
std::unique_ptr<gpu::Backend> CreateGlesBackend();

// The same, for the Spider-Man 2 renderer (native/gl43): OpenGL through their fully grown backend, which draws what the
// D3D12 path draws rather than the thinner translation this port has. nullptr leaves Vulkan to run.
std::unique_ptr<gpu::Backend> CreateGl43Backend();


namespace gpu {
// what the graphics report says when the GL ES renderer is the one drawing
void GlesGraphicsReport();
std::string GlesGraphicsInfo();
}  // namespace gpu
