#!/usr/bin/env python3
import sys
import struct

def read_cstrings(f, count):
    out = []
    for _ in range(count):
        s = bytearray()
        while True:
            b = f.read(1)
            if not b:
                break
            if b == b'\x00':
                break
            s += b
        out.append(s.decode('utf-8', errors='replace'))
    return out

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: python check_budmesh.py <file.budmesh> [max_meshlets=5]')
        sys.exit(1)

    path = sys.argv[1]
    max_meshlets = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    with open(path, 'rb') as f:
        header_data = f.read(136)
        if len(header_data) < 136:
            print('File too small or invalid header')
            sys.exit(2)
        # <8I 6f 10Q
        hdr = struct.unpack('<8I6f10Q', header_data)
        magic = hdr[0]
        version = hdr[1]
        total_vertices = hdr[2]
        total_indices = hdr[3]
        meshlet_count = hdr[4]
        submesh_count = hdr[5]
        texture_count = hdr[6]
        material_count = hdr[7]
        aabb_min = hdr[8:11]
        aabb_max = hdr[11:14]
        # offsets: start at index 14
        offsets = hdr[14:24]
        (reserved, vertex_offset, index_offset, meshlet_offset, vertex_index_offset,
         meshlet_index_offset, cull_data_offset, submesh_offset, material_offset, texture_offset) = offsets

        print(f'magic=0x{magic:08X} version={version}')
        print(f'vertices={total_vertices} indices={total_indices}')
        print(f'meshlets={meshlet_count} submeshes={submesh_count} textures={texture_count} materials={material_count}')
        print(f'aabb_min={aabb_min} aabb_max={aabb_max}')
        print(f'offsets: vertex={vertex_offset} index={index_offset} meshlet={meshlet_offset} vertex_index={vertex_index_offset}')
        print(f'         meshlet_index={meshlet_index_offset} cull_data={cull_data_offset} submesh={submesh_offset}')
        print(f'         material={material_offset} texture={texture_offset}')

        # Read materials
        if material_count > 0 and material_offset != 0:
            f.seek(material_offset)
            mats = []
            for i in range(material_count):
                data = f.read(12)
                if len(data) < 12:
                    print(f'Unexpected EOF reading material {i}')
                    break
                # '<I B B 2x f' -> pack as I, B, B, 2 pad, f
                base_color_texture, alpha_mode, double_sided, alpha_cutoff = struct.unpack('<I B B 2x f', data)
                mats.append((base_color_texture, alpha_mode, double_sided, alpha_cutoff))
            print('\nMaterials:')
            for idx, m in enumerate(mats):
                print(f'  [{idx}] base_tex={m[0]} alpha_mode={m[1]} double_sided={m[2]} alpha_cutoff={m[3]}')
        else:
            print('\nNo materials table present')

        # Read texture strings
        if texture_count > 0 and texture_offset != 0:
            f.seek(texture_offset)
            textures = read_cstrings(f, texture_count)
            print('\nTextures:')
            for ti, t in enumerate(textures):
                print(f'  [{ti}] {t}')
        else:
            print('\nNo textures present')

        # Read a few meshlet descriptors and cull data
        if meshlet_count > 0 and meshlet_offset != 0:
            to_read = min(meshlet_count, max_meshlets)
            f.seek(meshlet_offset)
            print(f'\nFirst {to_read} MeshletDescriptors:')
            for i in range(to_read):
                data = f.read(16)
                if len(data) < 16:
                    break
                vertex_offset_m, vertex_count_m, tri_offset_m, tri_count_m = struct.unpack('<4I', data)
                print(f'  [{i}] v_off={vertex_offset_m} v_count={vertex_count_m} tri_off={tri_offset_m} tri_count={tri_count_m}')

            if cull_data_offset != 0:
                f.seek(cull_data_offset)
                print(f'\nFirst {to_read} MeshletCullData:')
                for i in range(to_read):
                    data = f.read(20)
                    if len(data) < 20:
                        break
                    bs0, bs1, bs2, bs3, ca0, ca1, ca2, cutoff = struct.unpack('<4f3b b', data)
                    print(f'  [{i}] bs=({bs0:.3f},{bs1:.3f},{bs2:.3f}) r={bs3:.3f} cone_axis=({ca0},{ca1},{ca2}) cutoff={cutoff}')
        else:
            print('\nNo meshlets present')

    print('\nDone.')
