
uniform mat4 ModelViewProjectionMatrix;
uniform mat4 ModelMatrix;
uniform mat4 ModelViewMatrix;
uniform mat3 WorldNormalMatrix;
#ifndef ATTRIB
uniform mat3 NormalMatrix;
#endif

#ifndef HAIR_SHADER_FIBERS
in vec3 pos;
in vec3 nor;
#else
in int fiber_index;
in float curve_param;
#endif

out vec3 worldPosition;
out vec3 viewPosition;

/* Used for planar reflections */
/* keep in sync with EEVEE_ClipPlanesUniformBuffer */
layout(std140) uniform clip_block {
	vec4 ClipPlanes[1];
};

#ifdef USE_FLAT_NORMAL
flat out vec3 worldNormal;
flat out vec3 viewNormal;
#else
out vec3 worldNormal;
out vec3 viewNormal;
#endif

void main() {
#ifndef HAIR_SHADER_FIBERS
	gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);
#else
	vec3 pos;
	vec3 nor;
	vec2 view_offset;
	hair_fiber_get_vertex(fiber_index, curve_param, ModelViewMatrix, pos, nor, view_offset);
	gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);
	gl_Position.xy += view_offset * gl_Position.w;
#endif

	viewPosition = (ModelViewMatrix * vec4(pos, 1.0)).xyz;
	worldPosition = (ModelMatrix * vec4(pos, 1.0)).xyz;
	viewNormal = normalize(NormalMatrix * nor);
	worldNormal = normalize(WorldNormalMatrix * nor);

	/* Used for planar reflections */
	gl_ClipDistance[0] = dot(vec4(worldPosition, 1.0), ClipPlanes[0]);

#ifdef ATTRIB
	pass_attrib(pos);
#endif
}