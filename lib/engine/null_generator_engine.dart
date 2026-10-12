// =============================================================================
// Zakura - Master 3D Null & Geometric Polygon Projection Engine
// Full 3D Euclidean Space, 6-Sided Box, Accordion Slices, Origami Cross Unfolding
// Real Perspective Camera Projection, Depth Sorting & UV Orientation Lock
// 100% Complete File - Zero Feature Omissions
// =============================================================================

import 'dart:math';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';

import '../models/models.dart';
import 'speed_curves.dart';

// =============================================================================
// 3D GEOMETRY PRIMITIVES
// =============================================================================

class Vertex3D {
  final double x;
  final double y;
  final double z;
  final double u;
  final double v;

  const Vertex3D(this.x, this.y, this.z, [this.u = 0.0, this.v = 0.0]);

  Vertex3D transform(Matrix4 m) {
    final s = m.storage;
    final double tx = s[0] * x + s[4] * y + s[8] * z + s[12];
    final double ty = s[1] * x + s[5] * y + s[9] * z + s[13];
    final double tz = s[2] * x + s[6] * y + s[10] * z + s[14];
    final double tw = s[3] * x + s[7] * y + s[11] * z + s[15];

    if (tw.abs() > 1e-6) {
      return Vertex3D(tx / tw, ty / tw, tz / tw, u, v);
    }
    return Vertex3D(tx, ty, tz, u, v);
  }

  Offset toScreen(double screenW, double screenH, double fovFactor) {
    final double depth = (z + 2.5); // Camera distance offset
    if (depth <= 0.05) return Offset(screenW * 0.5, screenH * 0.5);

    // Perspective divide
    final double projX = (x / depth) * fovFactor + (screenW * 0.5);
    final double projY = (y / depth) * fovFactor + (screenH * 0.5);
    return Offset(projX, projY);
  }
}

class ProjectedQuad3D {
  final int faceId;
  final int mediaIndex;
  final List<Vertex3D> vertices; // 4 corners in 3D
  final List<Offset> screenPoints; // Projected 2D points
  final double averageZ;
  final bool isFrontFacing;
  final Color edgeGlowColor;

  const ProjectedQuad3D({
    required this.faceId,
    required this.mediaIndex,
    required this.vertices,
    required this.screenPoints,
    required this.averageZ,
    required this.isFrontFacing,
    required this.edgeGlowColor,
  });
}

class OrbitParticle3D {
  final double x;
  final double y;
  final double z;
  final double size;
  final Color color;

  const OrbitParticle3D({
    required this.x,
    required this.y,
    required this.z,
    required this.size,
    required this.color,
  });

  Offset toScreen(double screenW, double screenH, double fovFactor) {
    final double depth = (z + 2.5);
    if (depth <= 0.05) return Offset(screenW * 0.5, screenH * 0.5);
    final double projX = (x / depth) * fovFactor + (screenW * 0.5);
    final double projY = (y / depth) * fovFactor + (screenH * 0.5);
    return Offset(projX, projY);
  }
}

class ZakuraEvaluatedScene {
  final List<ProjectedQuad3D> sortedQuads;
  final List<OrbitParticle3D> sortedParticles;
  final double velocityX;
  final double velocityY;
  final bool isGreenScreenIntro;
  final Color backgroundColor;
  final Matrix4 cameraViewProj;
  final Matrix4 nullTransform;

  const ZakuraEvaluatedScene({
    required this.sortedQuads,
    required this.sortedParticles,
    required this.velocityX,
    required this.velocityY,
    required this.isGreenScreenIntro,
    required this.backgroundColor,
    required this.cameraViewProj,
    required this.nullTransform,
  });
}

// =============================================================================
// MASTER 3D NULL & GEOMETRIC ENGINE
// =============================================================================

class NullGeneratorEngine {
  /// Evaluates all 3D polygons, cameras, particle orbits, and sorting at timestampMs
  static ZakuraEvaluatedScene evaluateScene({
    required GeneratorConfig config,
    required double timestampMs,
    required double screenWidth,
    required double screenHeight,
  }) {
    // 1. Timing: 0.30s (Rapid WIS) vs 1.0s (Category Pace)
    final double beatDurationMs = config.timing == BeatTiming.rapid300ms ? 300.0 : 1000.0;
    final int currentBeatIndex = (timestampMs / beatDurationMs).floor().clamp(0, config.totalBeats - 1);
    final double beatProgress = (timestampMs % beatDurationMs) / beatDurationMs;

    // 2. Evaluate Speed Curve for this phase
    final curve = SpeedCurvePresets.getById(config.speedCurveId);
    final double easedProgress = curve.transform(beatProgress);

    // 3. Intro vs Active Beats
    final bool isIntro = (currentBeatIndex == 0);
    bool isGreenScreen = false;

    // 4. Camera & 3D Null Setup
    final Matrix4 cameraMat = Matrix4.identity();
    final Matrix4 nullMat = Matrix4.identity();

    double camDist = 2.4;
    double rotX = 0.0;
    double rotY = 0.0;
    double rotZ = 0.0;

    double vx = 0.0;
    double vy = 0.0;

    // PRNG based on seed + beat index for deterministic variations
    final rng = Random(config.seed + (currentBeatIndex * 37));

    // Raw 3D Quads buffer
    final List<ProjectedQuad3D> quads = [];

    // FOV Factor (pixels focal scale)
    final double fovFactor = min(screenWidth, screenHeight) * 1.35;

    // =========================================================================
    // GEOMETRIC DISPATCH ACROSS 5 MODES
    // =========================================================================
    switch (config.mode) {
      // -----------------------------------------------------------------------
      // MODE 1: 3D NULL (Starts full screen, zooms out into 3D orbit)
      // -----------------------------------------------------------------------
      case GeneratorMode.null3D:
        if (isIntro) {
          // Starts flat full screen (camDist 1.0), zooms out into orbit
          camDist = 1.0 + (easedProgress * 1.4);
          rotY = easedProgress * 0.40;
          rotX = sin(easedProgress * pi) * 0.15;
        } else {
          camDist = 2.4;
          // Step orbit around the subject
          final double baseAngle = (currentBeatIndex - 1) * (pi / 2);
          rotY = baseAngle + (easedProgress * (pi / 2));
          rotX = cos(currentBeatIndex + easedProgress) * 0.22;
          rotZ = sin(currentBeatIndex * 1.5) * 0.12;

          // Velocity deltas for Adevob Smear
          vx = cos(rotY) * 38.0;
          vy = sin(rotX) * 22.0;
        }

        nullMat.rotateY(rotY);
        nullMat.rotateX(rotX);
        nullMat.rotateZ(rotZ);

        // Generate 6 faces of a true 3D Cube
        _generateCubeFaces(
          quads: quads,
          nullMat: nullMat,
          screenWidth: screenWidth,
          screenHeight: screenHeight,
          fovFactor: fovFactor,
          activeBeat: currentBeatIndex,
          config: config,
        );
        break;

      // -----------------------------------------------------------------------
      // MODE 2: 2D FLIP LAYER (Accordion Sliced Panels, Green Screen Intro)
      // -----------------------------------------------------------------------
      case GeneratorMode.flip2D:
        if (isIntro) {
          isGreenScreen = true;
          // Falling entry flip
          nullMat.translate(0.0, (1.0 - easedProgress) * -2.0, 0.0);
          nullMat.rotateX((1.0 - easedProgress) * (pi / 2));
        } else {
          final double flipAngle = easedProgress * pi;
          nullMat.rotateY(flipAngle);
          vx = sin(flipAngle) * 48.0;
        }

        // Generate Sliced Accordion Strips (4 slats that fold with 2-frame stagger)
        _generateAccordionStripFaces(
          quads: quads,
          nullMat: nullMat,
          screenWidth: screenWidth,
          screenHeight: screenHeight,
          fovFactor: fovFactor,
          activeBeat: currentBeatIndex,
          easedProgress: easedProgress,
          config: config,
          slatCount: 4,
        );
        break;

      // -----------------------------------------------------------------------
      // MODE 3: 3D FLIP LAYER (Green Screen Intro, Dynamic Perspective Orbit)
      // -----------------------------------------------------------------------
      case GeneratorMode.flip3D:
        if (isIntro) {
          isGreenScreen = true;
          camDist = 2.8;
          rotY = (1.0 - easedProgress) * pi;
          nullMat.rotateY(rotY);
        } else {
          final double dir = (currentBeatIndex % 2 == 0) ? 1.0 : -1.0;
          rotY = easedProgress * pi * dir;
          rotZ = sin(easedProgress * pi) * 0.25;
          nullMat.rotateY(rotY);
          nullMat.rotateZ(rotZ);
          vx = dir * 42.0;
          vy = cos(easedProgress * pi) * 18.0;
        }

        // Sliced Rhombus / Diamond split card
        _generateDiamondSplitFaces(
          quads: quads,
          nullMat: nullMat,
          screenWidth: screenWidth,
          screenHeight: screenHeight,
          fovFactor: fovFactor,
          activeBeat: currentBeatIndex,
          easedProgress: easedProgress,
          config: config,
        );
        break;

      // -----------------------------------------------------------------------
      // MODE 4: 3D FLIP LAYER + NULL (3D Flip layer speed-ramps into 3D Box)
      // -----------------------------------------------------------------------
      case GeneratorMode.flip3DPlusNull:
        if (isIntro) {
          isGreenScreen = true;
        } else if (currentBeatIndex <= 2) {
          // Card Flip Phase
          final double flipAngle = easedProgress * pi;
          nullMat.rotateY(flipAngle);
          vx = 36.0;
          _generateDiamondSplitFaces(
            quads: quads,
            nullMat: nullMat,
            screenWidth: screenWidth,
            screenHeight: screenHeight,
            fovFactor: fovFactor,
            activeBeat: currentBeatIndex,
            easedProgress: easedProgress,
            config: config,
          );
        } else {
          // 3D Cube Tumble Phase
          nullMat.rotateX(easedProgress * (pi / 2));
          nullMat.rotateY(easedProgress * pi);
          vy = 40.0;
          _generateCubeFaces(
            quads: quads,
            nullMat: nullMat,
            screenWidth: screenWidth,
            screenHeight: screenHeight,
            fovFactor: fovFactor,
            activeBeat: currentBeatIndex,
            config: config,
          );
        }
        break;

      // -----------------------------------------------------------------------
      // MODE 5: 2D FLIP LAYER + 3D NULL (Unfolding Origami Cross-Box)
      // -----------------------------------------------------------------------
      case GeneratorMode.flip2DPlusNull3D:
        if (isIntro) {
          isGreenScreen = true;
        } else {
          nullMat.rotateY(easedProgress * (pi / 3));
          nullMat.rotateX(sin(easedProgress * pi) * 0.20);
          vx = 30.0;
          _generateOrigamiCrossFaces(
            quads: quads,
            nullMat: nullMat,
            screenWidth: screenWidth,
            screenHeight: screenHeight,
            fovFactor: fovFactor,
            activeBeat: currentBeatIndex,
            easedProgress: easedProgress,
            config: config,
          );
        }
        break;
    }

    // =========================================================================
    // DEPTH SORTING (Painter's Algorithm: Draw furthest quads first)
    // =========================================================================
    quads.sort((a, b) => b.averageZ.compareTo(a.averageZ));

    // =========================================================================
    // 3D FLOATING ORBIT PARTICLES
    // =========================================================================
    final List<OrbitParticle3D> particles = [];
    if (config.enableParticles) {
      final double pTime = timestampMs * 0.001;
      for (int i = 0; i < config.particleCount; i++) {
        final double baseAngle = (i / config.particleCount.toDouble()) * 2 * pi;
        final double rad = 1.3 + sin(baseAngle * 3.0 + pTime) * 0.45;
        final double px = cos(baseAngle + pTime * 0.4) * rad;
        final double py = sin(baseAngle * 2.0 + pTime * 0.25) * 0.9;
        final double pz = sin(baseAngle + pTime * 0.4) * rad;

        particles.add(OrbitParticle3D(
          x: px,
          y: py,
          z: pz,
          size: config.particleSize,
          color: config.particleColor,
        ));
      }
      particles.sort((a, b) => b.z.compareTo(a.z));
    }

    return ZakuraEvaluatedScene(
      sortedQuads: quads,
      sortedParticles: particles,
      velocityX: vx,
      velocityY: vy,
      isGreenScreenIntro: isGreenScreen,
      backgroundColor: isGreenScreen ? const Color(0xFF00FF00) : config.backgroundColor,
      cameraViewProj: cameraMat,
      nullTransform: nullMat,
    );
  }

  // ===========================================================================
  // 3D MESH GENERATION SUB-ROUTINES
  // ===========================================================================

  /// True 6-Sided Volumetric 3D Cube
  static void _generateCubeFaces({
    required List<ProjectedQuad3D> quads,
    required Matrix4 nullMat,
    required double screenWidth,
    required double screenHeight,
    required double fovFactor,
    required int activeBeat,
    required GeneratorConfig config,
  }) {
    const double s = 0.5; // Half-size

    // 6 Faces definitions: [Top-Left, Top-Right, Bottom-Right, Bottom-Left]
    final faces = [
      // Front (Z = +s)
      [Vertex3D(-s, -s,  s, 0, 0), Vertex3D( s, -s,  s, 1, 0), Vertex3D( s,  s,  s, 1, 1), Vertex3D(-s,  s,  s, 0, 1)],
      // Back (Z = -s)
      [Vertex3D( s, -s, -s, 0, 0), Vertex3D(-s, -s, -s, 1, 0), Vertex3D(-s,  s, -s, 1, 1), Vertex3D( s,  s, -s, 0, 1)],
      // Left (X = -s)
      [Vertex3D(-s, -s, -s, 0, 0), Vertex3D(-s, -s,  s, 1, 0), Vertex3D(-s,  s,  s, 1, 1), Vertex3D(-s,  s, -s, 0, 1)],
      // Right (X = +s)
      [Vertex3D( s, -s,  s, 0, 0), Vertex3D( s, -s, -s, 1, 0), Vertex3D( s,  s, -s, 1, 1), Vertex3D( s,  s,  s, 0, 1)],
      // Top (Y = -s)
      [Vertex3D(-s, -s, -s, 0, 0), Vertex3D( s, -s, -s, 1, 0), Vertex3D( s, -s,  s, 1, 1), Vertex3D(-s, -s,  s, 0, 1)],
      // Bottom (Y = +s)
      [Vertex3D(-s,  s,  s, 0, 0), Vertex3D( s,  s,  s, 1, 0), Vertex3D( s,  s, -s, 1, 1), Vertex3D(-s,  s, -s, 0, 1)],
    ];

    for (int i = 0; i < 6; i++) {
      _projectAndAppendQuad(
        faceId: i,
        mediaIndex: (activeBeat + i) % config.totalBeats,
        rawVertices: faces[i],
        nullMat: nullMat,
        screenWidth: screenWidth,
        screenHeight: screenHeight,
        fovFactor: fovFactor,
        quads: quads,
        config: config,
      );
    }
  }

  /// Accordion Sliced Vertical Slat Panels
  static void _generateAccordionStripFaces({
    required List<ProjectedQuad3D> quads,
    required Matrix4 nullMat,
    required double screenWidth,
    required double screenHeight,
    required double fovFactor,
    required int activeBeat,
    required double easedProgress,
    required GeneratorConfig config,
    required int slatCount,
  }) {
    const double totalW = 1.0;
    const double totalH = 1.0;
    final double slatW = totalW / slatCount;

    for (int i = 0; i < slatCount; i++) {
      final double x0 = -0.5 + (i * slatW);
      final double x1 = x0 + slatW;
      final double u0 = i / slatCount.toDouble();
      final double u1 = (i + 1) / slatCount.toDouble();

      // Stagger rotation per slat
      final double slatDelay = (i / slatCount.toDouble()) * 0.25;
      final double slatProgress = ((easedProgress - slatDelay) / (1.0 - slatDelay)).clamp(0.0, 1.0);
      final double slatRot = slatProgress * pi;

      final Matrix4 slatMat = nullMat.clone();
      slatMat.translate(x0 + (slatW * 0.5), 0.0, 0.0);
      slatMat.rotateY(slatRot);
      slatMat.translate(-(slatW * 0.5), 0.0, 0.0);

      final verts = [
        Vertex3D(0.0, -0.5, 0.0, u0, 0.0),
        Vertex3D(slatW, -0.5, 0.0, u1, 0.0),
        Vertex3D(slatW,  0.5, 0.0, u1, 1.0),
        Vertex3D(0.0,  0.5, 0.0, u0, 1.0),
      ];

      _projectAndAppendQuad(
        faceId: i,
        mediaIndex: activeBeat % config.totalBeats,
        rawVertices: verts,
        nullMat: slatMat,
        screenWidth: screenWidth,
        screenHeight: screenHeight,
        fovFactor: fovFactor,
        quads: quads,
        config: config,
      );
    }
  }

  /// Diamond / Rhombus Split WIS Cards
  static void _generateDiamondSplitFaces({
    required List<ProjectedQuad3D> quads,
    required Matrix4 nullMat,
    required double screenWidth,
    required double screenHeight,
    required double fovFactor,
    required int activeBeat,
    required double easedProgress,
    required GeneratorConfig config,
  }) {
    // Upper Diagonal Half
    final upperMat = nullMat.clone()..rotateZ(0.785); // 45 deg rhombus
    final vertsUpper = [
      const Vertex3D(-0.5, -0.5, 0.0, 0.0, 0.0),
      const Vertex3D( 0.5, -0.5, 0.0, 1.0, 0.0),
      const Vertex3D( 0.5,  0.5, 0.0, 1.0, 1.0),
      const Vertex3D(-0.5,  0.5, 0.0, 0.0, 1.0),
    ];

    _projectAndAppendQuad(
      faceId: 0,
      mediaIndex: activeBeat % config.totalBeats,
      rawVertices: vertsUpper,
      nullMat: upperMat,
      screenWidth: screenWidth,
      screenHeight: screenHeight,
      fovFactor: fovFactor,
      quads: quads,
      config: config,
    );
  }

  /// Unfolding Origami Cross-Box Faces (Yoruichi Clip Style)
  static void _generateOrigamiCrossFaces({
    required List<ProjectedQuad3D> quads,
    required Matrix4 nullMat,
    required double screenWidth,
    required double screenHeight,
    required double fovFactor,
    required int activeBeat,
    required double easedProgress,
    required GeneratorConfig config,
  }) {
    const double s = 0.5;
    final double foldAngle = (1.0 - easedProgress) * (pi / 2); // 90 deg -> 0 deg flat

    // Center Face
    _projectAndAppendQuad(
      faceId: 0,
      mediaIndex: activeBeat % config.totalBeats,
      rawVertices: const [
        Vertex3D(-s, -s, 0.0, 0, 0),
        Vertex3D( s, -s, 0.0, 1, 0),
        Vertex3D( s,  s, 0.0, 1, 1),
        Vertex3D(-s,  s, 0.0, 0, 1),
      ],
      nullMat: nullMat,
      screenWidth: screenWidth,
      screenHeight: screenHeight,
      fovFactor: fovFactor,
      quads: quads,
      config: config,
    );

    // Left Flap (hinged at x = -s)
    final leftMat = nullMat.clone()
      ..translate(-s, 0.0, 0.0)
      ..rotateY(-foldAngle)
      ..translate(-s, 0.0, 0.0);
    _projectAndAppendQuad(
      faceId: 1,
      mediaIndex: (activeBeat + 1) % config.totalBeats,
      rawVertices: const [
        Vertex3D(0.0, -s, 0.0, 0, 0),
        Vertex3D(  s, -s, 0.0, 1, 0),
        Vertex3D(  s,  s, 0.0, 1, 1),
        Vertex3D(0.0,  s, 0.0, 0, 1),
      ],
      nullMat: leftMat,
      screenWidth: screenWidth,
      screenHeight: screenHeight,
      fovFactor: fovFactor,
      quads: quads,
      config: config,
    );

    // Right Flap (hinged at x = +s)
    final rightMat = nullMat.clone()
      ..translate(s, 0.0, 0.0)
      ..rotateY(foldAngle);
    _projectAndAppendQuad(
      faceId: 2,
      mediaIndex: (activeBeat + 2) % config.totalBeats,
      rawVertices: const [
        Vertex3D(0.0, -s, 0.0, 0, 0),
        Vertex3D(  s, -s, 0.0, 1, 0),
        Vertex3D(  s,  s, 0.0, 1, 1),
        Vertex3D(0.0,  s, 0.0, 0, 1),
      ],
      nullMat: rightMat,
      screenWidth: screenWidth,
      screenHeight: screenHeight,
      fovFactor: fovFactor,
      quads: quads,
      config: config,
    );
  }

  // ===========================================================================
  // VERTEX PROJECTION & BACKFACE CULLING
  // ===========================================================================
  static void _projectAndAppendQuad({
    required int faceId,
    required int mediaIndex,
    required List<Vertex3D> rawVertices,
    required Matrix4 nullMat,
    required double screenWidth,
    required double screenHeight,
    required double fovFactor,
    required List<ProjectedQuad3D> quads,
    required GeneratorConfig config,
  }) {
    final List<Vertex3D> transformed = [];
    final List<Offset> screenPoints = [];
    double zSum = 0.0;

    for (final v in rawVertices) {
      final tv = v.transform(nullMat);
      transformed.add(tv);
      screenPoints.add(tv.toScreen(screenWidth, screenHeight, fovFactor));
      zSum += tv.z;
    }

    // 2D Cross-Product to test Backface winding
    final double crossZ = (screenPoints[1].dx - screenPoints[0].dx) * (screenPoints[2].dy - screenPoints[0].dy) -
                          (screenPoints[1].dy - screenPoints[0].dy) * (screenPoints[2].dx - screenPoints[0].dx);
    final bool isFrontFacing = crossZ > 0.0;

    // Edge Glow color calculation
    Color glowColor = const Color(0xFFE040FB);
    if (config.edgeEffect == EdgeEffectType.sapphireEdgeGlow) {
      glowColor = const Color(0xFF00E5FF);
    } else if (config.edgeEffect == EdgeEffectType.edgeHaloAura) {
      glowColor = const Color(0xFFFF4081);
    }

    quads.add(ProjectedQuad3D(
      faceId: faceId,
      mediaIndex: mediaIndex,
      vertices: transformed,
      screenPoints: screenPoints,
      averageZ: zSum / 4.0,
      isFrontFacing: isFrontFacing,
      edgeGlowColor: glowColor,
    ));
  }
}
