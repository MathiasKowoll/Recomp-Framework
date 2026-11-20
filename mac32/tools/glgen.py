#!/usr/bin/env python3
"""Generate OpenGL import shims (guest i386 cdecl -> host OpenGL) from a signature table.

Arg codes:  e enum/uint   i int   u uint   f float (1 slot)   d double (2 slots)
            p input pointer (NULL stays NULL)   o output pointer
            B array-buffer-relative pointer (offset when a VBO is bound, else guest pointer)
            X element-buffer-relative pointer
            U pixel data: an offset when a pixel UNPACK buffer is bound, else a guest pointer
            K the same for what is read back (a pixel PACK buffer)
            r an offset into a bound buffer, never a pointer (the indirect draws)
Return:     v void   u uint   i int
"""
import sys

SPEC = """
glActiveTexture v e
glAlphaFunc v e f
glAttachShader v u u
glBindAttribLocation v u u p
glBindBuffer v e u
glBindFramebufferEXT v e u
glBindProgramARB v e u
glBindRenderbufferEXT v e u
glBindTexture v e u
glBindVertexArrayAPPLE v u
glBlendEquation v e
glBlendEquationSeparate v e e
glBlendFunc v e e
glBlendFuncSeparate v e e e e
glBufferData v e i p e
glClear v u
glClearColor v f f f f
glClearDepth v d
glClientActiveTexture v e
glColor4f v f f f f
glColorMask v u u u u
glColorPointer v i e i B
glCompileShader v u
glCompressedTexImage2D v e i e i i i i U
glCreateProgram u
glCreateShader u e
glCullFace v e
glDeleteBuffers v i p
glDeleteFramebuffersEXT v i p
glDeleteProgram v u
glDeleteProgramsARB v i p
glDeleteRenderbuffersEXT v i p
glDeleteShader v u
glDeleteTextures v i p
glDeleteVertexArraysAPPLE v i p
glDepthFunc v e
glDepthMask v u
glDepthRange v d d
glDetachShader v u u
glDisable v e
glDisableClientState v e
glDisableVertexAttribArray v u
glDrawArrays v e i i
glDrawRangeElements v e u u i e X
glEnable v e
glEnableClientState v e
glEnableVertexAttribArray v u
glFlush v
glFramebufferRenderbufferEXT v e e e u
glFramebufferTexture2DEXT v e e e u i
glFrontFace v e
glGenBuffers v i o
glGenFramebuffersEXT v i o
glGenProgramsARB v i o
glGenRenderbuffersEXT v i o
glGenTextures v i o
glGenVertexArraysAPPLE v i o
glGetError u
glGetIntegerv v e o
glGetProgramInfoLog v u i o o
glGetProgramiv v u e o

glGetShaderInfoLog v u i o o
glGetShaderSource v u i o o
glGetShaderiv v u e o
glGetTexImage v e i e e K
glGetTexLevelParameteriv v e i e o
glGetUniformLocation i u p
glLightModelf v e f
glLightModelfv v e p
glLightf v e e f
glLightfv v e e p
glLinkProgram v u
glLoadIdentity v
glLoadMatrixf v p
glMaterialf v e e f
glMaterialfv v e e p
glMatrixMode v e
glNormal3f v f f f
glNormalPointer v e i B
glPixelStorei v e i
glPolygonOffset v f f
glProgramEnvParameters4fvEXT v e u i p
glProgramStringARB v e e i p
glReadPixels v i i i i e e K
glRenderbufferStorageEXT v e e i i
glStencilFunc v e i u
glStencilMask v u
glStencilOp v e e e
glTexCoordPointer v i e i B
glTexEnvfv v e e p
glTexEnvi v e e i
glTexImage2D v e i i i i i e e U
glTexParameterf v e e f
glTexParameterfv v e e p
glTexParameteri v e e i
glTexSubImage2D v e i i i i i e e U
glUniform1f v i f
glUniform1i v i i
glUniform4fv v i i p
glUniformMatrix4fv v i i u p
glUseProgram v u
glVertexAttrib1fv v u p
glVertexAttrib2fv v u p
glVertexAttrib3fv v u p
glVertexAttrib4fv v u p
glVertexAttribPointer v u i e u i B
glVertexPointer v i e i B
glBeginQuery v e u
glBeginTransformFeedback v e
glBindBufferBase v e u u
glBindBufferRange v e u u i i
glBindFragDataLocation v u u p
glBindFramebuffer v e u
glBindProgramPipeline v u
glBindRenderbuffer v e u
glBindSampler v u u
glBindVertexArray v u
glBlendColor v f f f f
glBlendEquationSeparatei v u e e
glBlendFuncSeparatei v u e e e e
glBlitFramebuffer v i i i i i i i i u e
glBufferParameteriAPPLE v e e i
glBufferSubData v e i i p
glCheckFramebufferStatus u e
glClampColorARB v e e
glClearBufferfi v e i f i
glClearBufferfv v e i p
glClearBufferiv v e i p
glClearBufferuiv v e i p
glClearStencil v i
glClipPlane v e p
glColor4fv v p
glColorMaskIndexedEXT v u u u u u
glColorMaski v u u u u u
glColorMaterial v e e
glCompressedTexImage1D v e i e i i i U
glCompressedTexImage3D v e i e i i i i i U
glCompressedTexSubImage1D v e i i i e i U
glCompressedTexSubImage2D v e i i i i i e i U
glCompressedTexSubImage3D v e i i i i i i i e i U
glCopyBufferSubData v e e i i i
glCopyTexImage2D v e i e i i i i i
glCopyTexSubImage2D v e i i i i i i i
glDeleteFencesAPPLE v i p
glDeleteFramebuffers v i p
glDeleteProgramPipelines v i p
glDeleteQueries v i p
glDeleteRenderbuffers v i p
glDeleteSamplers v i p
glDeleteVertexArrays v i p
glDepthBoundsEXT v d d
glDisableIndexedEXT v e u
glDisablei v e u
glDrawArraysIndirect v e r
glDrawArraysInstanced v e i i i
glDrawBuffer v e
glDrawBuffers v i p
glDrawElements v e i e X
glDrawElementsBaseVertex v e i e X i
glDrawElementsIndirect v e e r
glDrawElementsInstanced v e i e X i
glDrawElementsInstancedBaseVertex v e i e X i i
glDrawRangeElementsBaseVertex v e u u i e X i
glEnableIndexedEXT v e u
glEnablei v e u
glEndQuery v e
glEndTransformFeedback v
glFinish v
glFinishObjectAPPLE v e i
glFogCoordPointerEXT v e i B
glFogf v e f
glFogfv v e p
glFogi v e i
glFramebufferRenderbuffer v e e e u
glFramebufferTexture v e e u i
glFramebufferTexture2D v e e e u i
glFramebufferTextureLayer v e e u i i
glGenFencesAPPLE v i o
glGenFramebuffers v i o
glGenProgramPipelines v i o
glGenQueries v i o
glGenRenderbuffers v i o
glGenSamplers v i o
glGenVertexArrays v i o
glGenerateMipmap v e
glGenerateMipmapEXT v e
glGetBooleanv v e o
glGetBufferSubData v e i i o
glGetCompressedTexImage v e i K
glGetFloatv v e o
glGetFramebufferAttachmentParameteriv v e e e o
glGetObjectParameterivAPPLE v e u e o
glGetProgramBinary v u i o o o
glGetProgramStringARB v e e o
glGetQueryObjectiv v u e o
glGetQueryObjectui64v v u e o
glGetQueryObjectuiv v u e o
glGetQueryiv v e e o
glGetRenderbufferParameteriv v e e o
glGetTexEnvfv v e e o
glGetTransformFeedbackVarying v u u i o o o o
glGetUniformBlockIndex u u p
glHint v e e
glIsEnabled u e
glIsQuery u u
glIsTexture u u
glLightModeli v e i
glMateriali v e e i
glMultMatrixf v p
glObjectPurgeableAPPLE u e u e
glObjectUnpurgeableAPPLE u e u e
glPatchParameteri v e i
glPolygonMode v e e
glPopMatrix v
glPrioritizeTextures v i p p
glProgramBinary v u e p i
glProgramEnvParameter4fvARB v e u p
glProgramLocalParameters4fvEXT v e u i p
glProgramParameteri v u e i
glProgramUniform1i v u i i
glProgramUniform1iv v u i i p
glProgramUniform4fv v u i i p
glProgramUniform4iv v u i i p
glProvokingVertexEXT v e
glPushMatrix v
glQueryCounter v u e
glReadBuffer v e
glRenderbufferStorage v e e i i
glRenderbufferStorageMultisample v e i e i i
glSampleMaski v u u
glSamplerParameterf v u e f
glSamplerParameterfv v u e p
glSamplerParameteri v u e i
glScalef v f f f
glSecondaryColor3fvEXT v p
glSecondaryColorPointerEXT v i e i B
glSetFenceAPPLE v u
glShadeModel v e
glStencilFuncSeparate v e e i u
glStencilOpSeparate v e e e e
glTestFenceAPPLE u u
glTexBuffer v e e u
glTexEnvf v e e f
glTexGenfv v e e p
glTexGeni v e e i
glTexImage1D v e i i i i e e U
glTexImage2DMultisample v e i e i i u
glTexImage3D v e i i i i i i e e U
glTexImage3DMultisample v e i e i i i u
glTexStorage1D v e i e i
glTexStorage2D v e i e i i
glTexStorage3D v e i e i i i
glTexSubImage1D v e i i i e e U
glTexSubImage3D v e i i i i i i i e e U
glTranslatef v f f f
glUniform1iv v i i p
glUniform4iv v i i p
glUniformBlockBinding v u u u
glUseProgramStages v u u u
glVertexAttribDivisorARB v u u
glVertexAttribIPointer v u i e i B
glVertexBlendARB v i
"""


def gen(out):
    lines = ['// GENERATED by tools/glgen.py - do not edit', '#define GL_SILENCE_DEPRECATION',
             '#include "rt.h"', '#include <OpenGL/gl.h>', '#include <OpenGL/glext.h>',
             '#include <OpenGL/gl3.h>', '#include <OpenGL/gl3ext.h>',
             'void *gl_ptr_array(uint32_t g);', 'void *gl_ptr_element(uint32_t g);',
             'void *gl_ptr_unpack(uint32_t g);', 'void *gl_ptr_pack(uint32_t g);',
             'void gl_trace(const char *name);',
             'void gl_trace_args(const char *name, CPU *c, int nslots);',
             '#define P(g) ((g) ? G2H(g) : NULL)', '']
    for line in SPEC.strip().splitlines():
        if not line.strip(): continue
        name, ret, *args = line.split()
        slot, cargs = 0, []
        for a in args:
            if a in "eu": cargs.append("(GLuint)ARG(%d)" % slot); slot += 1
            elif a == "i": cargs.append("(GLint)ARG(%d)" % slot); slot += 1
            elif a == "f": cargs.append("ARGF(%d)" % slot); slot += 1
            elif a == "d": cargs.append("ARGD(%d)" % slot); slot += 2
            elif a in "po": cargs.append("P(ARG(%d))" % slot); slot += 1
            elif a == "B": cargs.append("gl_ptr_array(ARG(%d))" % slot); slot += 1
            elif a == "X": cargs.append("gl_ptr_element(ARG(%d))" % slot); slot += 1
            elif a == "U": cargs.append("gl_ptr_unpack(ARG(%d))" % slot); slot += 1
            elif a == "K": cargs.append("gl_ptr_pack(ARG(%d))" % slot); slot += 1
            elif a == "r": cargs.append("(void *)(uintptr_t)ARG(%d)" % slot); slot += 1
            else: raise SystemExit("bad code " + a)
        call = "%s(%s)" % (name, ", ".join(cargs))
        body = call + ";" if ret == "v" else "c->eax = (uint32_t)%s;" % call
        lines.append('void I__%s(CPU *c) { gl_trace("%s"); gl_trace_args("%s", c, %d); %s }' % (name, name, name, slot, body))
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    gen(sys.argv[1])
