// HLSL to GLSL ES 3.00: the port's shaders are written once, for the XDK's HLSL path, and the GL ES renderer gets the
// same maths out of glslang's SPIR-V through SPIRV-Cross. What GL ES cannot express is taken out here rather than
// branching the shader sources.
#include "glsl.h"

#include <cstdio>
#include <cstring>
#include <regex>

#include <spirv_glsl.hpp>

#include "core/log.h"
#include "vk_shader.h"

namespace glsl {

namespace {

// GL ES 3.00 has no layout(binding) on a uniform block (that is 3.10), so the port binds blocks by name with
// glUniformBlockBinding and the qualifier is stripped here instead of being left for a driver to reject
void StripBlockBindings(std::string& s) {
    static const std::regex layout(R"(layout\s*\(([^)]*)\))");
    static const std::regex binding(R"(\s*binding\s*=\s*\d+\s*,?)");
    std::string out;
    size_t at = 0;
    for (std::sregex_iterator it(s.begin(), s.end(), layout), end; it != end; ++it) {
        const std::smatch& m = *it;
        std::string qualifiers = m[1].str(), rest;
        size_t from = 0;
        for (std::sregex_iterator b(qualifiers.begin(), qualifiers.end(), binding), bend; b != bend; ++b) {
            rest += qualifiers.substr(from, size_t((*b).position(0)) - from);
            from = size_t((*b).position(0) + (*b).length(0));
        }
        rest += qualifiers.substr(from);
        out += s.substr(at, size_t(m.position(0)) - at);
        // "std140" on its own is what a GL ES 3.00 uniform block takes
        if (rest.find_first_not_of(" ,") != std::string::npos) out += "layout(" + rest + ")";
        at = size_t(m.position(0) + m.length(0));
    }
    out += s.substr(at);
    s = std::move(out);
}

// noperspective interpolation is an NV extension in GL ES 3.00, and no phone here has it: the quad pass loses its exact
// stretch and its textures come out a little softer, which is the one place the two renderers' pictures may differ
bool StripNoperspective(std::string& s) {
    bool lost = false;
    for (size_t at; (at = s.find("#extension GL_NV_shader_noperspective_interpolation")) != std::string::npos;) {
        size_t end = s.find('\n', at);
        s.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
        lost = true;
    }
    for (size_t at; (at = s.find("noperspective ")) != std::string::npos;) {
        s.erase(at, std::strlen("noperspective "));
        lost = true;
    }
    return lost;
}

// GL ES 3.00 cannot put a location on a fragment shader's inputs, so the two stages have to agree by name; SPIRV-Cross
// names them by role instead ("i_d0" in, "_entryPointOutput_d0" out). Both sides are renamed to the same varyings here.
void MatchVaryings(std::string& s, Stage stage) {
    static const std::regex out_name(R"(\b_entryPointOutput_(\w+))");
    static const std::regex in_name(R"(\bi_(\w+))");
    if (stage == Stage::Pixel) {
        s = std::regex_replace(s, in_name, "vary_$1");
        return;
    }
    s = std::regex_replace(s, out_name, "vary_$1");
    // a pixel shader may read a value its vertex shader never wrote (undefined on Vulkan too), and a driver will not
    // link that: every value the game's vertex struct carries is declared here, whether this vertex shader writes it
    static const char* kCarried[] = {"vec4 vary_d0;", "vec4 vary_d1;", "vec4 vary_b0;", "vec4 vary_b1;",
                                     "vec4 vary_tc0;", "vec4 vary_tc1;", "vec4 vary_tc2;", "vec4 vary_tc3;",
                                     "float vary_fogamt;"};
    std::string add;
    for (const char* name : kCarried) {
        std::string decl = std::string("out ") + name;
        std::string key = std::string(name).substr(std::string(name).find(' ') + 1);  // "vary_d0;"
        if (s.find(key) == std::string::npos) add += decl + "\n";
    }
    if (add.empty()) return;
    size_t version = s.find("#version");
    size_t end = version == std::string::npos ? std::string::npos : s.find('\n', version);
    s.insert(end == std::string::npos ? 0 : end + 1, add);
}

// GL ES has no default precision for float or int in a fragment shader, and HLSL carries none for SPIRV-Cross to copy:
// without these two lines a phone's driver rejects every pixel shader outright
void AddPrecision(std::string& s) {
    if (s.find("precision highp float") != std::string::npos) return;
    size_t version = s.find("#version");
    size_t end = version == std::string::npos ? std::string::npos : s.find('\n', version);
    s.insert(end == std::string::npos ? 0 : end + 1, "precision highp float;\nprecision highp int;\n");
}

// the console's depth leaves its shaders as [0,1] and GL's clip space wants [-1,1]: a gl_Position write is wrapped so z
// becomes 2z - w, which is the same depth after the divide. Without it every clear, every depth test against the game's
// own cleared depth and every shadow map comparison would be in another scale than the one the game wrote.
void RemapDepth(std::string& s) {
    const std::string key = "gl_Position = ";
    std::string out;
    size_t at = 0;
    bool wrapped = false;
    for (size_t pos; (pos = s.find(key, at)) != std::string::npos;) {
        size_t i = pos + key.size();
        int nest = 0;
        while (i < s.size()) {
            char ch = s[i];
            if (ch == '(' || ch == '[') nest++;
            else if (ch == ')' || ch == ']') nest--;
            else if (ch == ';' && nest <= 0) break;
            i++;
        }
        if (i >= s.size()) break;
        out += s.substr(at, pos - at);
        out += "gl_Position = port_depth(" + s.substr(pos + key.size(), i - (pos + key.size())) + ")";
        at = i;  // the ; is left where it was
        wrapped = true;
    }
    out += s.substr(at);
    s = std::move(out);
    if (!wrapped) return;
    size_t version = s.find("#version");
    size_t end = version == std::string::npos ? std::string::npos : s.find('\n', version);
    // highp is spelled out, so the helper holds wherever the shader's own precision statements sit
    std::string helper = "vec4 port_depth(highp vec4 p) { p.z = p.z * 2.0 - p.w; return p; }\n";
    s.insert(end == std::string::npos ? 0 : end + 1, helper);
}

}  // namespace

bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::string& out,
             std::vector<Sampler>& samplers) {
    std::vector<uint32_t> spirv;
    vkshader::Stage vk = stage == Stage::Vertex ? vkshader::Stage::Vertex : vkshader::Stage::Pixel;
    if (!vkshader::Compile(hlsl, entry, vk, hash, spirv)) return false;
    try {
        spirv_cross::CompilerGLSL c(std::move(spirv));
        spirv_cross::CompilerGLSL::Options opt;
        opt.version = 300;
        opt.es = true;
        // the console's clip space has y down and the window's has it up: the vertex shader flips it, as the Vulkan
        // renderer does with its negative viewport height
        opt.vertex.flip_vert_y = true;
        c.set_common_options(opt);
        // one texture unit per stage: the port's separate texture and sampler bindings become one combined sampler each
        c.build_combined_image_samplers();
        for (const auto& r : c.get_combined_image_samplers()) {
            uint32_t unit = 0;
            if (c.has_decoration(r.image_id, spv::DecorationBinding)) {
                uint32_t binding = c.get_decoration(r.image_id, spv::DecorationBinding);
                if (binding >= vkshader::kBindingTextures && binding < vkshader::kBindingSamplers)
                    unit = binding - vkshader::kBindingTextures;
            }
            c.set_decoration(r.combined_id, spv::DecorationBinding, unit);
            // glslang names its opaque parameters after numbers: a name the port chose is what it can look up later
            std::string name = "port_tex" + std::to_string(unit);
            c.set_name(r.combined_id, name);
            bool known = false;
            for (const Sampler& s : samplers) known = known || s.name == name;
            if (!known) samplers.push_back({name, unit});
        }
        out = c.compile();
    } catch (const std::exception& e) {
        LOGE("gl: %s does not translate to GLSL ES: %s", entry, e.what());
        return false;
    }
    StripBlockBindings(out);
    MatchVaryings(out, stage);
    AddPrecision(out);
    if (stage == Stage::Vertex) RemapDepth(out);
    if (StripNoperspective(out)) LOGI("gl: %s: noperspective interpolation is not in GL ES 3.00, smoothed", entry);
    return true;
}

}  // namespace glsl
