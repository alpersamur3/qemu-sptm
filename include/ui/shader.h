#ifndef QEMU_SHADER_H
#define QEMU_SHADER_H

typedef struct QemuGLShader QemuGLShader;

void qemu_gl_run_texture_blit(QemuGLShader *gls, bool flip);
void qemu_gl_set_sharp(QemuGLShader *gls, bool sharp);

QemuGLShader *qemu_gl_init_shader(void);
void qemu_gl_fini_shader(QemuGLShader *gls);

#endif /* QEMU_SHADER_H */
