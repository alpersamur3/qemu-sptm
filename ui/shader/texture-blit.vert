#version 300 es

in vec2  in_position;
out vec2 ex_tex_coord;
/* the picture turned about the viewport's centre: cos and sin of the angle, and the viewport's height over
   its width (the turn happens in pixels, not in the stretched clip space); (1, 0, any) leaves it as it is */
uniform vec3 turn;

void main(void) {
    vec2 p = in_position;
    p = vec2(p.x * turn.x - p.y * turn.z * turn.y, p.x * turn.y / turn.z + p.y * turn.x);
    gl_Position = vec4(p, 0.0, 1.0);
    ex_tex_coord = vec2(1.0 + in_position.x, 1.0 - in_position.y) * 0.5;
}
