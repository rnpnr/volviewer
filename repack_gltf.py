import json

def dump_sub_buffer_to_file(gltf, accessor, output):
    position_attribute = gltf["accessors"][accessor]
    buffer_view = gltf["bufferViews"][position_attribute["bufferView"]]
    offset = buffer_view["byteOffset"]
    length = buffer_view["byteLength"]
    buffer = buffer_view["buffer"]

    with open(gltf["buffers"][buffer]["uri"], "rb") as input:
        input.seek(offset, 0)
        output.write(input.read(length))

def dump_sub_buffer(gltf, accessor, output_name):
    with open(output_name, "wb") as output:
        dump_sub_buffer_to_file(gltf, accessor, output)

def main():
    with open('unit_cube.gltf', 'r') as f:
        unit_cube = json.load(f)

    assert(len(unit_cube["meshes"]) == 1)
    mesh = unit_cube["meshes"][0]

    assert(len(mesh["primitives"]) == 1)
    attributes = mesh["primitives"][0]["attributes"]
    indices    = mesh["primitives"][0]["indices"]

    dump_sub_buffer(unit_cube, attributes["POSITION"], "unit_cube_positions.bin")
    dump_sub_buffer(unit_cube, attributes["NORMAL"],   "unit_cube_normals.bin")
    dump_sub_buffer(unit_cube, indices,                "unit_cube_indices.bin")

if __name__ == '__main__':
    main()
