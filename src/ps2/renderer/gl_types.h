/* ================================================================================================
 * File: gl_types.h
 * Brief: The OpenGL type names QuakeSpasm's headers are written with, for a build that has no
 *        OpenGL. Some of QuakeSpasm's renderer structs (gltexture_t's texnum) and the extension
 *        function pointers glquake.h declares use GL types, and quakedef.h includes those headers
 *        into every engine file - so the names have to exist even though nothing calls GL.
 *        Types only, on purpose: with no GL function declared anywhere, a stray GL call left in
 *        the engine fails to compile instead of failing to draw.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_RENDERER_GL_TYPES_H
#define PS2_RENDERER_GL_TYPES_H

#include <stddef.h> // ptrdiff_t

typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLuint;
typedef float         GLfloat;
typedef char          GLchar;
typedef ptrdiff_t     GLsizeiptrARB;
typedef ptrdiff_t     GLintptrARB;

// These only ever stood in for Windows' __stdcall; the EE has the one calling convention.
#define APIENTRY
#define APIENTRYP APIENTRY *

// The extension entry points glquake.h keeps pointers to (multitexture and vertex buffers).
// Only the types are needed: the PS2 build never assigns or calls any of them.
typedef void (APIENTRYP PFNGLMULTITEXCOORD2FARBPROC)(GLenum target, GLfloat s, GLfloat t);
typedef void (APIENTRYP PFNGLACTIVETEXTUREARBPROC)(GLenum texture);
typedef void (APIENTRYP PFNGLCLIENTACTIVETEXTUREARBPROC)(GLenum texture);
typedef void (APIENTRYP PFNGLBINDBUFFERARBPROC)(GLenum target, GLuint buffer);
typedef void (APIENTRYP PFNGLBUFFERDATAARBPROC)(GLenum target, GLsizeiptrARB size, const void * data, GLenum usage);
typedef void (APIENTRYP PFNGLBUFFERSUBDATAARBPROC)(GLenum target, GLintptrARB offset, GLsizeiptrARB size, const void * data);
typedef void (APIENTRYP PFNGLDELETEBUFFERSARBPROC)(GLsizei n, const GLuint * buffers);
typedef void (APIENTRYP PFNGLGENBUFFERSARBPROC)(GLsizei n, GLuint * buffers);

#endif // PS2_RENDERER_GL_TYPES_H
