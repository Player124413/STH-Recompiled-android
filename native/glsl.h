#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The same HLSL the Vulkan renderer takes, through SPIR-V and out again as GLSL ES 3.00 for the OpenGL ES renderer.
// One source of truth for both: a shader is written once in shaders.cpp and every renderer sees the same maths, which is
// the whole point of the port.
namespace glsl {

enum class Stage { Vertex, Pixel };

// a combined sampler in the emitted GLSL and the texture unit it reads: unit i is the draw's texture slot i
struct Sampler {
    std::string name;
    uint32_t unit = 0;
};

// the GLSL ES 3.00 text of hlsl's entry point, false after logging why; samplers lists the uniforms to set the units of
// (once, after linking). The SPIR-V under it is the cached one, so switching renderers does not recompile anything.
bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::string& out,
             std::vector<Sampler>& samplers);

}  // namespace glsl
