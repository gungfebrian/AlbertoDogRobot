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
         surprised, sad, suspicious, neutral, excited

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
    // grumpy looks away, sad looks down
    private var moodShift: CGSize {
        switch mood {
        case .grumpy: return CGSize(width: -12, height: 4)
        case .sad:    return CGSize(width: 0,   height: 12)
        default:      return .zero
        }
    }
    // furious eyes seethe side-to-side
    private var shakeX: CGFloat {
        mood == .furious ? (pulse * 6 - 3) : 0
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
        .buttonStyle(.plain)
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
        .buttonStyle(.plain)
    }
}

// =====================================================================
// MARK: - Grid of command chips
// =====================================================================
struct CmdGrid: View {
    let commands: [(String, String)]   // (label, icon)
    let send: (String) -> Void
    var columns: Int = 3
    var body: some View {
        let cols = Array(repeating: GridItem(.flexible(), spacing: 8), count: columns)
        LazyVGrid(columns: cols, spacing: 8) {
            ForEach(commands, id: \.0) { cmd in
                CmdButton(title: cmd.0, icon: cmd.1.isEmpty ? nil : cmd.1) {
                    send(cmd.0)
                }
            }
        }
    }
}

// =====================================================================
// MARK: - ContentView (the main screen)
// =====================================================================
struct ContentView: View {
    @StateObject private var ble = BLEManager()
    @State private var statusPoll: Timer?
    @State private var showAllControls = false

    // pressing a Feelings button shows that mood on the face for a while,
    // so the app face reacts instantly instead of waiting for telemetry
    @State private var moodOverride: AlbertMood?
    @State private var overrideToken = UUID()

    // big text captions mirrored on the face card during text shows
    @State private var caption: String?
    @State private var captionTimer: Timer?

    @State private var showResetConfirm = false

    private var mood: AlbertMood { moodOverride ?? AlbertMood.from(ble.stats) }

    /// Send a feeling command and mirror it on the app face immediately.
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

            ScrollView {
                VStack(spacing: 20) {
                    header
                    facePanel
                    if ble.isConnected {
                        moodPanel
                        showsPanel
                        movementPanel
                        DisclosureGroup(isExpanded: $showAllControls) {
                            VStack(spacing: 18) {
                                posesPanel
                                tricksPanel
                                dancesPanel
                                expressionsPanel
                                systemPanel
                            }
                            .padding(.top, 12)
                        } label: {
                            Text(showAllControls ? "Hide extra controls" : "More poses, tricks, dances & faces")
                                .font(.system(size: 14, weight: .bold, design: .rounded))
                                .foregroundColor(mood.accent)
                        }
                        .padding(.horizontal, 4)
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
        .alert("Reset Albert's memory?", isPresented: $showResetConfirm) {
            Button("Reset", role: .destructive) { ble.send("RESET") }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This wipes his happiness, bonding and lifetime pets back to zero. He won't remember you!")
        }
        .preferredColorScheme(.dark)
    }

    // ---- header ----
    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Albert")
                    .font(.system(size: 30, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                Text(ble.isConnected ? mood.label : ble.statusText)
                    .font(.system(size: 13, weight: .medium, design: .rounded))
                    .foregroundColor(mood.accent)
            }
            Spacer()
            Circle()
                .fill(ble.isConnected ? Color.green : Color.gray)
                .frame(width: 12, height: 12)
                .shadow(color: ble.isConnected ? .green : .clear, radius: 6)
        }
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
            } else {
                Text("Reading Albert's mood…")
                    .font(.system(size: 13, design: .rounded))
                    .foregroundColor(AlbertPalette.subtle)
            }
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(AlbertPalette.card))
    }

    // ---- SHOWS (the star feature) ----
    private var showsPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Shows")
            HStack(spacing: 8) {
                CmdButton(title: "INTRO", icon: "hand.wave.fill",
                          tint: mood.accent.opacity(0.85), big: true) {
                    ble.send("INTRO")
                    playCaption(["HI", "AKU", "ALBERT"], step: 0.7, repeats: 2)
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

    private var posesPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Poses")
            CmdGrid(commands: [
                ("UP","arrow.up.to.line"),
                ("SIT","chair"),
                ("LIE","bed.double"),
                ("LAYDOWN","rectangle.compress.vertical"),
                ("REST","powersleep"),
                ("SLEEP","moon.fill"),
            ], send: { ble.send($0) })
        }
    }

    private var tricksPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Tricks")
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
            ], send: { ble.send($0) })
        }
    }

    private var dancesPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Dances")
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
            ], send: { ble.send($0) })
        }
    }

    // feelings carry a mood so the app face mirrors the button instantly
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
        ("NERVOUS",   "waveform.path.ecg",     nil),
        ("LAUGH",     "face.smiling.inverse",  .happy),
        ("CONFUSED",  "questionmark",          nil),
        ("WINK",      "eye.fill",              nil),
    ]

    private var expressionsPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "Feelings")
            let cols = Array(repeating: GridItem(.flexible(), spacing: 8), count: 3)
            LazyVGrid(columns: cols, spacing: 8) {
                ForEach(feelings, id: \.0) { f in
                    CmdButton(title: f.cmd, icon: f.icon) {
                        sendFeeling(f.cmd, as: f.mood)
                    }
                }
            }
        }
    }

    private var systemPanel: some View {
        VStack(spacing: 10) {
            SectionLabel(text: "System")
            HStack(spacing: 8) {
                CmdButton(title: "AUTO", icon: "wand.and.stars") { ble.send("AUTO") }
                CmdButton(title: "SLEEP", icon: "moon.fill") { ble.send("SLEEP") }
                CmdButton(title: "UP", icon: "arrow.up.to.line") { ble.send("UP") }
            }
            HStack(spacing: 8) {
                CmdButton(title: "STATS", icon: "chart.bar.fill") { ble.send("STATS") }
                CmdButton(title: "RESET", icon: "arrow.counterclockwise.circle",
                          tint: Color.orange.opacity(0.4)) { showResetConfirm = true }
                CmdButton(title: "Disconnect", icon: "xmark.circle",
                          tint: Color.red.opacity(0.3)) { ble.disconnect() }
            }
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

#Preview {
    ContentView()
}
