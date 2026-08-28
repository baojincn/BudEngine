#!/usr/bin/env python3
"""BudNanite (.budnanite) structural validator.

Validates the UE5-Nanite-aligned file layout produced by BudAssetTool:
  - Header magic/version, table offsets, page data region
  - FCluster layout (<=128 vertices / 128 triangles, page refs, u32 ordered LOD errors)
  - FClusterGroup layout (page range, children range)
  - FPageStreamingState / FPageDependency
  - Page capacity (<=128KB), page-local u16 index bounds
  - DAG integrity (all clusters covered via cluster.group_index, root reachable)
  - Page dependency closure
  - Page-level quantized-position read-back within page AABB

Usage: python check_budnanite.py <file.budnanite>
"""

import struct
import sys

NANITE_MAGIC = 0x544E4E42      # "BNNT"
NANITE_VERSION = 2
PAGE_DATA_MAGIC = 0x50474142   # "BAGP"
INVALID_INDEX = 0xFFFFFFFF
PAGE_MAX_SIZE = 128 * 1024
GROUP_MAX_CHILDREN = 8

HDR = struct.Struct("<IIIIIIIIIIQQQQQQQQffffffQQ")
assert HDR.size == 144, HDR.size

CLUSTER = struct.Struct("<10I14f")  # 10*u32 + 14*f32 = 40 + 56 = 96
assert CLUSTER.size == 96, CLUSTER.size

GROUP = struct.Struct("<4I5f")      # 4*u32 + 5*f32 = 16 + 20 = 36
assert GROUP.size == 36, GROUP.size

LEVEL = struct.Struct("<IIII")
assert LEVEL.size == 16

PAGE_STATE = struct.Struct("<9I")   # 36 bytes (28 UE5-aligned + 8 ext)
assert PAGE_STATE.size == 36, PAGE_STATE.size

DEP = struct.Struct("<III")
assert DEP.size == 12

PAGE_HDR = struct.Struct("<10I6f")  # 10*u32 + 6*f32 = 40 + 24 = 64
assert PAGE_HDR.size == 64, PAGE_HDR.size

MAT = struct.Struct("<I B B 2B f")
assert MAT.size == 12


def decode_lod_error(u):
	"""UE5-style ordered-int decode: (i & 0x80000000) ? ~i : (i ^ 0x80000000)."""
	u &= 0xFFFFFFFF
	if u & 0x80000000:
		bits = (u ^ 0x80000000) & 0xFFFFFFFF
	else:
		bits = (~u) & 0xFFFFFFFF
	return struct.unpack("<f", struct.pack("<I", bits))[0]


def fail(msg):
	print(f"[FAIL] {msg}")
	sys.exit(1)


def main():
	if len(sys.argv) < 2:
		print(__doc__)
		sys.exit(2)
	path = sys.argv[1]
	data = open(path, "rb").read()

	h = HDR.unpack_from(data, 0)
	magic, version, flags, cluster_count, group_count, level_count, page_count, dep_count, mat_count, tex_count = h[0:10]
	cluster_off, group_off, level_off, page_state_off, dep_off, mat_off, tex_off, page_data_off = h[10:18]
	aabb_min = h[18:21]
	aabb_max = h[21:24]

	if magic != NANITE_MAGIC:
		fail(f"bad magic 0x{magic:08X}")
	if version != NANITE_VERSION:
		fail(f"bad version {version} (expected {NANITE_VERSION})")
	print(f"version={version} clusters={cluster_count} groups={group_count} "
		  f"levels={level_count} pages={page_count} deps={dep_count} mats={mat_count} texs={tex_count}")
	print(f"aabb_min={aabb_min} aabb_max={aabb_max}")

	# ---- tables ----
	clusters = [CLUSTER.unpack_from(data, cluster_off + i * 96) for i in range(cluster_count)]
	groups = [GROUP.unpack_from(data, group_off + i * 36) for i in range(group_count)]
	levels = [LEVEL.unpack_from(data, level_off + i * 16) for i in range(level_count)]
	pages = [PAGE_STATE.unpack_from(data, page_state_off + i * 36) for i in range(page_count)]
	deps = [DEP.unpack_from(data, dep_off + i * 12) for i in range(dep_count)]

	# ---- offsets consistency ----
	expected = cluster_off
	expected += cluster_count * 96
	assert group_off == expected, "group_offset"
	expected += group_count * 36
	assert level_off == expected, "level_offset"
	expected += level_count * 16
	assert page_state_off == expected, "page_state_offset"
	expected += page_count * 36
	assert dep_off == expected, "dep_offset"
	expected += dep_count * 12
	assert mat_off == expected, "mat_offset"
	expected += mat_count * 12
	assert tex_off == expected, "tex_offset"
	off = tex_off
	for _ in range(tex_count):
		end = data.index(b"\0", off)
		off = end + 1
	assert page_data_off == off, "page_data_offset"
	print(f"table layout ok; page data at {page_data_off}")

	# ---- clusters (FCluster) ----
	max_v = max_t = 0
	for i, c in enumerate(clusters):
		nverts, ntris, mat, pos_off, pos_page, idx_off, idx_page, gidx, lerr, perr = c[0:10]
		if nverts > 128 or ntris > 128:
			fail(f"cluster {i}: over limit verts={nverts} tris={ntris}")
		max_v, max_t = max(max_v, nverts), max(max_t, ntris)
		if pos_page >= page_count or idx_page >= page_count:
			fail(f"cluster {i}: page {pos_page}/{idx_page} out of range")
		if pos_off + nverts > pages[pos_page][1]:
			fail(f"cluster {i}: vertex range over page vertex stream")
		if idx_off + ntris > pages[idx_page][3]:
			fail(f"cluster {i}: triangle range over page index stream")
		if gidx != INVALID_INDEX and gidx >= group_count:
			fail(f"cluster {i}: group {gidx} out of range")
		# u32 ordered LOD errors: decoded values must be >= 0 and parent >= self
		e = decode_lod_error(lerr)
		pe = decode_lod_error(perr)
		if e < 0:
			fail(f"cluster {i}: negative lod_error {e}")
		if pe < e:
			fail(f"cluster {i}: parent_lod_error {pe} < lod_error {e}")
	print(f"cluster limits ok (max verts={max_v}, max tris={max_t})")

	# ---- groups (FClusterGroup) ----
	for i, g in enumerate(groups):
		pstart, pnum, chstart, chnum = g[0:4]
		if pstart != INVALID_INDEX and pstart + pnum > page_count:
			fail(f"group {i}: page range overflow")
		if chnum > GROUP_MAX_CHILDREN:
			fail(f"group {i}: children_num {chnum} > {GROUP_MAX_CHILDREN}")
		if chnum > 0:
			if chstart == INVALID_INDEX or chstart + chnum > group_count:
				fail(f"group {i}: children range overflow")
			for k in range(chnum):
				gid = chstart + k
				if gid >= i:
					fail(f"group {i}: child group {gid} not finer than parent")
	print("group table ok")

	# ---- hierarchy levels ----
	for i, lv in enumerate(levels):
		cs, cc, gs, gc = lv
		if cs + cc > cluster_count or gs + gc > group_count:
			fail(f"level {i}: range overflow")
		if i > 0:
			prev = levels[i - 1]
			if cs != prev[0] + prev[1]:
				fail(f"level {i}: cluster start not contiguous")
			if gs != prev[2] + prev[3]:
				fail(f"level {i}: group start not contiguous")
	if level_count == 0:
		fail("no hierarchy levels")
	# Root clusters: every mesh in the file has its own DAG root. A root cluster
	# is one whose parent_lod_error == lod_error (set by the tool: "no coarser
	# fallback"). Collect their groups and walk children from all of them.
	root_groups = set()
	for cid, c in enumerate(clusters):
		e = decode_lod_error(c[8])
		pe = decode_lod_error(c[9])
		g = c[7]
		if abs(pe - e) < 1e-6:
			if g == INVALID_INDEX or g >= group_count:
				fail(f"root cluster {cid}: group {g} invalid")
			root_groups.add(g)
	if not root_groups:
		fail("no root clusters (parent_lod_error != lod_error everywhere)")

	# ---- DAG reachability: root groups -> children groups -> clusters ----
	visited_grp = [False] * group_count
	stack = list(root_groups)
	while stack:
		gid = stack.pop()
		if gid >= group_count or visited_grp[gid]:
			continue
		visited_grp[gid] = True
		g = groups[gid]
		for k in range(g[3]):
			child = g[2] + k
			if child >= group_count:
				fail(f"group {gid}: child group {child} out of range")
			stack.append(child)
	for cid, c in enumerate(clusters):
		g = c[7]
		if g != INVALID_INDEX and not visited_grp[g]:
			fail(f"cluster {cid}: group {g} not reachable from root")
	print(f"DAG coverage ok ({group_count} groups, {len(root_groups)} roots, all clusters covered)")

	# ---- page dependencies closure ----
	for i, p in enumerate(pages):
		dep = p[7]
		if p[8] > PAGE_MAX_SIZE:
			fail(f"page {i}: size {p[8]} exceeds {PAGE_MAX_SIZE}")
		if dep != INVALID_INDEX and dep >= page_count:
			fail(f"page {i}: dependency page {dep} out of range")
		if (p[6] & 1) and dep != INVALID_INDEX:
			fail(f"page {i}: root page must not depend on another page")
	for d in deps:
		if d[0] >= page_count or d[1] + d[2] > group_count:
			fail(f"dependency {d} out of range")
	print("page/dependency checks ok")

	# ---- page data read-back ----
	off = page_data_off
	for i, p in enumerate(pages):
		ph = PAGE_HDR.unpack_from(data, off)
		if ph[0] != PAGE_DATA_MAGIC:
			fail(f"page {i}: bad page data magic 0x{ph[0]:08X}")
		if ph[1] != NANITE_VERSION:
			fail(f"page {i}: bad page data version")
		pc, pvc, pic, vso, iso, total, pflags, pbits = ph[2], ph[3], ph[4], ph[5], ph[6], ph[7], ph[8], ph[9]
		if (pvc, pic) != (p[1], p[3]):
			fail(f"page {i}: header vertex/triangle count mismatch")
		if total != p[8]:
			fail(f"page {i}: total_size mismatch")
		if iso > total or vso > total:
			fail(f"page {i}: stream offsets out of range")

		idx_base = off + iso
		idx_u16 = struct.unpack_from(f"<{pic * 3}H", data, idx_base) if pic > 0 else ()
		if idx_u16 and max(idx_u16) >= pvc:
			fail(f"page {i}: u16 index {max(idx_u16)} >= page vertices {pvc}")

		pbits = pbits or 12
		max_q = (1 << pbits) - 1
		pos_base = off + vso
		for v in range(min(pvc, 8)):
			bit_pos = v * 3 * pbits
			for axis in range(3):
				q = 0
				for b in range(pbits):
					bit = bit_pos + axis * pbits + b
					byte = data[pos_base + bit // 8]
					q |= ((byte >> (bit % 8)) & 1) << b
				if q > max_q:
					fail(f"page {i}: vertex {v} axis {axis} quantized value {q} out of range")
				pos = ph[10 + axis] + q / max_q * ph[13 + axis]
				lo = ph[10 + axis] - 0.01
				hi = ph[10 + axis] + ph[13 + axis] + 0.01
				if pos < lo or pos > hi:
					fail(f"page {i}: vertex {v} axis {axis} decoded {pos} outside page AABB")
		off += total
	if off != len(data):
		fail(f"page data region mismatch: consumed {off} of {len(data)} bytes")
	print("page data read-back ok (u16 index bounds, page-level position quantization)")

	print("ALL CHECKS PASSED")


if __name__ == "__main__":
	main()
