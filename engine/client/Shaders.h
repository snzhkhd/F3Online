#pragma once
// GLSL for the original-asset renderer (diffuse x vertex colour x lightmap, "2X" modulation like the
// original BASE_2X / SK_BASE_LM_2X techniques). Desktop GL 3.3.

namespace f3 {

inline const char* kVertexShader = R"GLSL(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec2 vertexTexCoord2;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 vUv;
out vec2 vUv2;
out vec4 vCol;
out vec3 vNrm;
out float vWorldY;
void main() {
    vUv = vertexTexCoord;
    vUv2 = vertexTexCoord2;
    vCol = vertexColor;
    vec4 wp = matModel * vec4(vertexPosition, 1.0);
    vWorldY = wp.y;
    vNrm = mat3(matModel) * vertexNormal;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)GLSL";

inline const char* kFragmentShader = R"GLSL(
#version 330
in vec2 vUv;
in vec2 vUv2;
in vec4 vCol;
in vec3 vNrm;
in float vWorldY;
uniform sampler2D texture0;   // diffuse
uniform sampler2D texture1;   // lightmap
uniform int uUseTex;
uniform int uUseLM;
uniform int uAlphaMode;       // 0 opaque, 1 vertex alpha, 2 image alpha (cut-out), 3 image alpha x vertex alpha
uniform int uLit;             // 1: dynamic object, apply a simple sun term (baked tiles use their lightmap)
uniform vec4 uTint;
uniform float uClipY;         // fragments above this world height are discarded (roof cut-away)
uniform float uLmGain;
out vec4 finalColor;
void main() {
    if (vWorldY > uClipY) discard;
    vec4 base = (uUseTex != 0) ? texture(texture0, vUv) : vec4(1.0);
    vec3 col = base.rgb * vCol.rgb * 2.0;
    if (uUseLM != 0) col *= texture(texture1, vUv2).rgb * uLmGain;
    if (uLit != 0) {
        vec3 n = normalize(vNrm);
        float ndl = max(dot(n, normalize(vec3(0.45, 1.0, 0.35))), 0.0);
        col *= 0.55 + 0.55 * ndl;
    }
    col *= uTint.rgb;
    float a = 1.0;
    if (uAlphaMode == 1) a = vCol.a;
    else if (uAlphaMode == 2) { a = base.a; if (a < 0.5) discard; a = 1.0; }
    else if (uAlphaMode == 3) a = base.a * vCol.a;
    finalColor = vec4(col, a * uTint.a);
}
)GLSL";

}  // namespace f3
