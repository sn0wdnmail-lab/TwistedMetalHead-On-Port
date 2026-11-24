#ifndef PS2_GL_SHADER_H
#define PS2_GL_SHADER_H

#include <string>

namespace ps2gl
{
    // The picture shaders are written in GLSL 3.30. On Android (OpenGL ES 3) the same code runs as
    // GLSL ES 3.00: only the version line changes, plus the precision ES asks for.
    inline std::string fragmentSource(const char *glsl330)
    {
        std::string source(glsl330 ? glsl330 : "");
#if defined(__ANDROID__)
        const std::string version = "#version 330";
        const size_t at = source.find(version);
        if (at != std::string::npos)
        {
            size_t end = source.find('\n', at);
            if (end == std::string::npos)
                end = source.size();
            source.replace(at, end - at,
                           "#version 300 es\n"
                           "precision highp float;\n"
                           "precision highp int;\n"
                           "precision highp sampler2D;");
        }
#endif
        return source;
    }
}

#endif // PS2_GL_SHADER_H
