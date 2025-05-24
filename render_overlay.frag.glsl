/* See LICENSE for license details. */
layout(binding = 0) uniform sampler2D u_texture;

/* input:  h [0,360] | s,v [0, 1] *
 * output: rgb [0,1]              */
vec3 hsv2rgb(vec3 hsv)
{
	vec3 k = mod(vec3(5, 3, 1) + hsv.x / 60, 6);
	k = max(min(min(k, 4 - k), 1), 0);
	return hsv.z - hsv.z * hsv.y * k;
}

void main()
{
	out_colour = vec4(texture(u_texture, texture_coordinate).xyz, 1);
}
