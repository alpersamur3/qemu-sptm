#version 300 es
precision highp float;

/*
 * The guest's picture scaled to the window, sharper than the bilinear blit:
 * scaled up (a watch's 352x430 in a larger window), Catmull-Rom over 9
 * bilinear fetches; scaled down (a phone's 1179x2556), four rotated-grid
 * samples inside the window pixel with a light unsharp mask from four around
 * it, where one bilinear fetch would skip texels.
 */
uniform sampler2D image;
in  vec2 ex_tex_coord;
out vec4 out_frag_color;

vec3 up(vec2 uv, vec2 size)
{
    vec2 p = uv * size;
    vec2 tc = floor(p - 0.5) + 0.5;
    vec2 f = p - tc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (tc - 1.0) / size;
    vec2 t3 = (tc + 2.0) / size;
    vec2 t12 = (tc + w2 / w12) / size;
    vec3 c = texture(image, vec2(t0.x, t0.y)).rgb * w0.x * w0.y;

    c += texture(image, vec2(t12.x, t0.y)).rgb * w12.x * w0.y;
    c += texture(image, vec2(t3.x, t0.y)).rgb * w3.x * w0.y;
    c += texture(image, vec2(t0.x, t12.y)).rgb * w0.x * w12.y;
    c += texture(image, vec2(t12.x, t12.y)).rgb * w12.x * w12.y;
    c += texture(image, vec2(t3.x, t12.y)).rgb * w3.x * w12.y;
    c += texture(image, vec2(t0.x, t3.y)).rgb * w0.x * w3.y;
    c += texture(image, vec2(t12.x, t3.y)).rgb * w12.x * w3.y;
    c += texture(image, vec2(t3.x, t3.y)).rgb * w3.x * w3.y;
    return c;
}

vec3 down(vec2 uv, vec2 d)
{
    vec3 c = (texture(image, uv + d * vec2(0.125, -0.375)).rgb +
              texture(image, uv + d * vec2(0.375, 0.125)).rgb +
              texture(image, uv + d * vec2(-0.125, 0.375)).rgb +
              texture(image, uv + d * vec2(-0.375, -0.125)).rgb) * 0.25;
    vec3 around = (texture(image, uv + d * vec2(1.0, 0.0)).rgb +
                   texture(image, uv + d * vec2(-1.0, 0.0)).rgb +
                   texture(image, uv + d * vec2(0.0, 1.0)).rgb +
                   texture(image, uv + d * vec2(0.0, -1.0)).rgb) * 0.25;

    return c + (c - around) * 0.25;
}

void main(void)
{
    vec2 size = vec2(textureSize(image, 0));
    vec2 d = fwidth(ex_tex_coord);
    vec2 per_pixel = d * size;      /* texels per window pixel */
    vec3 c = max(per_pixel.x, per_pixel.y) > 1.01 ? down(ex_tex_coord, d)
                                                  : up(ex_tex_coord, size);

    out_frag_color = vec4(clamp(c, 0.0, 1.0), texture(image, ex_tex_coord).a);
}
