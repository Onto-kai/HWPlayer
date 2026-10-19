#version 420 core


out vec4 FragColor;


in vec2 TexCoord;


uniform sampler2D texY;
uniform sampler2D texUV;


void main()
{
    float y =
        texture(texY,TexCoord).r;


    vec2 uv =
        texture(texUV,TexCoord).rg;


    float u = uv.r - 0.5;
    float v = uv.g - 0.5;


    vec3 rgb;


    rgb.r = y + 1.402*v;

    rgb.g = y -0.344*u -0.714*v;

    rgb.b = y +1.772*u;


    FragColor =
        vec4(rgb,1.0);
}