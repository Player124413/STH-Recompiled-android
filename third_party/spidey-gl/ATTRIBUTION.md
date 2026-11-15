# spidey-gl — reference OpenGL renderer, not built into this port

Taken from https://github.com/SantiBros/SpiderMan2-2004-NX (MIT, © SantiBros), files `gl/gl_api.h`,
`gl/gl_backend.h`, `gl/gl_backend.cpp`, `gl/glsl.h`, `gl/glsl.cpp`, unmodified.

It is the same XboxXBERecomp `gpu::Backend` interface this port implements, drawn with OpenGL instead of Vulkan or
D3D12, and it is the only such renderer in the family. Kept here as the source of the GL path: the plan is to carry it
over to OpenGL ES 3.2 for Android, which is what a phone driver gives.

What it targets today, and what has to change:

- `#version 430 core` (glsl.cpp) and `GL/glcorearb.h`: desktop GL 4.3 through EGL (on the Switch through Mesa's
  nouveau). Android has GL ES 3.2 at most, so every shader needs the ES dialect and the generator needs its own head.
- `glPolygonMode` (3 calls) does not exist in ES at all: wireframe has to be dropped or emulated with a line index
  buffer.
- Dynamic sampler indexing is refused by ES: the pixel stage chooses its texture per stage at runtime, which in ES
  means one branch per stage or a small uniform-block indirection, not `samplers[i]`.
- `glBindVertexBuffer` (ES 3.1), `glBindBufferRange`, `glTexStorage2D`, `glMapBufferRange`, `glBindSampler`,
  `glDrawElementsBaseVertex`, `glCopyImageSubData` (ES 3.2) all exist and only need the loader and the headers
  switched.
