// SPDX-License-Identifier: AGPL-3.0-only
import Foundation

/// Conservative world-space bounds validation; actual slicing validation stays in the native engine.
enum PrintVolume {
    static func contains(minimum: [Double], maximum: [Double], matrix: [Double],
                         minX: Double, minY: Double, maxX: Double, maxY: Double, height: Double) -> Bool {
        guard minimum.count == 3, maximum.count == 3, matrix.count == 16,
              (minimum + maximum + matrix + [minX, minY, maxX, maxY, height]).allSatisfy({ $0.isFinite }),
              zip(minimum, maximum).allSatisfy({ $0.0 <= $0.1 }) else { return false }
        let tolerance = 0.05
        for x in [minimum[0], maximum[0]] {
            for y in [minimum[1], maximum[1]] {
                for z in [minimum[2], maximum[2]] {
                    let worldX = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12]
                    let worldY = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13]
                    let worldZ = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]
                    guard worldX >= minX - tolerance, worldX <= maxX + tolerance,
                          worldY >= minY - tolerance, worldY <= maxY + tolerance,
                          worldZ >= -tolerance, worldZ <= height + tolerance else { return false }
                }
            }
        }
        return true
    }
}
