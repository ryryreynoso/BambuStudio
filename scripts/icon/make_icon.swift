// Draws the "3D Print" app icon: a nozzle laying down a stack of rainbow
// layers (the Full Spectrum colour mixing) on a dark rounded square.
// Usage: swift scripts/icon/make_icon.swift <out.png> [size] [ios]
// "ios" draws the same design full-bleed and opaque, as App Store icons must be (iOS rounds the
// corners itself).
import AppKit

let args = CommandLine.arguments
let outPath = args.count > 1 ? args[1] : "icon_1024.png"
let size = CGFloat(args.count > 2 ? Double(args[2])! : 1024)
let ios = args.count > 3 && args[3] == "ios"
let s = size / 1024

let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: Int(size), height: Int(size), bitsPerComponent: 8,
                    bytesPerRow: 0, space: cs,
                    bitmapInfo: (ios ? CGImageAlphaInfo.noneSkipLast : CGImageAlphaInfo.premultipliedLast).rawValue)!
if ios {
    // Scale the 824 pt macOS tile up to the whole square.
    ctx.translateBy(x: size / 2, y: size / 2)
    ctx.scaleBy(x: 1024 / 824, y: 1024 / 824)
    ctx.translateBy(x: -size / 2, y: -size / 2)
}

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(red: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: a)
}
func r(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
    CGRect(x: x * s, y: y * s, width: w * s, height: h * s)
}

// Background: macOS icon grid (824 pt tile inside 1024, radius ~185), dark navy-to-plum gradient.
let tile = r(100, 100, 824, 824)
let radius: CGFloat = ios ? 0 : 185 * s
let tilePath = CGPath(roundedRect: tile, cornerWidth: radius, cornerHeight: radius, transform: nil)
ctx.saveGState()
if !ios { ctx.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 28 * s, color: rgb(0x000000, 0.35)) }
ctx.addPath(tilePath); ctx.setFillColor(rgb(0x1a1d3a)); ctx.fillPath()
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(tilePath); ctx.clip()
let bg = CGGradient(colorsSpace: cs, colors: [rgb(0x2b1f52), rgb(0x141a33)] as CFArray, locations: [0, 1])!
ctx.drawLinearGradient(bg, start: CGPoint(x: 0, y: 924 * s), end: CGPoint(x: 0, y: 100 * s), options: [])

// Build plate.
ctx.setFillColor(rgb(0x3a4170))
ctx.addPath(CGPath(roundedRect: r(220, 216, 584, 32), cornerWidth: 17 * s, cornerHeight: 17 * s, transform: nil))
ctx.fillPath()

// Printed part: layers bottom to top, each a spectrum colour, slightly rounded.
let colors: [UInt32] = [0x7b4dff, 0x3d7bff, 0x1fc8e8, 0x2ed47a, 0xc6e83a, 0xffc93a, 0xff8a3a, 0xff4d6d]
let layerH: CGFloat = 40, gap: CGFloat = 7, baseY: CGFloat = 262
for (i, c) in colors.enumerated() {
    let y = baseY + CGFloat(i) * (layerH + gap)
    // Top layer is the one being printed: shorter, still growing toward the nozzle.
    let w: CGFloat = i == colors.count - 1 ? 250 : 420
    let x: CGFloat = 302
    let path = CGPath(roundedRect: r(x, y, w, layerH), cornerWidth: 20 * s, cornerHeight: 20 * s, transform: nil)
    ctx.addPath(path); ctx.setFillColor(rgb(c)); ctx.fillPath()
    // Soft highlight on each layer's upper half.
    ctx.addPath(CGPath(roundedRect: r(x + 10, y + layerH * 0.55, w - 20, layerH * 0.28),
                       cornerWidth: 8 * s, cornerHeight: 8 * s, transform: nil))
    ctx.setFillColor(rgb(0xffffff, 0.18)); ctx.fillPath()
}

// Nozzle above the top layer, at its growing end.
let topY = baseY + CGFloat(colors.count) * (layerH + gap) - gap
let nx: CGFloat = 552 // right end of the top layer
let body = CGMutablePath()
body.addRoundedRect(in: r(nx - 70, topY + 120, 140, 125), cornerWidth: 22 * s, cornerHeight: 22 * s)
ctx.addPath(body); ctx.setFillColor(rgb(0xd9dcea)); ctx.fillPath()
let tip = CGMutablePath()
tip.move(to: CGPoint(x: (nx - 46) * s, y: (topY + 122) * s))
tip.addLine(to: CGPoint(x: (nx + 46) * s, y: (topY + 122) * s))
tip.addLine(to: CGPoint(x: (nx + 12) * s, y: (topY + 40) * s))
tip.addLine(to: CGPoint(x: (nx - 12) * s, y: (topY + 40) * s))
tip.closeSubpath()
ctx.addPath(tip); ctx.setFillColor(rgb(0xaab0c8)); ctx.fillPath()
// Heater-block stripe.
ctx.setFillColor(rgb(0x8a91ad))
ctx.fill(r(nx - 70, topY + 175, 140, 22))
// Molten filament from nozzle to the layer, in the top layer's colour.
ctx.setFillColor(rgb(colors.last!))
ctx.addPath(CGPath(roundedRect: r(nx - 9, topY - 6, 18, 50), cornerWidth: 9 * s, cornerHeight: 9 * s, transform: nil))
ctx.fillPath()
ctx.restoreGState()

let img = ctx.makeImage()!
let rep = NSBitmapImageRep(cgImage: img)
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: outPath))
