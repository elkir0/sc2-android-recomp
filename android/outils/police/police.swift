// police.swift <ttf> <largeur> <hauteur> : en-tête C d'une police matricielle 1 bit (ASCII 32..95), rendue par CoreText
import Foundation
import CoreText
import CoreGraphics
let a = CommandLine.arguments
let url = URL(fileURLWithPath: a[1]) as CFURL
let W = Int(a[2])!, H = Int(a[3])!
let desc = (CTFontManagerCreateFontDescriptorsFromURL(url) as! [CTFontDescriptor])[0]
let font = CTFontCreateWithFontDescriptor(desc, CGFloat(H) * 0.78, nil)
print("/* Généré par police.swift (CoreText) depuis LiberationSans-Bold.ttf (licence SIL OFL, android/assets/polices) :")
print(" * glyphes ASCII 32..95, \(W)x\(H), 1 bit par pixel, une ligne = un uint16_t (bit 15 = colonne 0). */")
print("#define BARRE_POLICE_L \(W)\n#define BARRE_POLICE_H \(H)")
print("static const uint16_t barre_police[64][\(H)] = {")
let cs = CGColorSpaceCreateDeviceGray()
for c in 32...95 {
    let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W, space: cs, bitmapInfo: 0)!
    ctx.setFillColor(gray: 0, alpha: 1); ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
    let s = String(UnicodeScalar(c)!)
    let attr = NSAttributedString(string: s, attributes: [kCTFontAttributeName as NSAttributedString.Key: font,
        kCTForegroundColorFromContextAttributeName as NSAttributedString.Key: true])
    let line = CTLineCreateWithAttributedString(attr)
    let b = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    ctx.setFillColor(gray: 1, alpha: 1)
    ctx.textPosition = CGPoint(x: (CGFloat(W) - b.width) / 2 - b.minX, y: CGFloat(H) * 0.18)
    CTLineDraw(line, ctx)
    let p = ctx.data!.bindMemory(to: UInt8.self, capacity: W * H)
    var lignes: [String] = []
    for y in 0..<H {               // CoreGraphics : origine en bas → on lit de haut en bas
        var v: UInt16 = 0
        for x in 0..<W where p[y * W + x] > 110 { v |= UInt16(0x8000) >> UInt16(x) }
        lignes.append(String(format: "0x%04X", v))
    }
    let nom = c == 92 ? "antislash" : (c == 32 ? "espace" : s)
    print("    { \(lignes.joined(separator: ", ")) },   /* \(c) \(nom == "*/" ? "" : nom) */")
}
print("};")
