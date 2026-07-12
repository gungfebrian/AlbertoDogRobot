//
//  AlbertUI.swift
//  INTERFACEALBERT
//
//  Drop this file into the SAME project as INTERFACEALBERTApp.swift.
//  It provides ContentView (the @main App already points to it) plus
//  the animated polygon RoboEyes and the full control interface.
//
//  Keep your existing INTERFACEALBERTApp.swift (BLEManager + PetStats).
//  Do NOT redefine ContentView anywhere else.
//

import SwiftUI
import CoreBluetooth

// =====================================================================
// MARK: - Mood model (derived from live PetStats telemetry)
// =====================================================================

/// The emotional state Albert's face should reflect. Mirrors the
/// firmware's mood-face logic so the on-screen eyes match the robot.
enum AlbertMood: String {
    case sleepy, happy, love, tired, angry, grumpy, furious,
         surprised, sad, suspicious, neutral, excited,
         bored, dizzy, proud

    /// Same priority ladder the firmware uses in moodFace().
    static func from(_ s: PetStats?) -> AlbertMood {
        guard let s = s else { return .neutral }
        if s.energy < 25          { return .sleepy }
        if s.happiness > 88       { return .love }
        if s.happiness > 72       { return .happy }
        if s.happiness < 18       { return .grumpy }
        if s.happiness < 32       { return .sad }
        if s.boredom  > 70        { return .suspicious }
        return .neutral
    }

    var label: String {
        switch self {
        case .sleepy:     return "Sleepy"
        case .happy:      return "Happy"
        case .love:       return "In love"
        case .tired:      return "Tired"
        case .angry:      return "Angry"
        case .grumpy:     return "Grumpy"
        case .furious:    return "FURIOUS"
        case .surprised:  return "Surprised!"
        case .sad:        return "Sad"
        case .suspicious: return "Curious"
        case .neutral:    return "Content"
        case .excited:    return "Excited"
        case .bored:      return "Bored"
        case .dizzy:      return "Dizzy"
        case .proud:      return "Proud"
        }
    }

    var accent: Color {
        switch self {
        case .sleepy:     return Color(red: 0.55, green: 0.60, blue: 0.85)
        case .happy:      return Color(red: 0.35, green: 0.80, blue: 0.65)
        case .love:       return Color(red: 0.95, green: 0.45, blue: 0.65)
        case .tired:      return Color(red: 0.60, green: 0.62, blue: 0.72)
        case .angry:      return Color(red: 0.95, green: 0.45, blue: 0.40)
        case .grumpy:     return Color(red: 0.95, green: 0.55, blue: 0.30)
        case .furious:    return Color(red: 0.90, green: 0.25, blue: 0.20)
        case .surprised:  return Color(red: 0.98, green: 0.85, blue: 0.35)
        case .sad:        return Color(red: 0.50, green: 0.62, blue: 0.85)
        case .suspicious: return Color(red: 0.55, green: 0.75, blue: 0.95)
        case .neutral:    return Color(red: 0.45, green: 0.70, blue: 0.95)
        case .excited:    return Color(red: 0.98, green: 0.75, blue: 0.35)
        case .bored:      return Color(red: 0.55, green: 0.58, blue: 0.65)
        case .dizzy:      return Color(red: 0.72, green: 0.55, blue: 0.95)
        case .proud:      return Color(red: 0.95, green: 0.80, blue: 0.30)
        }
    }
}

// =====================================================================
// MARK: - Anger mark (the anime 💢-style grumpy symbol)
// =====================================================================

/// Four short ticks radiating from a centre — same glyph the firmware
/// draws in the top-left corner of the OLED when Albert is grumpy.
struct AngerMarkShape: Shape {
    func path(in rect: CGRect) -> Path {
        var p = Path()
        let c  = CGPoint(x: rect.midX, y: rect.midY)
        let r1 = min(rect.width, rect.height) * 0.18
        let r2 = min(rect.width, rect.height) * 0.48
        for angle in stride(from: 45.0, to: 360.0, by: 90.0) {
            let a = angle * .pi / 180
            p.move(to:    CGPoint(x: c.x + cos(a) * r1, y: c.y + sin(a) * r1))
            p.addLine(to: CGPoint(x: c.x + cos(a) * r2, y: c.y + sin(a) * r2))
        }
        return p
    }
}

struct AngerBadge: View {
    var size: CGFloat = 26
    var color: Color = AlbertMood.furious.accent
    @State private var throb = false
    var body: some View {
        AngerMarkShape()
            .stroke(color, style: StrokeStyle(lineWidth: 3.5, lineCap: .round))
            .frame(width: size, height: size)
            .scaleEffect(throb ? 1.15 : 0.9)
            .onAppear {
                withAnimation(.easeInOut(duration: 0.5).repeatForever(autoreverses: true)) {
                    throb = true
                }
            }
    }
}

// =====================================================================
// MARK: - Polygon RoboEyes (FluxGarage-style, animated in SwiftUI)
// =====================================================================

/// A single rounded-rectangle "polygon" eye, matching the FluxGarage
/// RoboEyes look: soft-cornered rectangles that squash, shift and blink.
private struct PolyEye: View {
    var width: CGFloat
    var height: CGFloat
    var corner: CGFloat
    var color: Color
    var offset: CGSize
    /// A crescent slice used for the "happy" bottom-lid look.
    var happyLid: CGFloat   // 0 = open, 1 = big smile lid at bottom

    var body: some View {
        RoundedRectangle(cornerRadius: corner, style: .continuous)
            .fill(color)
            .frame(width: width, height: height)
            .overlay(
                // bottom lid crescent for smiles (a rounded rect that
                // eats the lower part of the eye, coloured as background)
                GeometryReader { geo in
                    RoundedRectangle(cornerRadius: corner, style: .continuous)
                        .fill(Color.black.opacity(0.001)) // keeps layout
                        .overlay(alignment: .bottom) {
                            RoundedRectangle(cornerRadius: corner, style: .continuous)
                                .fill(AlbertPalette.faceBg)
                                .frame(height: geo.size.height * happyLid)
                                .offset(y: 2)
                        }
                        .mask(
                            RoundedRectangle(cornerRadius: corner, style: .continuous)
                        )
                }
            )
            .offset(offset)
    }
}

/// The animated face. Reads a mood + optional gaze and continuously
/// blinks, breathes and drifts so it always looks alive.
struct RoboEyesView: View {
    var mood: AlbertMood
    var isConnected: Bool

    // animation state
    @State private var blink: CGFloat = 1      // 1 open, 0 shut
    @State private var breathe: CGFloat = 0    // subtle vertical bob
    @State private var gaze: CGSize = .zero    // idle look-around
    @State private var pulse: CGFloat = 0      // love/excited pulse
    @State private var timer: Timer?

    // per-mood geometry
    private var eyeW: CGFloat {
        switch mood {
        case .suspicious: return 62
        case .love, .happy: return 66
        case .surprised: return 72
        case .furious: return 58
        default: return 64
        }
    }
    private var eyeH: CGFloat {
        switch mood {
        case .sleepy: return 20
        case .tired:  return 34
        case .suspicious: return 30
        case .grumpy: return 26
        case .furious: return 36
        case .sad: return 30
        case .surprised: return 72
        case .bored: return 16
        case .dizzy: return 34
        case .proud: return 70
        case .excited: return 68 + pulse * 4
        case .love:   return 66 + pulse * 6
        default:      return 64
        }
    }
    private var corner: CGFloat {              // the polygon softness
        mood == .surprised ? 34 : 16           // surprised = round shock eyes
    }
    private var gap: CGFloat {
        mood == .love ? 14 : 26
    }
    private var happyLid: CGFloat {
        switch mood {
        case .happy: return 0.34
        case .love:  return 0.28
        default:     return 0.0
        }
    }
    // angry family: eyes tilt inward, more when more furious
    private var tilt: Double {
        switch mood {
        case .angry:   return 16
        case .grumpy:  return 12
        case .furious: return 22
        default:       return 0
        }
    }
    // grumpy looks away, sad and bored look down, proud looks up
    private var moodShift: CGSize {
        switch mood {
        case .grumpy: return CGSize(width: -12, height: 4)
        case .sad:    return CGSize(width: 0,   height: 12)
        case .bored:  return CGSize(width: 0,   height: 10)
        case .proud:  return CGSize(width: 0,   height: -8)
        default:      return .zero
        }
    }
    // furious eyes seethe, dizzy eyes wobble
    private var shakeX: CGFloat {
        switch mood {
        case .furious: return pulse * 6 - 3
        case .dizzy:   return pulse * 10 - 5
        default:       return 0
        }
    }

    var body: some View {
        ZStack {
            // emote glyphs above the eyes (hearts / zzz), in a top band
            emoteBand
                .offset(y: -70)

            HStack(spacing: gap) {
                PolyEye(width: eyeW, height: eyeH * blink, corner: corner,
                        color: AlbertPalette.eye,
                        offset: CGSize(width: gaze.width + moodShift.width + shakeX,
                                       height: gaze.height + breathe + moodShift.height),
                        happyLid: happyLid)
                    .rotationEffect(.degrees(tilt))

                PolyEye(width: eyeW, height: eyeH * blink, corner: corner,
                        color: AlbertPalette.eye,
                        offset: CGSize(width: gaze.width + moodShift.width + shakeX,
                                       height: gaze.height + breathe + moodShift.height),
                        happyLid: happyLid)
                    .rotationEffect(.degrees(-tilt))
            }
            .animation(.easeInOut(duration: 0.28), value: mood)
            .animation(.easeInOut(duration: 0.12), value: blink)
            .opacity(isConnected ? 1 : 0.35)
        }
        .onAppear { startLoop() }
        .onDisappear { timer?.invalidate() }
    }

    // hearts when in love, Zzz when sleepy, sparkle when excited
    @ViewBuilder private var emoteBand: some View {
        switch mood {
        case .love:
            HStack(spacing: 10) {
                ForEach(0..<3, id: \.self) { i in
                    Image(systemName: "heart.fill")
                        .foregroundColor(AlbertMood.love.accent)
                        .font(.system(size: i == 1 ? 20 : 14))
                        .offset(y: pulse * -6)
                        .opacity(0.9)
                }
            }
        case .sleepy:
            HStack(spacing: 4) {
                ForEach(0..<3, id: \.self) { i in
                    Text("z")
                        .font(.system(size: 14 + CGFloat(i) * 6, weight: .black, design: .rounded))
                        .foregroundColor(AlbertMood.sleepy.accent)
                        .offset(y: -CGFloat(i) * 8)
                        .opacity(0.8)
                }
            }
        case .excited, .happy:
            Image(systemName: "sparkles")
                .foregroundColor(mood.accent)
                .font(.system(size: 18))
                .opacity(0.8)
        case .surprised:
            Text("!")
                .font(.system(size: 32, weight: .black, design: .rounded))
                .foregroundColor(mood.accent)
                .offset(y: pulse * -4)
        case .sad:
            Image(systemName: "drop.fill")
                .foregroundColor(AlbertMood.sad.accent)
                .font(.system(size: 16))
                .offset(x: -52, y: pulse * 8)
                .opacity(0.85)
        case .dizzy:
            Text("?")
                .font(.system(size: 30, weight: .black, design: .rounded))
                .foregroundColor(mood.accent)
                .rotationEffect(.degrees(pulse * 20 - 10))
        case .proud:
            Image(systemName: "crown.fill")
                .foregroundColor(mood.accent)
                .font(.system(size: 18))
                .opacity(0.9)
        default:
            EmptyView()
        }
    }

    private func startLoop() {
        timer?.invalidate()
        // gentle breathing
        withAnimation(.easeInOut(duration: 2.2).repeatForever(autoreverses: true)) {
            breathe = 4
        }
        // love/excited pulse
        withAnimation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true)) {
            pulse = 1
        }
        // random blink + idle gaze on a repeating timer
        timer = Timer.scheduledTimer(withTimeInterval: 0.9, repeats: true) { _ in
            // don't blink while sleepy (eyes already near-shut)
            if mood != .sleepy, Int.random(in: 0..<3) == 0 {
                withAnimation(.easeIn(duration: 0.08)) { blink = 0.1 }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                    withAnimation(.easeOut(duration: 0.12)) { blink = 1 }
                }
            }
            // occasional idle glance (not while in love — he stares at you)
            if mood != .love, Int.random(in: 0..<4) == 0 {
                let dx = CGFloat(Int.random(in: -14...14))
                let dy = CGFloat(Int.random(in: -8...8))
                withAnimation(.easeInOut(duration: 0.5)) { gaze = CGSize(width: dx, height: dy) }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.9) {
                    withAnimation(.easeInOut(duration: 0.5)) { gaze = .zero }
                }
            }
        }
    }
}

// =====================================================================
// MARK: - Palette
// =====================================================================
enum AlbertPalette {
    static let bg      = Color(red: 0.06, green: 0.07, blue: 0.12)
    static let bg2     = Color(red: 0.10, green: 0.12, blue: 0.20)
    static let faceBg  = Color(red: 0.14, green: 0.17, blue: 0.30)
    static let eye     = Color(red: 0.90, green: 0.94, blue: 1.0)
    static let card    = Color.white.opacity(0.06)
    static let cardHi  = Color.white.opacity(0.12)
    static let text    = Color.white
    static let subtle  = Color.white.opacity(0.55)
}

// =====================================================================
// MARK: - Reusable command button
// =====================================================================

/// Buttons squash slightly while pressed — makes the whole app feel
/// tactile, like real controller buttons.
struct SquishStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.92 : 1)
            .opacity(configuration.isPressed ? 0.8 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

struct CmdButton: View {
    let title: String
    var icon: String? = nil
    var tint: Color = AlbertPalette.cardHi
    var big: Bool = false
    let action: () -> Void

    @State private var pressed = false

    var body: some View {
        Button {
            let gen = UIImpactFeedbackGenerator(style: .medium)
            gen.impactOccurred()
            action()
        } label: {
            HStack(spacing: 6) {
                if let icon = icon {
                    Image(systemName: icon).font(.system(size: big ? 18 : 14, weight: .semibold))
                }
                Text(title)
                    .font(.system(size: big ? 16 : 13, weight: .semibold, design: .rounded))
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, big ? 16 : 12)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(tint)
            )
            .foregroundColor(AlbertPalette.text)
        }
        .buttonStyle(SquishStyle())
    }
}

// section header
struct SectionLabel: View {
    let text: String
    var body: some View {
        Text(text.uppercased())
            .font(.system(size: 12, weight: .heavy, design: .rounded))
            .foregroundColor(AlbertPalette.subtle)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 4)
    }
}

// a labelled progress bar for mood stats
struct StatBar: View {
    let label: String
    let value: Int
    let color: Color
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(label).font(.system(size: 12, weight: .semibold, design: .rounded))
                    .foregroundColor(AlbertPalette.subtle)
                Spacer()
                Text("\(value)").font(.system(size: 12, weight: .bold, design: .rounded))
                    .foregroundColor(AlbertPalette.text)
            }
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.white.opacity(0.08))
                    Capsule().fill(color)
                        .frame(width: geo.size.width * CGFloat(max(0, min(100, value))) / 100)
                        .animation(.easeInOut(duration: 0.4), value: value)
                }
            }
            .frame(height: 8)
        }
    }
}

// =====================================================================
// MARK: - Directional D-pad for movement
// =====================================================================
struct DPad: View {
    let send: (String) -> Void
    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                dirButton("SL", "arrow.counterclockwise")
                dirButton("WALK", "arrow.up")
                dirButton("SR", "arrow.clockwise")
            }
            HStack(spacing: 8) {
                dirButton("LEFT", "arrow.left")
                dirButton("STOP", "stop.fill", tint: Color(red: 0.9, green: 0.35, blue: 0.35).opacity(0.8))
                dirButton("RIGHT", "arrow.right")
            }
            HStack(spacing: 8) {
                Spacer().frame(maxWidth: .infinity)
                dirButton("BACK", "arrow.down")
                Spacer().frame(maxWidth: .infinity)
            }
        }
    }
    private func dirButton(_ cmd: String, _ icon: String, tint: Color = AlbertPalette.cardHi) -> some View {
        Button {
            UIImpactFeedbackGenerator(style: .rigid).impactOccurred()
            send(cmd)
        } label: {
            Image(systemName: icon)
                .font(.system(size: 20, weight: .bold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 16)
                .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(tint))
                .foregroundColor(.white)
        }
        .buttonStyle(SquishStyle())
    }
}

// =====================================================================
// MARK: - Grid of command chips
// =====================================================================
struct CmdGrid: View {
    let commands: [(String, String)]   // (label, icon)
    let send: (String) -> Void
    var columns: Int = 3
    var tint: Color = AlbertPalette.cardHi   // per-category colour coding
    var body: some View {
        let cols = Array(repeating: GridItem(.flexible(), spacing: 8), count: columns)
        LazyVGrid(columns: cols, spacing: 8) {
            ForEach(commands, id: \.0) { cmd in
                CmdButton(title: cmd.0, icon: cmd.1.isEmpty ? nil : cmd.1,
                          tint: tint) {
                    send(cmd.0)
                }
            }
        }
    }
}

// =====================================================================
// MARK: - ContentView (tab shell: Dashboard + Controller pages)
// =====================================================================
struct ContentView: View {
    @StateObject private var ble = BLEManager()

    var body: some View {
        TabView {
            DashboardView(ble: ble)
                .tabItem { Label("Albert", systemImage: "face.smiling") }
            ControllerView(ble: ble)
                .tabItem { Label("Control", systemImage: "gamecontroller.fill") }
            ActionsView(ble: ble)
                .tabItem { Label("Actions", systemImage: "square.grid.2x2.fill") }
        }
        .preferredColorScheme(.dark)
    }
}

// =====================================================================
// MARK: - DashboardView (the original main screen)
// =====================================================================
struct DashboardView: View {
    @ObservedObject var ble: BLEManager
    @State private var statusPoll: Timer?

    // big text captions mirrored on the face card during text shows
    @State private var caption: String?
    @State private var captionTimer: Timer?

    // Feelings buttons (and their instant mood-override trick) live in
    // the Actions tab now — the dashboard face just mirrors telemetry.
    private var mood: AlbertMood { AlbertMood.from(ble.stats) }

    /// Flash a word sequence over the face card (mirrors the OLED banners).
    private func playCaption(_ words: [String], step: Double = 0.55, repeats: Int = 1) {
        captionTimer?.invalidate()
        var seq: [String] = []
        for _ in 0..<max(1, repeats) { seq.append(contentsOf: words) }
        var i = 0
        caption = seq.first
        captionTimer = Timer.scheduledTimer(withTimeInterval: step, repeats: true) { t in
            i += 1
            if i >= seq.count {
                t.invalidate()
                withAnimation(.easeOut(duration: 0.3)) { caption = nil }
            } else {
                caption = seq[i]
            }
        }
    }

    var body: some View {
        ZStack {
            // animated gradient background tinted by mood
            LinearGradient(colors: [AlbertPalette.bg, AlbertPalette.bg2],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()
            RadialGradient(colors: [mood.accent.opacity(0.28), .clear],
                           center: .top, startRadius: 10, endRadius: 420)
                .ignoresSafeArea()
                .animation(.easeInOut(duration: 0.8), value: mood)

            ScrollView(showsIndicators: false) {
                VStack(spacing: 20) {
                    header
                    facePanel
                    if ble.isConnected {
                        moodPanel
                        autoPanel
                        showsPanel
                        movementPanel
                        Text("Tricks, dances & feelings live in the Actions tab")
                            .font(.system(size: 12, weight: .medium, design: .rounded))
                            .foregroundColor(AlbertPalette.subtle)
                            .padding(.bottom, 4)
                    } else {
                        connectPanel
                    }
                }
                .padding(20)
            }
        }
        .onChange(of: ble.isConnected) { connected in
            if connected { startStatusPolling() } else { statusPoll?.invalidate() }
        }
        .preferredColorScheme(.dark)
    }

    // pretty name for the firmware's activity strings
    private func activityLabel(_ raw: String) -> String {
        switch raw {
        case "chill":          return "Chilling"
        case "explore":        return "Exploring"
        case "play":           return "Playing"
        case "seek-attention": return "Wants attention"
        case "groom":          return "Grooming"
        case "zoomies!":       return "ZOOMIES!"
        case "show-off":       return "Showing off"
        default:               return raw
        }
    }

    // ---- header ----
    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Albert")
                    .font(.system(size: 30, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                // mood + live autopilot activity in one line
                Text(headerSubtitle)
                    .font(.system(size: 13, weight: .medium, design: .rounded))
                    .foregroundColor(mood.accent)
            }
            Spacer()
            // connection pill — clearer than a bare dot
            HStack(spacing: 6) {
                Circle()
                    .fill(ble.isConnected ? Color.green : Color.gray)
                    .frame(width: 8, height: 8)
                Text(ble.isConnected ? "Online" : "Offline")
                    .font(.system(size: 11, weight: .bold, design: .rounded))
                    .foregroundColor(ble.isConnected ? .green : AlbertPalette.subtle)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Capsule().fill(Color.white.opacity(0.07)))
        }
    }

    private var headerSubtitle: String {
        guard ble.isConnected else { return ble.statusText }
        if let act = ble.stats?.activity, !act.isEmpty {
            return "\(mood.label) · \(activityLabel(act))"
        }
        return mood.label
    }

    // ---- the animated face card ----
    private var facePanel: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 28, style: .continuous)
                .fill(AlbertPalette.faceBg)
                .frame(height: 240)
                .overlay(
                    RoundedRectangle(cornerRadius: 28, style: .continuous)
                        .stroke(mood.accent.opacity(0.4), lineWidth: 1.5)
                )
                .shadow(color: mood.accent.opacity(0.25), radius: 20, y: 8)

            RoboEyesView(mood: mood, isConnected: ble.isConnected)

            // big show caption (mirrors the OLED text banners)
            if let caption = caption {
                RoundedRectangle(cornerRadius: 28, style: .continuous)
                    .fill(AlbertPalette.faceBg.opacity(0.96))
                Text(caption)
                    .font(.system(size: 46, weight: .black, design: .rounded))
                    .foregroundColor(AlbertPalette.eye)
                    .minimumScaleFactor(0.4)
                    .lineLimit(1)
                    .padding(.horizontal, 20)
                    .transition(.scale.combined(with: .opacity))
                    .id(caption)   // re-animates on every word change
            }

            // tap the face to pet him
            if ble.isConnected && caption == nil {
                VStack {
                    Spacer()
                    Text("tap to pet")
                        .font(.system(size: 11, weight: .semibold, design: .rounded))
                        .foregroundColor(AlbertPalette.subtle)
                        .padding(.bottom, 12)
                }
            }
        }
        // the grumpy symbol, TOP-LEFT — just like on the robot's OLED
        .overlay(alignment: .topLeading) {
            if mood == .grumpy || mood == .furious {
                AngerBadge(size: mood == .furious ? 30 : 26,
                           color: mood.accent)
                    .padding(16)
                    .transition(.scale.combined(with: .opacity))
            }
        }
        // furious throbs in BOTH corners
        .overlay(alignment: .topTrailing) {
            if mood == .furious {
                AngerBadge(size: 30, color: mood.accent)
                    .padding(16)
                    .transition(.scale.combined(with: .opacity))
            }
        }
        .contentShape(RoundedRectangle(cornerRadius: 28))
        .onTapGesture {
            guard ble.isConnected else { return }
            UINotificationFeedbackGenerator().notificationOccurred(.success)
            ble.send("PET")
        }
    }

    // ---- connect prompt ----
    private var connectPanel: some View {
        VStack(spacing: 14) {
            Text(ble.isScanning ? "Looking for Albert…" : "Albert is offline")
                .font(.system(size: 16, weight: .semibold, design: .rounded))
                .foregroundColor(AlbertPalette.subtle)
            CmdButton(title: ble.isScanning ? "Scanning…" : "Scan & Connect",
                      icon: "antenna.radiowaves.left.and.right",
                      tint: mood.accent.opacity(0.85), big: true) {
                ble.startScan()
            }
        }
        .padding(.top, 20)
    }

    // ---- mood stats ----
    private var moodPanel: some View {
        VStack(spacing: 12) {
            if let s = ble.stats {
                StatBar(label: "Happiness", value: s.happiness, color: AlbertMood.happy.accent)
                StatBar(label: "Energy",    value: s.energy,    color: AlbertMood.excited.accent)
                StatBar(label: "Bonding",   value: s.bonding,   color: AlbertMood.love.accent)
                HStack {
                    Label("\(s.pets) pets", systemImage: "hand.raised.fill")
                    Spacer()
                    Label("age \(s.age)", systemImage: "sparkles")
                }
                .font(.system(size: 12, weight: .semibold, design: .rounded))
                .foregroundColor(AlbertPalette.subtle)
                // the last thing Albert said over BLE ("<3", "grr", "yawn"...)
                if !ble.lastMessage.isEmpty && !ble.lastMessage.contains("H:") {
                    Label(ble.lastMessage, systemImage: "quote.bubble.fill")
                        .font(.system(size: 12, weight: .semibold, design: .rounded))
                        .foregroundColor(mood.accent)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else {
                Text("Reading Albert's mood…")
                    .font(.system(size: 13, design: .rounded))
                    .foregroundColor(AlbertPalette.subtle)
            }
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(AlbertPalette.card))
    }

    // ---- Autopilot: free roam or lock him into one activity ----
    private var autoPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Autopilot")
            CmdButton(title: "FREE ROAM", icon: "wand.and.stars",
                      tint: mood.accent.opacity(0.75), big: true) {
                ble.send("AUTO")
            }
            let acts: [(label: String, cmd: String, match: String, icon: String)] = [
                ("Chill",    "AUTO CHILL",   "chill",          "leaf.fill"),
                ("Explore",  "AUTO EXPLORE", "explore",        "binoculars.fill"),
                ("Play",     "AUTO PLAY",    "play",           "figure.play"),
                ("Cuddle",   "AUTO SEEK",    "seek-attention", "heart.fill"),
                ("Groom",    "AUTO GROOM",   "groom",          "comb.fill"),
                ("Zoomies",  "AUTO ZOOM",    "zoomies!",       "bolt.fill"),
                ("Show off", "AUTO SHOW",    "show-off",       "star.fill"),
            ]
            let cols = Array(repeating: GridItem(.flexible(), spacing: 8), count: 4)
            LazyVGrid(columns: cols, spacing: 8) {
                ForEach(acts, id: \.cmd) { a in
                    let active = ble.stats?.activity == a.match
                    Button {
                        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                        ble.send(a.cmd)
                    } label: {
                        VStack(spacing: 4) {
                            Image(systemName: a.icon)
                                .font(.system(size: 15, weight: .semibold))
                            Text(a.label)
                                .font(.system(size: 10, weight: .bold, design: .rounded))
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                        .background(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .fill(active ? mood.accent.opacity(0.55)
                                             : AlbertPalette.cardHi)
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .stroke(active ? mood.accent : .clear, lineWidth: 1.5)
                        )
                        .foregroundColor(.white)
                    }
                    .buttonStyle(SquishStyle())
                }
            }
            Text("Pick one to lock him into that activity — Free Roam lets him choose")
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundColor(AlbertPalette.subtle)
        }
    }

    // ---- SHOWS (the star feature) ----
    private var showsPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Shows")
            HStack(spacing: 8) {
                CmdButton(title: "INTRO", icon: "hand.wave.fill",
                          tint: mood.accent.opacity(0.85), big: true) {
                    ble.send("INTRO")
                    playCaption(["HI!", "I'M", "ALBERTO"], step: 0.7, repeats: 2)
                }
                CmdButton(title: "5G GACOR", icon: "bolt.fill",
                          tint: AlbertMood.excited.accent.opacity(0.85), big: true) {
                    ble.send("GACOR")
                    playCaption(["5G", "GACOR!"], step: 0.5, repeats: 5)
                }
            }
            HStack(spacing: 8) {
                CmdButton(title: "TEAM", icon: "person.3.fill",
                          tint: AlbertMood.love.accent.opacity(0.8), big: true) {
                    ble.send("TEAM")
                    playCaption(["GUNG", "MAYUN", "GABBY", "ABUI", "TIO"],
                                step: 0.6, repeats: 3)
                }
                CmdButton(title: "PARTY", icon: "party.popper.fill",
                          tint: AlbertMood.happy.accent.opacity(0.8), big: true) {
                    ble.send("PARTY")
                    playCaption(["PARTY!", "TIME!"], step: 0.5, repeats: 3)
                }
            }
        }
    }

    private var movementPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Move")
            DPad { ble.send($0) }
        }
    }

    // poll STATUS so the on-screen face keeps mirroring the robot
    private func startStatusPolling() {
        statusPoll?.invalidate()
        ble.requestStatus()
        statusPoll = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { _ in
            ble.requestStatus()
        }
    }
}

// =====================================================================
// MARK: - ActionsView (the full command library, organised by category)
// =====================================================================
struct ActionsView: View {
    @ObservedObject var ble: BLEManager
    @State private var category = 0
    @State private var moodOverride: AlbertMood?
    @State private var overrideToken = UUID()
    @State private var showResetConfirm = false

    private var mood: AlbertMood { moodOverride ?? AlbertMood.from(ble.stats) }

    private let categories = ["Poses", "Tricks", "Dances", "Feelings", "System"]

    // feelings carry a mood so the mini face mirrors the button instantly
    private let feelings: [(cmd: String, icon: String, mood: AlbertMood?)] = [
        ("HAPPY",     "face.smiling",          .happy),
        ("LOVE",      "heart.fill",            .love),
        ("CUTE",      "sparkles",              .happy),
        ("GRUMPY",    "cloud.bolt.fill",       .grumpy),
        ("FURIOUS",   "flame.circle.fill",     .furious),
        ("ANGRY",     "flame.fill",            .angry),
        ("POUT",      "cloud.drizzle.fill",    .sad),
        ("SAD",       "cloud.rain",            .sad),
        ("SURPRISED", "exclamationmark",       .surprised),
        ("SLEEPY",    "moon.zzz.fill",         .sleepy),
        ("SUS",       "eyes",                  .suspicious),
        ("EXCITED",   "bolt.heart.fill",       .excited),
        ("BORED",     "zzz",                   .bored),
        ("DIZZY",     "arrow.triangle.2.circlepath.circle", .dizzy),
        ("PROUD",     "crown.fill",            .proud),
        ("NERVOUS",   "waveform.path.ecg",     nil),
        ("LAUGH",     "face.smiling.inverse",  .happy),
        ("CONFUSED",  "questionmark",          nil),
        ("WINK",      "eye.fill",              nil),
    ]

    var body: some View {
        ZStack {
            LinearGradient(colors: [AlbertPalette.bg, AlbertPalette.bg2],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()

            VStack(spacing: 14) {
                // compact live face so feelings give instant feedback here too
                ZStack {
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .fill(AlbertPalette.faceBg)
                        .frame(height: 110)
                        .overlay(
                            RoundedRectangle(cornerRadius: 20, style: .continuous)
                                .stroke(mood.accent.opacity(0.4), lineWidth: 1.5)
                        )
                    RoboEyesView(mood: mood, isConnected: ble.isConnected)
                        .scaleEffect(0.45)
                        .frame(height: 110)
                        .clipped()
                }
                .overlay(alignment: .topLeading) {
                    if mood == .grumpy || mood == .furious {
                        AngerBadge(size: 18, color: mood.accent).padding(10)
                    }
                }

                Picker("Category", selection: $category) {
                    ForEach(0..<categories.count, id: \.self) { i in
                        Text(categories[i]).tag(i)
                    }
                }
                .pickerStyle(.segmented)

                ScrollView(showsIndicators: false) {
                    VStack(spacing: 14) {
                        switch category {
                        case 0: posesGrid
                        case 1: tricksGrid
                        case 2: dancesGrid
                        case 3: feelingsGrid
                        default: systemGrid
                        }
                    }
                    .padding(.top, 4)
                }
            }
            .padding(20)
        }
        .alert("Reset Albert's memory?", isPresented: $showResetConfirm) {
            Button("Reset", role: .destructive) { ble.send("RESET") }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This wipes his happiness, bonding and lifetime pets back to zero. He won't remember you!")
        }
    }

    private func sendFeeling(_ cmd: String, as m: AlbertMood?) {
        ble.send(cmd)
        guard let m = m else { return }
        let token = UUID()
        overrideToken = token
        withAnimation(.easeInOut(duration: 0.3)) { moodOverride = m }
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) {
            if overrideToken == token {
                withAnimation(.easeInOut(duration: 0.5)) { moodOverride = nil }
            }
        }
    }

    private var posesGrid: some View {
        CmdGrid(commands: [
            ("UP","arrow.up.to.line"),
            ("SIT","chair"),
            ("LIE","bed.double"),
            ("LAYDOWN","rectangle.compress.vertical"),
            ("REST","powersleep"),
            ("SLEEP","moon.fill"),
        ], send: { ble.send($0) },
           tint: AlbertMood.neutral.accent.opacity(0.28))
    }

    private var tricksGrid: some View {
        CmdGrid(commands: [
            ("PUSHUPS","figure.strengthtraining.traditional"),
            ("STRETCH","figure.flexibility"),
            ("WAVE","hand.wave"),
            ("BOW","figure.bowling"),
            ("BEG","pawprint"),
            ("SPINPOSE","arrow.triangle.2.circlepath"),
            ("SHAKE","waveform"),
            ("GALLOP","hare.fill"),
            ("BOUNCE","arrow.up.arrow.down"),
            ("PLAYDEAD","theatermasks"),
            ("SNEEZE","wind"),
            ("KISS","heart.circle.fill"),
            ("ZOOMIES","bolt.fill"),
            ("SHY","eye.slash"),
            ("SULK","cloud.fill"),
            ("TANTRUM","exclamationmark.2"),
            ("YES","checkmark"),
            ("NO","xmark"),
        ], send: { ble.send($0) },
           tint: AlbertMood.happy.accent.opacity(0.24))
    }

    private var dancesGrid: some View {
        CmdGrid(commands: [
            ("DANCE","music.note"),
            ("MOONWALK","figure.walk"),
            ("CHACHA","music.quarternote.3"),
            ("HEADBANG","guitars"),
            ("BREAKDANCE","figure.dance"),
            ("ROBOT","cpu"),
            ("DISCO","rays"),
            ("BALLET","moon.stars"),
            ("STOMP","figure.stairs"),
            ("SALUTE","hand.raised"),
            ("PEEKABOO","eye"),
            ("POUNCE","hare"),
            ("TWIST","tornado"),
            ("WIGGLE","scribble.variable"),
            ("SWING","metronome"),
        ], send: { ble.send($0) },
           tint: AlbertMood.dizzy.accent.opacity(0.24))
    }

    private var feelingsGrid: some View {
        let cols = Array(repeating: GridItem(.flexible(), spacing: 8), count: 3)
        return LazyVGrid(columns: cols, spacing: 8) {
            ForEach(feelings, id: \.0) { f in
                CmdButton(title: f.cmd, icon: f.icon,
                          tint: AlbertMood.love.accent.opacity(0.22)) {
                    sendFeeling(f.cmd, as: f.mood)
                }
            }
        }
    }

    private var systemGrid: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                CmdButton(title: "AUTO", icon: "wand.and.stars") { ble.send("AUTO") }
                CmdButton(title: "SLEEP", icon: "moon.fill") { ble.send("SLEEP") }
                CmdButton(title: "STATS", icon: "chart.bar.fill") { ble.send("STATS") }
            }
            HStack(spacing: 8) {
                CmdButton(title: "SAVE", icon: "internaldrive") { ble.send("SAVE") }
                CmdButton(title: "RESET", icon: "arrow.counterclockwise.circle",
                          tint: Color.orange.opacity(0.4)) { showResetConfirm = true }
                CmdButton(title: "Disconnect", icon: "xmark.circle",
                          tint: Color.red.opacity(0.3)) { ble.disconnect() }
            }
        }
    }
}

// =====================================================================
// MARK: - ControllerView (PlayStation-style manual control page)
// =====================================================================

/// Analog-style stick. Drag it around: the direction zone maps to a
/// drive command; releasing snaps back to center and sends STOP.
struct JoystickPad: View {
    /// Called with "WALK" / "BACK" / "LEFT" / "RIGHT" while dragging,
    /// and nil when the stick is released.
    let onZone: (String?) -> Void

    @State private var drag: CGSize = .zero
    @State private var zone: String? = nil

    private let baseSize: CGFloat = 180
    private let thumbSize: CGFloat = 72
    private let maxRadius: CGFloat = 58
    private let deadZone: CGFloat = 22

    var body: some View {
        ZStack {
            // base plate with direction arrows
            Circle()
                .fill(Color.white.opacity(0.06))
                .overlay(Circle().stroke(Color.white.opacity(0.14), lineWidth: 1.5))
            Group {
                arrow("arrowtriangle.up.fill").offset(y: -baseSize/2 + 16)
                arrow("arrowtriangle.down.fill").offset(y: baseSize/2 - 16)
                arrow("arrowtriangle.left.fill").offset(x: -baseSize/2 + 16)
                arrow("arrowtriangle.right.fill").offset(x: baseSize/2 - 16)
            }
            // thumb stick
            Circle()
                .fill(zone == nil ? AlbertPalette.cardHi : AlbertMood.neutral.accent.opacity(0.85))
                .frame(width: thumbSize, height: thumbSize)
                .overlay(Circle().stroke(Color.white.opacity(0.25), lineWidth: 2))
                .shadow(color: .black.opacity(0.4), radius: 6, y: 3)
                .offset(drag)
                .animation(.spring(response: 0.25, dampingFraction: 0.6), value: drag)
        }
        .frame(width: baseSize, height: baseSize)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { g in
                    // clamp the thumb inside the base circle
                    let dx = g.translation.width, dy = g.translation.height
                    let dist = max(1, sqrt(dx*dx + dy*dy))
                    let clamped = min(dist, maxRadius)
                    drag = CGSize(width: dx / dist * clamped, height: dy / dist * clamped)

                    // outside the dead zone: pick a direction by angle
                    let newZone: String?
                    if clamped < deadZone {
                        newZone = nil
                    } else {
                        let angle = atan2(dy, dx) * 180 / .pi   // 0° = right, y down
                        switch angle {
                        case -135 ..< -45: newZone = "WALK"
                        case  -45 ..<  45: newZone = "RIGHT"
                        case   45 ..< 135: newZone = "BACK"
                        default:           newZone = "LEFT"
                        }
                    }
                    if newZone != zone {
                        zone = newZone
                        UIImpactFeedbackGenerator(style: .rigid).impactOccurred()
                        onZone(newZone)
                    }
                }
                .onEnded { _ in
                    drag = .zero
                    if zone != nil {
                        zone = nil
                        onZone(nil)
                    }
                }
        )
    }

    private func arrow(_ name: String) -> some View {
        Image(systemName: name)
            .font(.system(size: 13, weight: .bold))
            .foregroundColor(AlbertPalette.subtle)
    }
}

/// Press-and-hold button: sends a command on press, STOP on release.
/// Used for the spin "shoulder buttons".
struct HoldButton: View {
    let title: String
    let icon: String
    let command: String
    let send: (String) -> Void

    @State private var held = false

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: icon).font(.system(size: 14, weight: .bold))
            Text(title).font(.system(size: 13, weight: .heavy, design: .rounded))
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 12)
        .background(
            Capsule().fill(held ? AlbertMood.excited.accent.opacity(0.7)
                                : AlbertPalette.cardHi)
        )
        .foregroundColor(.white)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !held {
                        held = true
                        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                        send(command)
                    }
                }
                .onEnded { _ in
                    held = false
                    send("STOP")
                }
        )
    }
}

/// The PlayStation-style action diamond: four round buttons
/// (top / right / bottom / left) firing the current emotion bank.
struct EmotionDiamond: View {
    let items: [(cmd: String, icon: String, color: Color)]   // exactly 4
    let send: (String) -> Void

    var body: some View {
        ZStack {
            diamondButton(items[0]).offset(y: -58)
            diamondButton(items[1]).offset(x: 58)
            diamondButton(items[2]).offset(y: 58)
            diamondButton(items[3]).offset(x: -58)
        }
        .frame(width: 180, height: 180)
    }

    private func diamondButton(_ item: (cmd: String, icon: String, color: Color)) -> some View {
        Button {
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            send(item.cmd)
        } label: {
            VStack(spacing: 3) {
                Image(systemName: item.icon)
                    .font(.system(size: 20, weight: .bold))
                Text(item.cmd)
                    .font(.system(size: 8, weight: .heavy, design: .rounded))
            }
            .frame(width: 62, height: 62)
            .background(Circle().fill(item.color.opacity(0.32)))
            .overlay(Circle().stroke(item.color.opacity(0.8), lineWidth: 2))
            .foregroundColor(.white)
        }
        .buttonStyle(SquishStyle())
    }
}

struct ControllerView: View {
    @ObservedObject var ble: BLEManager
    @State private var bank = 0
    @State private var driveCmd: String?

    private var mood: AlbertMood { AlbertMood.from(ble.stats) }

    // the emotion switch: three swappable banks for the action diamond,
    // each in PS order (top, right, bottom, left)
    private let banks: [[(cmd: String, icon: String, color: Color)]] = [
        [("HAPPY",     "sun.max.fill",       AlbertMood.happy.accent),
         ("LOVE",      "heart.fill",         AlbertMood.love.accent),
         ("CUTE",      "sparkles",           AlbertMood.excited.accent),
         ("LAUGH",     "face.smiling",       AlbertMood.neutral.accent)],
        [("GRUMPY",    "cloud.bolt.fill",    AlbertMood.grumpy.accent),
         ("ANGRY",     "flame.fill",         AlbertMood.angry.accent),
         ("FURIOUS",   "flame.circle.fill",  AlbertMood.furious.accent),
         ("POUT",      "cloud.drizzle.fill", AlbertMood.sad.accent)],
        [("SURPRISED", "exclamationmark",    AlbertMood.surprised.accent),
         ("SAD",       "cloud.rain",         AlbertMood.sad.accent),
         ("SLEEPY",    "moon.zzz.fill",      AlbertMood.sleepy.accent),
         ("SUS",       "eyes",               AlbertMood.suspicious.accent)],
        [("EXCITED",   "bolt.heart.fill",    AlbertMood.excited.accent),
         ("DIZZY",     "arrow.triangle.2.circlepath.circle", AlbertMood.dizzy.accent),
         ("BORED",     "zzz",                AlbertMood.bored.accent),
         ("PROUD",     "crown.fill",         AlbertMood.proud.accent)],
    ]
    private let bankNames = ["Sunny", "Grumpy", "Drama", "Extra"]

    var body: some View {
        ZStack {
            LinearGradient(colors: [AlbertPalette.bg, AlbertPalette.bg2],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()

            VStack(spacing: 18) {
                // header with connection state
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Controller")
                            .font(.system(size: 26, weight: .black, design: .rounded))
                            .foregroundColor(.white)
                        Text(ble.isConnected ? "Drive me!" : ble.statusText)
                            .font(.system(size: 12, weight: .medium, design: .rounded))
                            .foregroundColor(mood.accent)
                    }
                    Spacer()
                    Circle()
                        .fill(ble.isConnected ? Color.green : Color.gray)
                        .frame(width: 12, height: 12)
                        .shadow(color: ble.isConnected ? .green : .clear, radius: 6)
                }

                if !ble.isConnected {
                    Spacer()
                    Text("Connect on the Albert tab first")
                        .font(.system(size: 15, weight: .semibold, design: .rounded))
                        .foregroundColor(AlbertPalette.subtle)
                    CmdButton(title: ble.isScanning ? "Scanning…" : "Scan & Connect",
                              icon: "antenna.radiowaves.left.and.right",
                              tint: mood.accent.opacity(0.85), big: true) {
                        ble.startScan()
                    }
                    Spacer()
                } else {
                    ScrollView(showsIndicators: false) {
                        VStack(spacing: 16) {
                            // shoulder buttons: hold to spin
                            HStack(spacing: 12) {
                                HoldButton(title: "SPIN L", icon: "arrow.counterclockwise",
                                           command: "SL", send: { ble.send($0) })
                                HoldButton(title: "SPIN R", icon: "arrow.clockwise",
                                           command: "SR", send: { ble.send($0) })
                            }

                            // the main deck: stick drives, diamond emotes
                            HStack(alignment: .center) {
                                VStack(spacing: 8) {
                                    JoystickPad { zone in
                                        driveCmd = zone
                                        ble.send(zone ?? "STOP")
                                    }
                                    // live readout of what the stick is sending
                                    Text(driveCmd ?? "idle")
                                        .font(.system(size: 11, weight: .heavy, design: .rounded))
                                        .foregroundColor(driveCmd == nil
                                            ? AlbertPalette.subtle : mood.accent)
                                        .animation(.easeInOut(duration: 0.15), value: driveCmd)
                                }
                                Spacer(minLength: 12)
                                EmotionDiamond(items: banks[bank], send: { ble.send($0) })
                            }
                            .padding(.vertical, 6)

                            // emotion switch between the banks
                            Picker("Mood set", selection: $bank) {
                                ForEach(0..<bankNames.count, id: \.self) { i in
                                    Text(bankNames[i]).tag(i)
                                }
                            }
                            .pickerStyle(.segmented)

                            // utility row
                            HStack(spacing: 8) {
                                CmdButton(title: "STAND", icon: "arrow.up.to.line") { ble.send("UP") }
                                CmdButton(title: "SIT", icon: "chair") { ble.send("SIT") }
                                CmdButton(title: "STOP", icon: "stop.fill",
                                          tint: Color(red: 0.9, green: 0.35, blue: 0.35).opacity(0.8)) {
                                    ble.send("STOP")
                                }
                            }

                            // quick tricks — one tap, no page switching
                            SectionLabel(text: "Tricks")
                            CmdGrid(commands: [
                                ("WAVE","hand.wave"),
                                ("LAYDOWN","rectangle.compress.vertical"),
                                ("PLAYDEAD","theatermasks"),
                                ("SNEEZE","wind"),
                                ("SHAKE","waveform"),
                                ("ZOOMIES","bolt.fill"),
                                ("TANTRUM","exclamationmark.2"),
                                ("YES","checkmark"),
                                ("NO","xmark"),
                                ("BOUNCE","arrow.up.arrow.down"),
                                ("BEG","pawprint"),
                            ], send: { ble.send($0) })

                            // quick dances
                            SectionLabel(text: "Dances")
                            CmdGrid(commands: [
                                ("ROBOT","cpu"),
                                ("SALUTE","hand.raised"),
                                ("WIGGLE","scribble.variable"),
                                ("STOMP","figure.stairs"),
                                ("CHACHA","music.quarternote.3"),
                            ], send: { ble.send($0) })
                        }
                    }
                }
            }
            .padding(20)
        }
    }
}

#Preview {
    ContentView()
}
