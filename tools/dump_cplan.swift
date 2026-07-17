#!/usr/bin/env swift
// Dumps cubeconjure's C-logo layout plan as constant C data.
//
// Source of truth: ~/Developer/cubeconjure/Sources/CubeConjureApp/Cube3DView.swift
// The colorPlan() cluster there is pure geometry + hand-authored spec tables — no live
// SceneKit state — so it is copied verbatim below with SCNVector3 replaced by a trivial
// three-Double struct and `cubies.indices` replaced by `0..<27` (buildCubies() always
// makes exactly 27, and colorPlan only ever uses the indices + home[]).
//
// Run:  swift tools/dump_cplan.swift > /tmp/cplan.h

import Foundation
import simd

// MARK: - SceneKit stand-in

struct SCNVector3 {
    var x: Double, y: Double, z: Double
    init(_ x: Double, _ y: Double, _ z: Double) { self.x = x; self.y = y; self.z = z }
}
typealias CGFloatish = Double

// MARK: - Copied cluster (Cube3DController)

final class CPlan {
    // buildCubies(): 27 cubies, index = ((x+1)*3 + (y+1))*3 + (z+1), home = pos * gap.
    private var home: [SCNVector3] = []
    private let gap: Double = 1.04             // centre-to-centre spacing
    private let cubieCount = 27
    private var cubieIndices: Range<Int> { 0..<cubieCount }

    init() {
        for x in -1...1 {
            for y in -1...1 {
                for z in -1...1 {
                    home.append(SCNVector3(Double(x) * gap, Double(y) * gap, Double(z) * gap))
                }
            }
        }
    }

    /// The "C", on a 4-wide × 5-tall grid (x→right, y→down), opening right.
    static let cCells: [(Int, Int)] = [
        (0, 0), (1, 0), (2, 0), (3, 0),
        (0, 1), (1, 1),
        (0, 2), (1, 2),
        (0, 3), (1, 3),
        (0, 4), (1, 4), (2, 4), (3, 4),
    ]

    static let startScrambleMoves: [Int] = [3, 0, 6, 14, 10, 5, 8, 0]

    /// A fixed move dance: K moves out, the same K undone.
    static let danceSeq: [Int] = {
        let fwd = [3, 0, 6, 14, 9, 1]                       // R U F L' B U'  (raw = face*3 + qt-1)
        let undo = fwd.reversed().map { ($0 / 3) * 3 + (2 - $0 % 3) }   // inverse, reversed
        return fwd + undo
    }()

    private func stickerCount(_ n: Int) -> Int {
        let p = home[n]
        return [p.x, p.y, p.z].filter { abs(Int(($0 / gap).rounded())) == 1 }.count
    }

    /// Grid position of C cell `cell` (x→right, y→down), centred.
    private func cPos(_ cell: (Int, Int)) -> SCNVector3 {
        let gridW: Double = 3, gridH: Double = 4, cGap: Double = 1.06
        return SCNVector3((Double(cell.0) - gridW / 2) * cGap,
                          -(Double(cell.1) - gridH / 2) * cGap, 0)
    }

    static let identityQ = simd_quatf(angle: 0, axis: [0, 1, 0])
    static let camPos = SIMD3<Float>(5, 4.2, 6.6)
    static let camDir = simd_normalize(SIMD3<Float>(5, 4.2, 6.6))
    static let logoScale: Double = 280.0 / 460.0

    /// The 24 rotational symmetries of a cube (signed-permutation matrices, det +1).
    static let symmetries: [simd_float3x3] = {
        let axis = [SIMD3<Float>(1, 0, 0), SIMD3<Float>(0, 1, 0), SIMD3<Float>(0, 0, 1)]
        let perms = [[0, 1, 2], [0, 2, 1], [1, 0, 2], [1, 2, 0], [2, 0, 1], [2, 1, 0]]
        var out: [simd_float3x3] = []
        for p in perms {
            for s in 0..<8 {
                let sx: Float = (s & 1) == 0 ? 1 : -1
                let sy: Float = (s & 2) == 0 ? 1 : -1
                let sz: Float = (s & 4) == 0 ? 1 : -1
                let m = simd_float3x3(axis[p[0]] * sx, axis[p[1]] * sy, axis[p[2]] * sz)
                if abs(simd_determinant(m) - 1) < 0.01 { out.append(m) }
            }
        }
        return out
    }()

    private func stickerNormals(_ n: Int) -> [SIMD3<Float>] {
        let p = home[n]
        func c(_ v: Double) -> Float { Float(Int((v / gap).rounded())) }
        let co = SIMD3<Float>(c(p.x), c(p.y), c(p.z))
        var out: [SIMD3<Float>] = []
        if co.x != 0 { out.append(SIMD3(co.x, 0, 0)) }
        if co.y != 0 { out.append(SIMD3(0, co.y, 0)) }
        if co.z != 0 { out.append(SIMD3(0, 0, co.z)) }
        return out
    }

    private func hasCell(_ x: Int, _ y: Int) -> Bool {
        Self.cCells.contains { $0.0 == x && $0.1 == y }
    }

    private func exposedFaces(_ cell: (Int, Int)) -> [SIMD3<Float>] {
        var f: [SIMD3<Float>] = [SIMD3(0, 0, 1)]                 // +Z front
        if !hasCell(cell.0, cell.1 - 1) { f.append(SIMD3(0, 1, 0)) }   // +Y top
        if !hasCell(cell.0 + 1, cell.1) { f.append(SIMD3(1, 0, 0)) }   // +X right
        return f
    }

    private func exteriorFaces(_ cell: (Int, Int), front: Bool) -> [SIMD3<Float>] {
        var f: [SIMD3<Float>] = [front ? SIMD3(0, 0, 1) : SIMD3(0, 0, -1)]
        if !hasCell(cell.0, cell.1 - 1) { f.append(SIMD3(0, 1, 0)) }   // top
        if !hasCell(cell.0, cell.1 + 1) { f.append(SIMD3(0, -1, 0)) }  // bottom
        if !hasCell(cell.0 - 1, cell.1) { f.append(SIMD3(-1, 0, 0)) }  // left
        if !hasCell(cell.0 + 1, cell.1) { f.append(SIMD3(1, 0, 0)) }   // right
        return f
    }

    private func bestSymmetry(_ n: Int, _ faces: [SIMD3<Float>], cap: Int,
                              avoid: [SIMD3<Float>]) -> simd_float3x3 {
        let normals = stickerNormals(n)
        var best = Self.symmetries[0]
        var bestScore = -Float.greatestFiniteMagnitude
        for m in Self.symmetries {
            var onFace = 0, onAvoid = 0
            var align: Float = 0
            for nrm in normals {
                let r = m * nrm
                if avoid.contains(where: { simd_dot($0, r) > 0.9 }) { onAvoid += 1 }
                else if faces.contains(where: { simd_dot($0, r) > 0.9 }) { onFace += 1 }
                align += simd_dot(r, Self.camDir)
            }
            let shown = min(onFace, cap)
            let excess = max(0, onFace - cap)
            let score = Float(shown) * 100 - Float(excess) * 60 - Float(onAvoid) * 200 + align
            if score > bestScore { bestScore = score; best = m }
        }
        return best
    }

    static let fX = SIMD3<Float>(1, 0, 0)
    static let fY = SIMD3<Float>(0, 1, 0)
    static let fZ = SIMD3<Float>(0, 0, 1)

    private func frontColorSpec(_ c: (Int, Int)) -> [SIMD3<Float>]? {
        let X = Self.fX, Y = Self.fY, Z = Self.fZ
        switch (c.0, c.1) {
        case (0,0), (1,0), (2,0):                  return [Z, Y]      // a1 a2 a3
        case (3,0):                                return [Z, Y, X]   // a4
        case (1,1), (1,2), (1,3):                  return [Z, X]      // a6 a8 a10
        case (0,1), (0,2), (0,3), (0,4), (1,4):    return [Z]         // a5 a7 a9 a11 a12
        case (2,4):                                return [Z, Y]      // a13
        case (3,4):                                return [Z, Y, X]   // a14
        default:                                   return nil
        }
    }

    private func frontBlackSpec(_ c: (Int, Int)) -> [SIMD3<Float>] {
        let X = Self.fX, Y = Self.fY
        switch (c.0, c.1) {
        case (0,0), (1,0), (2,0), (2,4):           return [X]         // a1 a2 a3 a13
        case (0,1), (0,2), (0,3), (0,4), (1,4):    return [Y, X]      // a5 a7 a9 a11 a12
        case (1,1), (1,2), (1,3):                  return [Y]         // a6 a8 a10
        default:                                   return []          // a4 a14
        }
    }

    private func backColorSpec(_ c: (Int, Int)) -> [SIMD3<Float>]? {
        let X = Self.fX, Y = Self.fY
        switch (c.0, c.1) {
        case (0,0), (1,0), (2,0), (2,4):           return [Y]         // b1 b2 b3 b13
        case (3,0), (3,4):                         return [Y, X]      // b4 b14
        case (1,1), (1,2), (1,3):                  return [X]         // b6 b8 b10
        default:                                   return nil
        }
    }

    private func backBlackSpec(_ c: (Int, Int)) -> [SIMD3<Float>] {
        let X = Self.fX, Y = Self.fY, Z = Self.fZ
        switch (c.0, c.1) {
        case (0,0), (1,0), (2,0), (1,4), (2,4):    return [Z, X]      // b1 b2 b3 b12 b13
        case (1,2), (1,3):                         return [Z, Y]      // b8 b10
        case (3,0), (1,1), (3,4):                  return [Z]         // b4 b6 b14
        default:                                   return []
        }
    }

    /// Where every piece goes + how it's turned + which faces it colours.
    func colorPlan() -> [(idx: Int, pos: SCNVector3, quat: simd_quatf, faces: [SIMD3<Float>])] {
        let frontZ: Double = 0.52
        func same(_ a: (Int,Int), _ b: (Int,Int)) -> Bool { a.0 == b.0 && a.1 == b.1 }
        func neighbours(_ c: (Int,Int)) -> Int {
            [(0,-1),(0,1),(-1,0),(1,0)].filter { hasCell(c.0+$0.0, c.1+$0.1) }.count
        }
        func frontPos(_ c: (Int,Int)) -> SCNVector3 { let b = cPos(c); return SCNVector3(b.x, b.y, frontZ) }
        func backPos(_ c: (Int,Int)) -> SCNVector3 { let b = cPos(c); return SCNVector3(b.x, b.y, frontZ - 1.04) }

        let gap = (0, 2)
        let cells = Self.cCells.sorted { $0.1 != $1.1 ? $0.1 < $1.1 : $0.0 < $1.0 }
        let backCells = cells.filter { !same($0, gap) }
        func coveredBack(_ c: (Int,Int)) -> Bool {
            backCells.contains { same($0, (c.0, c.1-1)) } && backCells.contains { same($0, (c.0+1, c.1)) }
        }

        typealias Slot = (pos: SCNVector3, spec: [SIMD3<Float>]?, black: [SIMD3<Float>],
                          ext: [SIMD3<Float>], req: Int, covered: Bool, nb: Int)
        var slots: [Slot] = []
        for c in cells {
            let s = frontColorSpec(c)
            slots.append((frontPos(c), s, frontBlackSpec(c), exposedFaces(c), s?.count ?? -1, false, neighbours(c)))
        }
        for c in backCells {
            let s = backColorSpec(c)
            slots.append((backPos(c), s, backBlackSpec(c), exteriorFaces(c, front: false), s?.count ?? -1, coveredBack(c), neighbours(c)))
        }

        let coreIdx = cubieIndices.first { stickerCount($0) == 0 } ?? cubieCount - 1
        let coreSlotI = slots.indices.filter { slots[$0].spec == nil && slots[$0].covered }
                .max { slots[$0].nb < slots[$1].nb }
            ?? slots.indices.filter { slots[$0].spec == nil }.max { slots[$0].nb < slots[$1].nb }
            ?? (slots.count - 1)
        var plan: [(idx: Int, pos: SCNVector3, quat: simd_quatf, faces: [SIMD3<Float>])] =
            [(coreIdx, slots[coreSlotI].pos, Self.identityQ, [])]
        slots.remove(at: coreSlotI)

        var corners = cubieIndices.filter { $0 != coreIdx && stickerCount($0) == 3 }
        var edges = cubieIndices.filter { stickerCount($0) == 2 }
        var centres = cubieIndices.filter { stickerCount($0) == 1 }
        func pop(_ a: inout [Int]) -> Int? { a.isEmpty ? nil : a.removeFirst() }
        func take(_ req: Int) -> Int {
            switch req {
            case 3:  return pop(&corners) ?? pop(&edges) ?? pop(&centres) ?? coreIdx
            case 2:  return pop(&edges) ?? pop(&corners) ?? pop(&centres) ?? coreIdx
            case 1:  return pop(&centres) ?? pop(&edges) ?? pop(&corners) ?? coreIdx
            default: return pop(&corners) ?? pop(&edges) ?? pop(&centres) ?? coreIdx
            }
        }
        let ordered = slots.sorted { $0.req != $1.req ? $0.req > $1.req : $0.nb < $1.nb }
        for slot in ordered {
            let n = take(slot.req)
            let faces = slot.spec ?? slot.ext
            let cap = slot.spec?.count ?? 2
            let quat = simd_quatf(bestSymmetry(n, faces, cap: cap, avoid: slot.black))
            plan.append((n, slot.pos, quat, faces))
        }
        return plan
    }
}

// MARK: - Emit

let plan = CPlan().colorPlan()

func q15(_ v: Float) -> Int {
    Int((Double(v) * 32767.0).rounded())
}
func u10(_ v: Double) -> Int {
    Int((v * 1024.0).rounded())
}

var warnings: [String] = []

print("// generated by tools/dump_cplan.swift from cubeconjure's Cube3DView.swift. Don't hand-edit.")
print("//")
print("// startScrambleMoves = [3, 0, 6, 14, 10, 5, 8, 0]")
print("//   Real (legal) scramble that colours the start cube / C: each cubie carries")
print("//   stickers only on its true outer faces, the rest black plastic.")
print("//   The fixed moves (from solved) that build the start scramble — reused both to")
print("//   colour the start cube/C and to \"solve into\" the start cube on the way home.")
print("//")
print("// danceSeq (start screen) = \(CPlan.danceSeq)")
print("//   A fixed move dance: K moves out, the same K undone, so it ALWAYS lands back on")
print("//   the exact start-cube (and thus the explosion always yields the exact C).")
print("//     fwd  = [3, 0, 6, 14, 9, 1]                     // R U F L' B U'  (raw = face*3 + qt-1)")
print("//     undo = fwd.reversed().map { ($0/3)*3 + (2 - $0%3) }   // inverse, reversed")
print("//   Encoding: raw = face*3 + (quarterTurns-1); face order U R F D L B;")
print("//   the app calls animate(face: raw/3, quarterTurns: raw%3 + 1).")
print("//")
print("// \(plan.count) slots. x,y,z in 1/1024 world unit; m[9] row-major Q15 (1.0 = 32767).")
print("typedef struct { uint8_t cubie; int16_t x, y, z; int16_t m[9]; } CSlot;")
print("static const CSlot C_PLAN[\(plan.count)] = {")
for e in plan {
    let m = simd_float3x3(e.quat)
    var row: [Int] = []
    for r in 0..<3 {
        for c in 0..<3 {
            let v = m[c][r]           // columns[c][r] = row-major element (r,c)
            let ok = abs(v) < 0.01 || abs(abs(v) - 1) < 0.01
            if !ok { warnings.append("cubie \(e.idx): m[\(r)][\(c)] = \(v) is not near -1/0/+1") }
            row.append(q15(v))
        }
    }
    let ms = "\(row[0]),\(row[1]),\(row[2]), \(row[3]),\(row[4]),\(row[5]), \(row[6]),\(row[7]),\(row[8])"
    print(String(format: "  { %2d, %5d, %5d, %5d, { %@ } },", e.idx,
                 u10(e.pos.x), u10(e.pos.y), u10(e.pos.z), ms))
}
print("};")

if !warnings.isEmpty {
    FileHandle.standardError.write(("RED FLAG:\n" + warnings.joined(separator: "\n") + "\n").data(using: .utf8)!)
}
