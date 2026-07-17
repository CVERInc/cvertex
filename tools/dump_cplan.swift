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
print("//   NOTE: that \"R U F L' B U'\" is cubeconjure's own comment and it mis-decodes its")
print("//   last two moves — raw 9 = D (not B) and raw 1 = U2 (not U'). The move NUMBERS are")
print("//   the truth and are what everything below uses; see DANCE for the real decode.")
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

// MARK: - Dance states
//
// The start-screen show applies danceSeq to the start cube. That's a fixed, finite walk:
// state 0 = the start cube (solved + startScrambleMoves), then one state per dance move.
// We do NOT reimplement a cube: the 13 facelet strings come out of cubeconjure's REAL
// CubieCube/Move/FaceletCube. `swift file.swift` only ever runs one file, so we shell out
// to swiftc against those exact source files and read the strings back.

let cubeconjure = ProcessInfo.processInfo.environment["CUBECONJURE"]
    ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Developer/cubeconjure").path
let solverDir = cubeconjure + "/Sources/CubeConjure/Solver"

func die(_ msg: String) -> Never {
    FileHandle.standardError.write(("dump_cplan: \(msg)\n").data(using: .utf8)!)
    exit(1)
}

/// The 13 facelet strings, straight from cubeconjure's own solver library.
func faceletStates() -> [String] {
    let tmp = NSTemporaryDirectory() + "/dump_cplan_\(getpid())"
    try? FileManager.default.createDirectory(atPath: tmp, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(atPath: tmp) }

    // Mirrors Cube3DController.startScramble (Cube3DView.swift) exactly, then walks danceSeq.
    let driver = """
    let solved = "UUUUUUUUURRRRRRRRRFFFFFFFFFDDDDDDDDDLLLLLLLLLBBBBBBBBB"
    var cube = try FaceletCube.toCubie(solved)
    for raw in \(CPlan.startScrambleMoves) {
        guard let m = Move(rawValue: raw) else { fatalError("bad scramble move \\(raw)") }
        cube = cube.applying(m)
    }
    print(FaceletCube.toFacelets(cube))
    for raw in \(CPlan.danceSeq) {
        guard let m = Move(rawValue: raw) else { fatalError("bad dance move \\(raw)") }
        cube = cube.applying(m)
        print(FaceletCube.toFacelets(cube))
    }
    """
    let mainPath = tmp + "/main.swift"   // must be main.swift for top-level code
    try? driver.write(toFile: mainPath, atomically: true, encoding: .utf8)

    let sources = ["CubieCube.swift", "Moves.swift", "FaceletCube.swift"].map { solverDir + "/" + $0 }
    for s in sources where !FileManager.default.fileExists(atPath: s) { die("missing \(s)") }

    let bin = tmp + "/states"
    let build = Process()
    build.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    build.arguments = ["swiftc", "-O", "-o", bin, mainPath] + sources
    build.standardOutput = FileHandle.nullDevice
    try? build.run()
    build.waitUntilExit()
    guard build.terminationStatus == 0 else { die("swiftc failed (status \(build.terminationStatus))") }

    let run = Process()
    run.executableURL = URL(fileURLWithPath: bin)
    let pipe = Pipe()
    run.standardOutput = pipe
    try? run.run()
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    run.waitUntilExit()
    guard run.terminationStatus == 0 else { die("state driver failed (status \(run.terminationStatus))") }

    let out = String(data: data, encoding: .utf8)!.split(separator: "\n").map(String.init)
    guard out.count == CPlan.danceSeq.count + 1 else {
        die("expected \(CPlan.danceSeq.count + 1) states, got \(out.count)")
    }
    for s in out where s.count != 54 { die("state is \(s.count) facelets, not 54") }
    return out
}

/// Copied VERBATIM from Cube3DController.cell (Cube3DView.swift) — it is private there,
/// and it is a pure static function. Re-deriving the geometry is exactly the mistake to
/// avoid, so this is a literal copy, comments and all.
///
/// Facelet index → (cubie x,y,z, SCNBox material index). SceneKit box materials:
/// 0 +Z, 1 +X, 2 −Z, 3 −X, 4 +Y, 5 −Y. Orientation: U=+Y, F=+Z, R=+X.
/// Derived face by face from reading order (r = row top→bottom, c = col L→R).
func cell(_ i: Int) -> (x: Int, y: Int, z: Int, mat: Int) {
    let f = i / 9, r = (i % 9) / 3, c = (i % 9) % 3
    switch f {
    case 0: return (c - 1, 1, r - 1, 4)      // U +Y  (top row = back)
    case 1: return (1, 1 - r, 1 - c, 1)      // R +X  (left col = front +Z)
    case 2: return (c - 1, 1 - r, 1, 0)      // F +Z
    case 3: return (c - 1, -1, 1 - r, 5)     // D −Y  (top row = front)
    case 4: return (-1, 1 - r, c - 1, 3)     // L −X  (left col = back −Z)
    default: return (1 - c, 1 - r, -1, 2)    // B −Z  (left col = right +X)
    }
}

/// Copied VERBATIM from Cube3DController.faceToSlab: face 0..5 (U R F D L B) →
/// (axis 0=x/1=y/2=z, layer −1/0/+1).
func faceToSlab(_ face: Int) -> (axis: Int, layer: Int) {
    switch face {
    case 0: return (1, 1)    // U +Y
    case 1: return (0, 1)    // R +X
    case 2: return (2, 1)    // F +Z
    case 3: return (1, -1)   // D −Y
    case 4: return (0, -1)   // L −X
    default: return (2, -1)  // B −Z
    }
}

/// Signed quarter turns about the **+axis**, right-hand rule. This is cubeconjure's own
/// two-step chain, copied rather than re-derived (see the DANCE_QT comment in the output):
///   animate():     qt = layer < 0 ? (4 - quarterTurns) : quarterTurns
///   animateSlab(): signed = qt == 1 ? -1 : (qt == 2 ? -2 : 1)
func signedQuarterTurns(face: Int, quarterTurns: Int) -> Int {
    let (_, layer) = faceToSlab(face)
    let qt = layer < 0 ? (4 - quarterTurns) : quarterTurns
    return qt == 1 ? -1 : (qt == 2 ? -2 : 1)
}

// buildCubies(): index = ((x+1)*3 + (y+1))*3 + (z+1).
func cubieIndex(_ x: Int, _ y: Int, _ z: Int) -> Int { ((x + 1) * 3 + (y + 1)) * 3 + (z + 1) }
func cubieCoord(_ n: Int) -> (x: Int, y: Int, z: Int) { (n / 9 - 1, (n / 3) % 3 - 1, n % 3 - 1) }

let letters: [Character] = ["U", "R", "F", "D", "L", "B"]
let states = faceletStates()
let nStates = states.count

// states[s][cubie][mat] — 0 = black plastic, 1..6 = U R F D L B.
var cubeStates = [[[Int]]](repeating: [[Int]](repeating: [Int](repeating: 0, count: 6), count: 27),
                           count: nStates)
for (s, facelets) in states.enumerated() {
    let f = Array(facelets)
    for i in 0..<54 {
        let p = cell(i)
        guard let colour = letters.firstIndex(of: f[i]) else { die("bad facelet '\(f[i])'") }
        cubeStates[s][cubieIndex(p.x, p.y, p.z)][p.mat] = colour + 1
    }
}

// MARK: - Checks (hard failures — a silent wrong table is worse than no table)

var checks: [String] = []

// danceSeq is self-inverse: the last state must equal state 0, element for element.
let selfInverse = cubeStates[nStates - 1] == cubeStates[0]
checks.append("state[\(nStates - 1)] == state[0]: \(selfInverse ? "YES" : "NO")")
if !selfInverse { warnings.append("danceSeq is NOT self-inverse: state[\(nStates - 1)] != state[0]") }

let warningsBeforeColourCheck = warnings.count
for s in 0..<nStates {
    var counts = [Int](repeating: 0, count: 7)
    for c in 0..<27 { for m in 0..<6 { counts[cubeStates[s][c][m]] += 1 } }
    let coloured = counts[1...6].reduce(0, +)
    if coloured != 54 { warnings.append("state \(s): \(coloured) coloured faces, expected 54") }
    for k in 1...6 where counts[k] != 9 {
        warnings.append("state \(s): colour \(letters[k - 1]) appears \(counts[k])×, expected 9")
    }
}
checks.append("every state: 9 of each colour 1..6, 54 non-zero faces: "
              + (warnings.count == warningsBeforeColourCheck ? "YES (all \(nStates) states)" : "NO"))

let core = cubieIndex(0, 0, 0)
let coreBlack = cubeStates.allSatisfy { $0[core].allSatisfy { $0 == 0 } }
checks.append("core cubie (index \(core), x=y=z=0) all 6 faces = 0: \(coreBlack ? "YES" : "NO")")
if !coreBlack { warnings.append("core cubie \(core) is not fully black") }

// Corners 3 stickers, edges 2, centres 1, core 0 — in every state.
var kinds = [Int: Int]()
for c in 0..<27 {
    let counts = Set((0..<nStates).map { s in cubeStates[s][c].filter { $0 != 0 }.count })
    guard counts.count == 1, let n = counts.first else {
        warnings.append("cubie \(c): sticker count varies across states (\(counts.sorted()))")
        continue
    }
    let p = cubieCoord(c)
    let expect = [p.x, p.y, p.z].filter { $0 != 0 }.count
    if n != expect { warnings.append("cubie \(c) at \(p): \(n) stickers, geometry says \(expect)") }
    kinds[n, default: 0] += 1
}
checks.append("corners(3 faces)=\(kinds[3] ?? 0) edges(2)=\(kinds[2] ?? 0) "
              + "centres(1)=\(kinds[1] ?? 0) core(0)=\(kinds[0] ?? 0)  [expect 8/12/6/1]")

// The sign check that matters: DANCE_AXIS/DANCE_QT are only right if physically rotating
// DANCE_LAYER's 9 cubies by DANCE_QT × 90° about +DANCE_AXIS (RH rule) turns state s into
// state s+1. The states come from the real solver, so this closes the loop on the signs
// WITHOUT trusting the derivation. Everything below is generic rotation, not cube logic.

/// Copied VERBATIM from Cube3DController.matNormal (Cube3DView.swift):
/// outward normal for an SCNBox material index (0 +Z, 1 +X, 2 −Z, 3 −X, 4 +Y, 5 −Y).
func matNormal(_ mat: Int) -> (Int, Int, Int) {
    switch mat {
    case 0: return (0, 0, 1)
    case 1: return (1, 0, 0)
    case 2: return (0, 0, -1)
    case 3: return (-1, 0, 0)
    case 4: return (0, 1, 0)
    default: return (0, -1, 0)
    }
}
func matForNormal(_ n: (Int, Int, Int)) -> Int {
    (0..<6).first { matNormal($0) == n } ?? -1
}

/// Rotate an integer vector by `qt` × 90° about +axis, right-hand rule.
func rotate(_ v: (Int, Int, Int), axis: Int, qt: Int) -> (Int, Int, Int) {
    var (x, y, z) = v
    for _ in 0..<((qt % 4) + 4) % 4 {          // one +90° RH step at a time
        switch axis {
        case 0: (y, z) = (-z, y)               // about +X: y→z
        case 1: (z, x) = (-x, z)               // about +Y: z→x
        default: (x, y) = (-y, x)              // about +Z: x→y
        }
    }
    return (x, y, z)
}

var transitionsOK = 0
for s in 0..<(nStates - 1) {
    let raw = CPlan.danceSeq[s]
    let (axis, layer) = faceToSlab(raw / 3)
    let qt = signedQuarterTurns(face: raw / 3, quarterTurns: raw % 3 + 1)
    var next = cubeStates[s]                                   // untouched cubies stay put
    for c in 0..<27 {
        let p = cubieCoord(c)
        guard (axis == 0 ? p.x : axis == 1 ? p.y : p.z) == layer else { continue }
        let np = rotate((p.x, p.y, p.z), axis: axis, qt: qt)
        for m in 0..<6 {
            let nm = matForNormal(rotate(matNormal(m), axis: axis, qt: qt))
            next[cubieIndex(np.0, np.1, np.2)][nm] = cubeStates[s][c][m]
        }
    }
    if next == cubeStates[s + 1] { transitionsOK += 1 }
    else { warnings.append("move \(s) (raw \(raw)): axis \(axis) qt \(qt) does NOT map state \(s) → state \(s + 1) — SIGN ERROR") }
}
checks.append("DANCE_AXIS/DANCE_QT reproduce every state transition: "
              + "\(transitionsOK)/\(nStates - 1)")

// MARK: - Emit dance data

print("")
print("// ---- start-screen dance ----------------------------------------------------")
print("//")
print("// Raw move encoding: raw = face*3 + (quarterTurns-1); face order U R F D L B;")
print("// quarterTurns 1/2/3 = 90° CW / 180° / 90° CCW seen from OUTSIDE that face.")
print("//   decode: face = raw/3, quarterTurns = raw%3 + 1")
print("// The 12 dance moves are: "
      + CPlan.danceSeq.map { raw -> String in
            let f = raw / 3, qt = raw % 3 + 1
            return "\(letters[f])" + (qt == 1 ? "" : qt == 2 ? "2" : "'")
        }.joined(separator: " "))
print("static const uint8_t DANCE[\(CPlan.danceSeq.count)] = { "
      + CPlan.danceSeq.map(String.init).joined(separator: ", ") + " };")
print("")
print("// The 9 cubie indices in each dance move's turning layer (cubeconjure's own")
print("// numbering: ((x+1)*3 + (y+1))*3 + (z+1)), ascending.")
print("static const uint8_t DANCE_LAYER[\(CPlan.danceSeq.count)][9] = {")
for raw in CPlan.danceSeq {
    let (axis, layer) = faceToSlab(raw / 3)
    let layerCubies = (0..<27).filter { n in
        let p = cubieCoord(n)
        return (axis == 0 ? p.x : axis == 1 ? p.y : p.z) == layer
    }
    if layerCubies.count != 9 { warnings.append("move \(raw): layer has \(layerCubies.count) cubies") }
    let f = raw / 3, qt = raw % 3 + 1
    print("  { " + layerCubies.map { String(format: "%2d", $0) }.joined(separator: ", ")
          + " },   // \(letters[f])\(qt == 1 ? "" : qt == 2 ? "2" : "'")")
}
print("};")
print("")
print("// Rotation axis of each dance move: 0=x, 1=y, 2=z.")
print("// From cubeconjure's faceToSlab(): U=+Y/+1  R=+X/+1  F=+Z/+1  D=−Y/−1  L=−X/−1  B=−Z/−1")
print("// (its orientation is U=+Y, F=+Z, R=+X), so the axis is the face's axis and the")
print("// layer is its sign.")
print("static const int8_t DANCE_AXIS[\(CPlan.danceSeq.count)] = { "
      + CPlan.danceSeq.map { String(faceToSlab($0 / 3).axis) }.joined(separator: ", ") + " };")
print("")
print("// Signed quarter turns about the +axis named by DANCE_AXIS, right-hand rule:")
print("// negative = RH-negative (= CW seen from outside the +axis). ±1 = quarter, −2 = half.")
print("//")
print("// HOW THE SIGN IS DERIVED (a sign error here is invisible until it isn't), taken")
print("// from cubeconjure's own two-step chain rather than re-derived:")
print("//   1. animate(face:quarterTurns:) turns the standard face notation into a slab turn:")
print("//        let (axis, layer) = faceToSlab(face)")
print("//        let qt = layer < 0 ? (4 - quarterTurns) : quarterTurns")
print("//      D/L/B sit on the −axis and are measured CW from outside the −axis, i.e. the")
print("//      OPPOSITE sense to animateSlab's one uniform convention — hence the 1↔3 flip.")
print("//   2. animateSlab(axis:layer:quarterTurns:) rotates ALL slabs about the +axis:")
print("//        let signed = qt == 1 ? -1 : (qt == 2 ? -2 : 1);  angle = signed * (π/2)")
print("//      \"CW seen from outside the +axis is a NEGATIVE rotation about it (RH rule).\"")
print("// So: DANCE_QT = signed, and rotating DANCE_LAYER's 9 cubies by DANCE_QT * 90°")
print("// about +DANCE_AXIS (RH rule) reproduces exactly what the app draws — and lands on")
print("// the next CUBE_STATES entry.")
print("static const int8_t DANCE_QT[\(CPlan.danceSeq.count)] = { "
      + CPlan.danceSeq.map { String(signedQuarterTurns(face: $0 / 3, quarterTurns: $0 % 3 + 1)) }
            .joined(separator: ", ") + " };")

// MARK: - Emit states

print("")
print("// ---- sticker colours per dance state ---------------------------------------")
print("//")
print("// \(nStates) states × 27 cubies × 6 faces. 0 = black plastic, 1..6 = U R F D L B.")
print("// State 0 = the start cube (solved + startScrambleMoves); state s = state 0 with")
print("// DANCE[0..<s] applied. danceSeq is self-inverse, so state \(nStates - 1) == state 0"
      + " (asserted: \(selfInverse ? "holds" : "FAILS")).")
print("//")
print("// Cubie index: cubeconjure's buildCubies() numbering, ((x+1)*3 + (y+1))*3 + (z+1).")
print("// Face index: cubeconjure's SCNBox material order — 0 +Z, 1 +X, 2 −Z, 3 −X, 4 +Y, 5 −Y.")
print("// A face with no sticker (interior) is 0: each cubie carries stickers only on its")
print("// true outer faces, the rest black plastic.")
print("//")
print("// State 0, as the six faces read U R F D L B (top-left → bottom-right):")
for f in 0..<6 {
    let rows = (0..<3).map { r in (0..<3).map { c in String(Array(states[0])[f * 9 + r * 3 + c]) }.joined(separator: " ") }
    print("//   \(letters[f]):  \(rows[0])   |   \(rows[1])   |   \(rows[2])")
}
print("static const uint8_t CUBE_STATES[\(nStates)][27][6] = {")
for s in 0..<nStates {
    print("  {   // state \(s)" + (s == 0 ? " — the start cube" : " — after \(letters[CPlan.danceSeq[s-1] / 3])"
          + (CPlan.danceSeq[s-1] % 3 == 0 ? "" : CPlan.danceSeq[s-1] % 3 == 1 ? "2" : "'")))
    for c in 0..<27 {
        let p = cubieCoord(c)
        print("    { " + cubeStates[s][c].map { String($0) }.joined(separator: ", ")
              + " },  // \(String(format: "%2d", c)) (\(p.x >= 0 ? " " : "")\(p.x),\(p.y >= 0 ? " " : "")\(p.y),\(p.z >= 0 ? " " : "")\(p.z))")
    }
    print("  },")
}
print("};")

// MARK: - Report

var report = "checks (dump_cplan):\n"
for c in checks { report += "  \(c)\n" }
report += "  state 0, six 3×3 grids (U R F D L B):\n"
for f in 0..<6 {
    let rows = (0..<3).map { r in (0..<3).map { c in String(Array(states[0])[f * 9 + r * 3 + c]) }.joined(separator: " ") }
    report += "    \(letters[f]):  \(rows[0])   \(rows[1])   \(rows[2])\n"
}
FileHandle.standardError.write(report.data(using: .utf8)!)

if !warnings.isEmpty {
    FileHandle.standardError.write(("RED FLAG:\n" + warnings.joined(separator: "\n") + "\n").data(using: .utf8)!)
    exit(1)
}
