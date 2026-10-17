#!/usr/bin/env swift

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// Draws OpenSCP's application icon at 1024 pixels in each of its styles: the
// standard icon, and the macOS tile with a light or a dark background. The
// drawing is made of two public-domain Tango drawings, the padlock of
// Rocket000's lock.svg and two copies of the arrow of Jakub Steiner's
// edit-redo.svg; every path, gradient and opacity below is copied from them.
// The padlock and the arrows share one scale, so their outlines are equally
// wide, the arrows are each other's half-turn about the padlock body's
// center, and the drawing is centered on the canvas. Each image is drawn and
// saved in sRGB, so each color comes out as written.

// MARK: - Configuration

let canvasSize = 1024

/// Where the styles' folders go: the repository's icon folder, unless an
/// argument names another one.
let outputDirectory: URL = {
    let arguments = CommandLine.arguments
    if arguments.count > 1 {
        return URL(fileURLWithPath: NSString(string: arguments[1]).expandingTildeInPath)
    }
    return URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()
        .appendingPathComponent("../../assets/icons/app")
        .standardizedFileURL
}()

/// How the padlock and the arrows are put together.
struct Composition {
    /// The drawing's longer side, in pixels.
    let span: CGFloat
    /// The arrows' size against the padlock's. Their outlines stay as wide
    /// as the padlock's.
    let arrowScale: CGFloat
    /// How much nearer the head, in edit-redo.svg's units, the tails fade out.
    let tailShortening: CGFloat
    /// How much of lock.svg's opacity the padlock's grooves keep.
    let grooveOpacity: CGFloat
    /// Whether the padlock casts lock.svg's shadow on the ground. Otherwise
    /// one soft shadow falls under the whole drawing.
    let castsGroundShadow: Bool
    /// Whether the arrow in front, which is upside down, has its head's gloss
    /// turned back, so the head is lit from above like the rest.
    let relightsFrontHead: Bool
}

/// The drawings as their files have them.
let freeStanding = Composition(
    span: 876, arrowScale: 1, tailShortening: 0, grooveOpacity: 1,
    castsGroundShadow: true, relightsFrontHead: false
)
/// On a tile the padlock fills more of the shape, every piece is lit from
/// above, and the grooves, which blur below 64 pixels, are fainter.
let onTile = Composition(
    span: 690, arrowScale: 0.95, tailShortening: 1, grooveOpacity: 0.5,
    castsGroundShadow: false, relightsFrontHead: true
)

struct Style {
    let name: String
    /// The tile's colors at its top and at its bottom, for a style on a tile.
    let tile: (top: UInt32, bottom: UInt32)?
    let composition: Composition
}

let styles = [
    Style(name: "standard", tile: nil, composition: freeStanding),
    Style(name: "macos-light", tile: (top: 0xFFFFFF, bottom: 0xEEEEEC), composition: onTile),
    Style(name: "macos-dark", tile: (top: 0x555753, bottom: 0x2E3436), composition: onTile),
]

// MARK: - SVG Path Data

/// Builds a path from SVG path data. It reads the commands the two drawings
/// use, M, L, H, V, C and Z, absolute or relative, with implicit repeats; the
/// padlock's shadow, an arc in its file, is drawn as an ellipse instead.
func svgPath(_ data: String) -> CGPath {
    let characters = Array(data.utf8)
    var index = 0

    func isDigit(_ character: UInt8) -> Bool { character >= 0x30 && character <= 0x39 }

    func skipSeparators() {
        while index < characters.count, [0x20, 0x2C, 0x09, 0x0A, 0x0D].contains(characters[index]) {
            index += 1
        }
    }

    func number() -> CGFloat {
        skipSeparators()
        let start = index
        if index < characters.count, characters[index] == UInt8(ascii: "-") || characters[index] == UInt8(ascii: "+") {
            index += 1
        }
        var seenDot = false
        while index < characters.count {
            if isDigit(characters[index]) {
                index += 1
            } else if characters[index] == UInt8(ascii: "."), !seenDot {
                seenDot = true
                index += 1
            } else {
                break
            }
        }
        if index < characters.count, characters[index] == UInt8(ascii: "e") || characters[index] == UInt8(ascii: "E") {
            index += 1
            if index < characters.count, characters[index] == UInt8(ascii: "-") || characters[index] == UInt8(ascii: "+") {
                index += 1
            }
            while index < characters.count, isDigit(characters[index]) {
                index += 1
            }
        }
        guard let value = Double(String(decoding: characters[start..<index], as: UTF8.self)) else {
            fatalError("Bad number in SVG path data at offset \(start)")
        }
        return CGFloat(value)
    }

    let path = CGMutablePath()
    var current = CGPoint.zero
    var subpathStart = CGPoint.zero
    var command: UInt8 = 0

    while true {
        skipSeparators()
        guard index < characters.count else { break }
        let character = characters[index]
        if (character | 0x20) >= UInt8(ascii: "a") && (character | 0x20) <= UInt8(ascii: "z") {
            command = character
            index += 1
        } else if command == 0 || (command | 0x20) == UInt8(ascii: "z") {
            fatalError("SVG path data has numbers without a command at offset \(index)")
        }
        let relative = command >= UInt8(ascii: "a")

        func point() -> CGPoint {
            let x = number()
            let y = number()
            return relative ? CGPoint(x: current.x + x, y: current.y + y) : CGPoint(x: x, y: y)
        }

        switch command | 0x20 {
        case UInt8(ascii: "m"):
            current = point()
            subpathStart = current
            path.move(to: current)
            // Coordinates that follow a move draw lines.
            command = relative ? UInt8(ascii: "l") : UInt8(ascii: "L")
        case UInt8(ascii: "l"):
            current = point()
            path.addLine(to: current)
        case UInt8(ascii: "h"):
            let x = number()
            current = CGPoint(x: relative ? current.x + x : x, y: current.y)
            path.addLine(to: current)
        case UInt8(ascii: "v"):
            let y = number()
            current = CGPoint(x: current.x, y: relative ? current.y + y : y)
            path.addLine(to: current)
        case UInt8(ascii: "c"):
            let control1 = point()
            let control2 = point()
            current = point()
            path.addCurve(to: current, control1: control1, control2: control2)
        case UInt8(ascii: "z"):
            path.closeSubpath()
            current = subpathStart
        default:
            fatalError("Unsupported SVG path command \(Character(UnicodeScalar(command)))")
        }
    }
    return path
}

// The reader's own check: in this path each relative, repeated or absolute
// command leaves the pen where a mistake in reading it would show.
precondition(
    svgPath("M2 2c0 0 1 0 1 0 0 0 0 1 0 1h-3v1z").boundingBoxOfPath == CGRect(x: 0, y: 2, width: 3, height: 2),
    "The SVG path reader is broken"
)

/// An SVG `matrix(a b c d e f)`.
func matrix(_ a: CGFloat, _ b: CGFloat, _ c: CGFloat, _ d: CGFloat, _ e: CGFloat, _ f: CGFloat) -> CGAffineTransform {
    CGAffineTransform(a: a, b: b, c: c, d: d, tx: e, ty: f)
}

/// An SVG `rect` with rounded corners.
func roundedRect(_ x: CGFloat, _ y: CGFloat, _ width: CGFloat, _ height: CGFloat, rx: CGFloat, ry: CGFloat) -> CGPath {
    CGPath(
        roundedRect: CGRect(x: x, y: y, width: width, height: height),
        cornerWidth: rx,
        cornerHeight: ry,
        transform: nil
    )
}

// MARK: - Paint

guard let sRGB = CGColorSpace(name: CGColorSpace.sRGB) else {
    fatalError("Could not create the sRGB color space")
}

/// An sRGB color from `0xRRGGBB`.
func color(_ hex: UInt32, alpha: CGFloat = 1.0) -> CGColor {
    CGColor(
        srgbRed: CGFloat((hex >> 16) & 0xFF) / 255.0,
        green: CGFloat((hex >> 8) & 0xFF) / 255.0,
        blue: CGFloat(hex & 0xFF) / 255.0,
        alpha: alpha
    )
}

/// A gradient from SVG stops: offset, `0xRRGGBB` and stop opacity.
func gradient(_ stops: [(CGFloat, UInt32, CGFloat)]) -> CGGradient {
    let colors = stops.map { _, hex, alpha in color(hex, alpha: alpha) }
    guard let gradient = CGGradient(colorsSpace: sRGB, colors: colors as CFArray, locations: stops.map { $0.0 }) else {
        fatalError("Could not create CGGradient")
    }
    return gradient
}

/// An SVG paint: a flat color, or a gradient in user space with its own
/// `gradientTransform`, padded beyond its ends as SVG pads them.
enum Paint {
    case color(UInt32)
    case linear(CGGradient, from: CGPoint, to: CGPoint, transform: CGAffineTransform)
    case radial(CGGradient, center: CGPoint, radius: CGFloat, transform: CGAffineTransform)
}

func makeCanvas() -> CGContext {
    guard
        let canvas = CGContext(
            data: nil,
            width: canvasSize,
            height: canvasSize,
            bitsPerComponent: 8,
            bytesPerRow: 0,
            space: sRGB,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        )
    else {
        fatalError("Could not create the sRGB canvas")
    }
    canvas.setShouldAntialias(true)
    canvas.interpolationQuality = .high
    return canvas
}

/// Paints into a canvas whose y axis points down, as SVG's does.
struct Painter {
    let canvas: CGContext

    init(canvas: CGContext) {
        self.canvas = canvas
        canvas.translateBy(x: 0, y: CGFloat(canvasSize))
        canvas.scaleBy(x: 1, y: -1)
    }

    /// Fills `shape` with `paint`, at `opacity`.
    func fill(_ shape: CGPath, _ paint: Paint, opacity: CGFloat = 1, evenOdd: Bool = false) {
        canvas.saveGState()
        canvas.setAlpha(opacity)
        canvas.addPath(shape)
        canvas.clip(using: evenOdd ? .evenOdd : .winding)
        switch paint {
        case let .color(hex):
            canvas.setFillColor(color(hex))
            canvas.fill(shape.boundingBoxOfPath)
        case let .linear(gradient, start, end, transform):
            canvas.concatenate(transform)
            canvas.drawLinearGradient(
                gradient,
                start: start,
                end: end,
                options: [.drawsBeforeStartLocation, .drawsAfterEndLocation]
            )
        case let .radial(gradient, center, radius, transform):
            canvas.concatenate(transform)
            canvas.drawRadialGradient(
                gradient,
                startCenter: center,
                startRadius: 0,
                endCenter: center,
                endRadius: radius,
                options: [.drawsAfterEndLocation]
            )
        }
        canvas.restoreGState()
    }

    /// Strokes `shape` with `paint`, at `opacity`.
    func stroke(
        _ shape: CGPath,
        _ paint: Paint,
        width: CGFloat = 1,
        join: CGLineJoin = .miter,
        cap: CGLineCap = .butt,
        miterLimit: CGFloat = 4,
        opacity: CGFloat = 1
    ) {
        let outline = shape.copy(strokingWithWidth: width, lineCap: cap, lineJoin: join, miterLimit: miterLimit)
        fill(outline, paint, opacity: opacity)
    }

    /// Runs `draw` with `transform` applied to what it draws.
    func transformed(_ transform: CGAffineTransform, _ draw: () -> Void) {
        canvas.saveGState()
        canvas.concatenate(transform)
        draw()
        canvas.restoreGState()
    }
}

// MARK: - The Padlock (lock.svg)

let shackleData = "M10.5 20.234846V13c0-7.8701334 5.397609-11.6089934 13.520027-11.5107079C32.18664 1.5875777 37.5 5.0372782 37.5 13v7.234846c-.083947 1.703585-4.963388 1.697303-5 0V15c0-2 .638264-8.471853-8.422759-8.471853-9.123523 0-8.587252 6.511738-8.554561 8.463879v5.276498C15.3125 21.859846 10.5 21.797346 10.5 20.234846z"
let shackleTransform = CGAffineTransform(translationX: -2e-7, y: -0.987405)
let bodyRect = CGRect(x: 6.5, y: 17.5, width: 34.993771, height: 27.009277)
let innerRimRect = CGRect(x: 7.5, y: 18.5, width: 33.000904, height: 25.006882)

/// The padlock's shadow on the ground.
func drawPadlockShadow(_ painter: Painter) {
    let ellipse = CGPath(
        ellipseIn: CGRect(x: 7, y: 20.714285, width: 31.142856, height: 20.285716),
        transform: nil
    )
    painter.transformed(matrix(1.44023, 0, 0, 0.41901, -8.581573, 29.82043)) {
        painter.fill(
            ellipse,
            .radial(
                gradient([(0, 0x000000, 1), (1, 0x000000, 0)]),
                center: CGPoint(x: 22.571428, y: 30.857143),
                radius: 15.571428,
                transform: matrix(1, 0, 0, 0.65138, 0, 10.75754)
            ),
            opacity: 0.47368421
        )
    }
}

func drawPadlock(_ painter: Painter, grooveOpacity: CGFloat) {
    let body = roundedRect(bodyRect.minX, bodyRect.minY, bodyRect.width, bodyRect.height, rx: 2.2349751, ry: 2.2570534)
    painter.fill(
        body,
        .linear(
            gradient([(0, 0xEAD200, 0.71341461), (0.20999999, 0xFFEC41, 1), (0.83999997, 0xE2CC00, 1), (1, 0xC3AF00, 1)]),
            from: CGPoint(x: 6.72682, y: 32.161697),
            to: CGPoint(x: 40.938126, y: 32.161697),
            transform: matrix(1, 0, 0, 1.08988, 0, -5.10979)
        ),
        evenOdd: true
    )
    painter.stroke(
        body,
        .linear(
            gradient([(0, 0x7D6400, 1), (1, 0xBE9700, 1)]),
            from: CGPoint(x: 31.630468, y: 41.791817),
            to: CGPoint(x: 8.6713638, y: 25.793524),
            transform: matrix(1, 0, 0, 1.08988, 0, -3.986899)
        ),
        width: 0.99999946,
        join: .round,
        cap: .round
    )

    painter.stroke(
        roundedRect(innerRimRect.minX, innerRimRect.minY, innerRimRect.width, innerRimRect.height, rx: 1.288511, ry: 1.288511),
        .linear(
            gradient([(0, 0xFFFFFF, 1), (1, 0xFFFFFF, 0.49484536)]),
            from: CGPoint(x: 10.907269, y: 25.002281),
            to: CGPoint(x: 30.875446, y: 36.127281),
            transform: matrix(0.99911, 0, 0, 1.0958, 0.03642141, -4.139733)
        ),
        width: 1.00000012,
        join: .round,
        cap: .round,
        opacity: 0.60109289
    )

    // The grooves: ten bands, each darkest along its top.
    painter.transformed(matrix(1.0663, 0, 0, 1, -1.59099, 0)) {
        for groove in 0..<10 {
            painter.fill(
                CGPath(rect: CGRect(x: 8, y: 23 + 2 * CGFloat(groove), width: 32, height: 0.97227097), transform: nil),
                .linear(
                    gradient([(0, 0x896D00, 1), (1, 0xA18000, 0.26804122)]),
                    from: CGPoint(x: 27.8125, y: 22.739454),
                    to: CGPoint(x: 27.824219, y: 26.024691),
                    transform: matrix(1.23077, 0, 0, 0.32409, -5.538462, 14.89774 + 2 * CGFloat(groove))
                ),
                opacity: 0.5 * grooveOpacity,
                evenOdd: true
            )
        }
    }

    // The lid.
    painter.fill(
        roundedRect(7, 18, 34, 4, rx: 1.7146454, ry: 1.6610726),
        .linear(
            gradient([(0, 0xFFEC41, 1), (1, 0xC3AF00, 1)]),
            from: CGPoint(x: 24.875, y: 21),
            to: CGPoint(x: 24.75, y: 17),
            transform: CGAffineTransform(translationX: 0, y: 1)
        ),
        evenOdd: true
    )

    // The gloss over the body's upper part.
    painter.fill(
        svgPath("M39.022894 21.954401H8.9789228c-1.518102.008101-1.980695.640186-1.99632 2.380825v10.845656c5.3481802 4.75668 17.7926802-4.969999 34.0000002 0V24.314186c-.012611-1.780269-.540848-2.346047-1.959709-2.359785z"),
        .radial(
            gradient([(0, 0xFFFFFF, 1), (1, 0xFFFFFF, 0)]),
            center: CGPoint(x: 12.845672, y: 13.342374),
            radius: 17,
            transform: matrix(2.53109, 0, 0, 2.86055, -19.66786, -16.33929)
        ),
        opacity: 0.40659341,
        evenOdd: true
    )

    // The shackle, its legs in front of the lid, with a light down each leg.
    painter.transformed(shackleTransform) {
        let shackle = svgPath(shackleData)
        painter.fill(
            shackle,
            .linear(
                gradient([(0, 0xCAD0C6, 1), (0.5, 0xEAECE9, 1), (1, 0xC5CBC0, 1)]),
                from: CGPoint(x: 10.650842, y: 2.9136841),
                to: CGPoint(x: 27.192274, y: 17.470011),
                transform: CGAffineTransform(translationX: 0, y: -1.926279)
            ),
            evenOdd: true
        )
        painter.stroke(
            shackle,
            .linear(
                gradient([(0, 0x6F716D, 1), (1, 0x9EA09C, 1)]),
                from: CGPoint(x: 19.250618, y: 9.6635771),
                to: CGPoint(x: 16.198252, y: 6.0396547),
                transform: CGAffineTransform(translationX: 0, y: -1.926279)
            ),
            width: 0.99999958
        )
        painter.fill(
            svgPath("M11.496849 13.160018c.248424-4.3978382 1.416201-8.4519615 6.959268-9.9612363 1.580544 1.8244316-4.8983 1.9207621-4.997669 8.2829673l.02166 7.982511c-.234835.588388-1.920759.606565-1.983259-.049685v-6.254557z"),
            .radial(
                gradient([(0, 0xFFFFFF, 1), (1, 0xFFFFFF, 0.49484536)]),
                center: CGPoint(x: 12.184196, y: 8.5463438),
                radius: 3.6037733,
                transform: matrix(-0.44134, 3.78696, -3.2859, -0.38294, 47.91818, -38.4483)
            ),
            evenOdd: true
        )
        painter.fill(
            svgPath("M33.500368 9.7120694h2v9.6354126c-.0625 1.125-2.03125.53125-2 0V9.7120694z"),
            .radial(
                gradient([(0, 0xFFFFFF, 0), (1, 0xFFFFFF, 1)]),
                center: CGPoint(x: 30.996042, y: 13.156409),
                radius: 1.000184,
                transform: matrix(0, 14.96898, -8.27668, 0.0004, 143.8949, -454.2723)
            ),
            evenOdd: true
        )
    }

    // The light edge under the lid.
    painter.stroke(
        roundedRect(7.5, 18.634619, 33, 2.8557696, rx: 1.2001942, ry: 1.0968477),
        .linear(
            gradient([(0, 0xFFFFFF, 1), (1, 0xFFFFFF, 0)]),
            from: CGPoint(x: 21.941509, y: 21.550869),
            to: CGPoint(x: 21.941509, y: 18.037588),
            transform: matrix(1, 0, 0, 0.95192, 0, 1.975963)
        ),
        join: .round,
        cap: .round
    )
}

// MARK: - The Arrow (edit-redo.svg)

let arrowShape = svgPath("M 38.37476,45.034369 C -1.6510486,46.355509 4.6747954,12.29355 25.49479,12.49765 L 25.49479,3.1222396 L 42.143271,17.708819 L 25.49479,33.006349 C 25.49479,33.006349 25.49479,23.337969 25.49479,23.337969 C 11.43168,22.751999 7.3172614,44.770549 38.37476,45.034369 z")
/// The head's back edge and the height of its tip.
let headBackEdgeX: CGFloat = 25.49479
let headTipY: CGFloat = 17.708819
/// The head, and the middle of its back edge.
let headShape: CGPath = {
    let path = CGMutablePath()
    path.addLines(between: [
        CGPoint(x: headBackEdgeX, y: 3.1222396),
        CGPoint(x: 42.143271, y: headTipY),
        CGPoint(x: headBackEdgeX, y: 33.006349),
    ])
    path.closeSubpath()
    return path
}()
let headMiddleY: CGFloat = (3.1222396 + 33.006349) / 2

let rimShape = svgPath("M 16.92492,39.315519 C 5.2018204,33.235892 8.7371274,13.087489 26.5085,13.549959 L 26.5085,5.4508678 C 26.5085,5.4508678 40.556238,17.714589 40.556238,17.714589 L 26.5085,30.658617 C 26.5085,30.658617 26.5085,22.380979 26.5085,22.380979 C 11.66865,22.032709 12.34859,35.138579 16.92492,39.315519 z")
let rimPaint = Paint.linear(
    gradient([(0, 0xFFFFFF, 1), (1, 0xFFFFFF, 0)]),
    from: CGPoint(x: 17.060806, y: 11.39502),
    to: CGPoint(x: 12.624337, y: 12.583769),
    transform: matrix(1.813471e-16, -1.171926, 1.171926, 1.813471e-16, 1.782801, 54.10111)
)
let glossShape = svgPath("M 26.036989,4.5686095 L 36.723727,14.798241 C 29.786227,14.79824 32.036989,23.735424 25.911989,26.610424 L 25.974489,22.943609 C 10.786989,22.881109 11.661989,38.443609 22.724489,42.693609 C 3.6363414,37.811681 6.2869904,13.381109 25.911989,12.88111 L 26.036989,4.5686095 z")
let glossPaint = Paint.radial(
    gradient([(0, 0xFFFFFF, 1), (1, 0xFFFFFF, 0)]),
    center: CGPoint(x: 16.563837, y: 11.132236),
    radius: 19.0625,
    transform: matrix(-1.290127e-2, 1.685197, 1.713082, 1.311475e-2, -1.041499, -10.11571)
)

/// Draws the arrow as one layer, its outlines as wide as the padlock's. In
/// edit-redo.svg its fill and outline fade out toward the tail while its
/// highlights stay; here the same fade covers the whole arrow, so the
/// highlights leave no light streak on a dark background, and the arrow
/// casts no shadow, since one of the two is upside down.
func drawArrow(_ painter: Painter, composition: Composition, upsideDown: Bool) {
    let canvas = painter.canvas
    canvas.beginTransparencyLayer(auxiliaryInfo: nil)
    painter.fill(arrowShape, .color(0x99B00B))
    painter.stroke(arrowShape, .color(0x788600), width: 1.00000012 / composition.arrowScale)
    painter.stroke(rimShape, rimPaint, width: 0.9999997 / composition.arrowScale, miterLimit: 10, opacity: 0.69886361)

    var gloss = glossShape
    if upsideDown && composition.relightsFrontHead {
        // Upside down, the head's gloss would light it from below. It is
        // turned back about the head's middle, where the gloss's light is
        // centered, so the band's gloss still runs into the head's.
        var turnBack = CGAffineTransform(translationX: 0, y: 2 * headMiddleY).scaledBy(x: 1, y: -1)
        guard let headGloss = glossShape.intersection(headShape).copy(using: &turnBack) else {
            fatalError("Could not turn the head's gloss back")
        }
        gloss = glossShape.subtracting(headShape).union(headGloss)
    }
    painter.fill(gloss, glossPaint, opacity: 0.49431817)

    // The SVG's fade, from its fill's gradient: opaque at the first point,
    // gone at the second.
    canvas.saveGState()
    canvas.setBlendMode(.destinationIn)
    canvas.drawLinearGradient(
        gradient([(0, 0x000000, 1), (1, 0x000000, 0)]),
        start: CGPoint(x: 33, y: 35.75 - composition.tailShortening),
        end: CGPoint(x: 31.5, y: 42.5 - composition.tailShortening),
        options: [.drawsBeforeStartLocation, .drawsAfterEndLocation]
    )
    canvas.restoreGState()
    canvas.endTransparencyLayer()
}

// MARK: - Arrangement

// In lock.svg's units. The arrows turn about the body's center. The right
// arrow's head rises behind the padlock just clear of the shackle's right leg,
// and so the left arrow's head, its half-turn, reaches in over the body's
// left side; the left head's tip rests on the body's light rim.
let orbitCenter = CGPoint(x: bodyRect.midX, y: bodyRect.midY)
let shackleRightEdge: CGFloat = 37.5
/// Clear space between the shackle's right leg and the head behind it, past
/// both outlines' halves.
let headClearance: CGFloat = 1
let headOffset = CGVector(
    dx: shackleRightEdge - orbitCenter.x + 0.5 + headClearance + 0.5,
    dy: innerRimRect.maxY - orbitCenter.y
)

/// The left arrow is edit-redo's arrow upside down, its head pointing right
/// below the padlock's center; the right one is its half-turn.
func arrowPlacements(arrowScale: CGFloat) -> (left: CGAffineTransform, right: CGAffineTransform) {
    let left = CGAffineTransform(translationX: -headBackEdgeX, y: -headTipY)
        .concatenating(CGAffineTransform(scaleX: arrowScale, y: -arrowScale))
        .concatenating(CGAffineTransform(translationX: orbitCenter.x - headOffset.dx, y: orbitCenter.y + headOffset.dy))
    let right = left.concatenating(
        CGAffineTransform(a: -1, b: 0, c: 0, d: -1, tx: 2 * orbitCenter.x, ty: 2 * orbitCenter.y)
    )
    return (left, right)
}

/// Scales the drawing so its longer side, outlines included, spans the
/// composition's span, and centers it on the canvas.
func sceneTransform(_ composition: Composition) -> CGAffineTransform {
    let arrows = arrowPlacements(arrowScale: composition.arrowScale)
    let outline = { (shape: CGPath, transform: CGAffineTransform) -> CGRect in
        shape.copy(strokingWithWidth: 1, lineCap: .butt, lineJoin: .miter, miterLimit: 4, transform: transform)
            .boundingBoxOfPath
    }
    let bounds = bodyRect.insetBy(dx: -0.5, dy: -0.5)
        .union(outline(svgPath(shackleData), shackleTransform))
        .union(outline(arrowShape, arrows.left))
        .union(outline(arrowShape, arrows.right))
    let scale = composition.span / max(bounds.width, bounds.height)
    return CGAffineTransform(translationX: -bounds.midX, y: -bounds.midY)
        .concatenating(CGAffineTransform(scaleX: scale, y: scale))
        .concatenating(CGAffineTransform(translationX: CGFloat(canvasSize) / 2, y: CGFloat(canvasSize) / 2))
}

/// The padlock and its arrows on a transparent canvas. The right arrow
/// passes behind the padlock and the left one in front of it.
func drawDrawing(_ composition: Composition) -> CGImage {
    let canvas = makeCanvas()
    let painter = Painter(canvas: canvas)
    let arrows = arrowPlacements(arrowScale: composition.arrowScale)
    painter.transformed(sceneTransform(composition)) {
        if composition.castsGroundShadow {
            drawPadlockShadow(painter)
        }
        painter.transformed(arrows.right) {
            drawArrow(painter, composition: composition, upsideDown: false)
        }
        drawPadlock(painter, grooveOpacity: composition.grooveOpacity)
        painter.transformed(arrows.left) {
            drawArrow(painter, composition: composition, upsideDown: true)
        }
    }
    guard let image = canvas.makeImage() else {
        fatalError("Could not finish the drawing")
    }
    return image
}

// MARK: - Tile

/// Apple's macOS icon grid: an 824-pixel shape centered on the canvas.
let tileRect = CGRect(x: 100, y: 100, width: 824, height: 824)
let tileCornerRadius: CGFloat = 185.4

/// A rounded rectangle whose corners ease into their curve, as Apple's icon
/// shapes do, instead of the sudden change of a circular arc. Each corner
/// spans 1.528 radii of its edges; the control points are the usual cubic
/// fit for that curve.
func continuousRoundedRect(_ rect: CGRect, radius: CGFloat) -> CGPath {
    let span: CGFloat = 1.52866483
    let r = min(radius, min(rect.width, rect.height) / (2 * span))
    let path = CGMutablePath()

    // Arrives at `corner` heading along `incoming` and leaves along `outgoing`.
    func corner(_ corner: CGPoint, _ incoming: CGVector, _ outgoing: CGVector) {
        func point(_ back: CGFloat, _ forward: CGFloat) -> CGPoint {
            CGPoint(
                x: corner.x - back * r * incoming.dx + forward * r * outgoing.dx,
                y: corner.y - back * r * incoming.dy + forward * r * outgoing.dy
            )
        }
        path.addLine(to: point(span, 0))
        path.addCurve(
            to: point(0.63149399, 0.07491100),
            control1: point(1.08849299, 0),
            control2: point(0.86840701, 0)
        )
        path.addCurve(
            to: point(0.07491100, 0.63149399),
            control1: point(0.37282401, 0.16905001),
            control2: point(0.16905001, 0.37282401)
        )
        path.addCurve(
            to: point(0, span),
            control1: point(0, 0.86840701),
            control2: point(0, 1.08849299)
        )
    }

    path.move(to: CGPoint(x: rect.minX + span * r, y: rect.minY))
    corner(CGPoint(x: rect.maxX, y: rect.minY), CGVector(dx: 1, dy: 0), CGVector(dx: 0, dy: 1))
    corner(CGPoint(x: rect.maxX, y: rect.maxY), CGVector(dx: 0, dy: 1), CGVector(dx: -1, dy: 0))
    corner(CGPoint(x: rect.minX, y: rect.maxY), CGVector(dx: -1, dy: 0), CGVector(dx: 0, dy: -1))
    corner(CGPoint(x: rect.minX, y: rect.minY), CGVector(dx: 0, dy: -1), CGVector(dx: 1, dy: 0))
    path.closeSubpath()
    return path
}

/// The tile in `top` and `bottom`, lit and shaded as ruston-mail's icon is,
/// with `drawing` on it under one soft shadow.
func drawTile(top: UInt32, bottom: UInt32, drawing: CGImage) -> CGImage {
    let canvas = makeCanvas()
    let shape = continuousRoundedRect(tileRect, radius: tileCornerRadius)

    canvas.saveGState()
    canvas.setShadow(offset: CGSize(width: 0, height: -24), blur: 46, color: color(0x000000, alpha: 0.34))
    canvas.addPath(shape)
    canvas.setFillColor(color(0x000000))
    canvas.fillPath()
    canvas.restoreGState()

    canvas.saveGState()
    canvas.addPath(shape)
    canvas.clip()
    canvas.drawLinearGradient(
        gradient([(0, bottom, 1), (1, top, 1)]),
        start: CGPoint(x: tileRect.midX, y: tileRect.minY),
        end: CGPoint(x: tileRect.midX, y: tileRect.maxY),
        options: []
    )
    // A faint light over the top of the shape.
    let topLightHeight: CGFloat = 309
    canvas.drawLinearGradient(
        gradient([(0, 0xFFFFFF, 0), (1, 0xFFFFFF, 0.045)]),
        start: CGPoint(x: tileRect.midX, y: tileRect.maxY - topLightHeight),
        end: CGPoint(x: tileRect.midX, y: tileRect.maxY),
        options: []
    )
    // A light line just inside the shape's edge; half of the stroke lies
    // outside the shape, where the clip drops it.
    canvas.addPath(shape)
    canvas.setLineWidth(2 * 3)
    canvas.setStrokeColor(color(0xFFFFFF, alpha: 0.105))
    canvas.strokePath()

    canvas.setShadow(offset: CGSize(width: 0, height: -14), blur: 26, color: color(0x000000, alpha: 0.27))
    canvas.draw(drawing, in: CGRect(x: 0, y: 0, width: canvasSize, height: canvasSize))
    canvas.restoreGState()

    guard let image = canvas.makeImage() else {
        fatalError("Could not finish the tile")
    }
    return image
}

// MARK: - PNG Export

/// Writes `image` as a PNG, which carries the sRGB profile it was drawn in.
func writePNG(_ image: CGImage, to url: URL) {
    do {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
    } catch {
        fputs("Error creating \(url.deletingLastPathComponent().path): \(error)\n", stderr)
        exit(EXIT_FAILURE)
    }
    guard
        let destination = CGImageDestinationCreateWithURL(
            url as CFURL,
            UTType.png.identifier as CFString,
            1,
            nil
        )
    else {
        fputs("Error writing PNG: cannot create \(url.path)\n", stderr)
        exit(EXIT_FAILURE)
    }
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else {
        fputs("Error writing PNG: \(url.path)\n", stderr)
        exit(EXIT_FAILURE)
    }
}

for style in styles {
    let drawing = drawDrawing(style.composition)
    let image = style.tile.map { drawTile(top: $0.top, bottom: $0.bottom, drawing: drawing) } ?? drawing
    let url = outputDirectory.appendingPathComponent("\(style.name)/openscp-1024.png")
    writePNG(image, to: url)
    print("OpenSCP icon (\(style.name)) written to \(url.path)")
}
