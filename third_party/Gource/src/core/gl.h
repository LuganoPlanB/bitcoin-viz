#ifndef CORE_GL_H
#define CORE_GL_H

#ifdef _WIN32
#include <windows.h> // Keep APIENTRY defined after GLEW for the system GLU header.
#endif
#include <GL/glew.h>
// Some GLEW packages deliberately omit GLU declarations.
#ifdef __APPLE__
#include <OpenGL/glu.h>
#else
#include <GL/glu.h>
#endif

#define PRINT_GL_ERRORS

#ifdef PRINT_GL_ERRORS
#define glCheckError() { \
    int gl_error = glGetError(); \
    if(gl_error != GL_NO_ERROR) warnLog("GL error %s at %s:%d", gluErrorString(gl_error), __FILE__, __LINE__); \
}
#else
#define glCheckError()
#endif

#endif


