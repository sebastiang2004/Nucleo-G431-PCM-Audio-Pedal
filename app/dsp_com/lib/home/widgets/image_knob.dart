import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

class ImageKnob extends StatefulWidget {
  final String assetPath;
  final double size;
  final double valuePct;
  final double minPct;
  final double maxPct;
  final ValueChanged<double> onChanged;
  final double maxRotationDeg;
  final double dragSensitivity;

  const ImageKnob({
    super.key,
    required this.assetPath,
    required this.size,
    required this.valuePct,
    required this.minPct,
    required this.maxPct,
    required this.onChanged,
    this.maxRotationDeg = 270,
    this.dragSensitivity = 0.45,
  });

  @override
  State<ImageKnob> createState() => _ImageKnobState();
}

class _ImageKnobState extends State<ImageKnob> {
  late double _value;
  bool _isDragging = false;

  @override
  void initState() {
    super.initState();
    _value = widget.valuePct;
  }

  @override
  void didUpdateWidget(covariant ImageKnob oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!_isDragging && oldWidget.valuePct != widget.valuePct) {
      _value = widget.valuePct;
    }
  }

  void _setValue(double next) {
    final clamped = next.clamp(widget.minPct, widget.maxPct);
    if (clamped == _value) return;
    setState(() => _value = clamped);
    widget.onChanged(_value);
  }

  void _applyDeltaDy(double dy) {
    _setValue(_value + (-dy) * widget.dragSensitivity);
  }

  @override
  Widget build(BuildContext context) {
    final range = (widget.maxPct - widget.minPct);
    final t = range <= 0
        ? 0.0
        : ((_value - widget.minPct) / range).clamp(0.0, 1.0);

    return SizedBox(
      width: widget.size,
      height: widget.size,
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerSignal: (signal) {
          if (signal is PointerScrollEvent) {
            _applyDeltaDy(signal.scrollDelta.dy);
          }
        },
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onPanStart: (_) => setState(() => _isDragging = true),
          onPanEnd: (_) => setState(() => _isDragging = false),
          onPanCancel: () => setState(() => _isDragging = false),
          onPanUpdate: (details) => _applyDeltaDy(details.delta.dy),
          child: CustomPaint(
            size: Size(widget.size, widget.size),
            painter: _AggressiveKnobPainter(
              value: t,
              maxRotationDeg: widget.maxRotationDeg,
            ),
          ),
        ),
      ),
    );
  }
}

class _AggressiveKnobPainter extends CustomPainter {
  final double value;
  final double maxRotationDeg;

  _AggressiveKnobPainter({required this.value, this.maxRotationDeg = 270});

  @override
  void paint(Canvas canvas, Size size) {
    final center = Offset(size.width / 2, size.height / 2);
    final radius = size.width / 2;

    // Outer shadow for depth
    final shadowPaint = Paint()
      // ignore: deprecated_member_use
      ..color = Colors.black.withOpacity(0.5)
      ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 8);
    canvas.drawCircle(center + const Offset(2, 4), radius * 0.88, shadowPaint);

    // Main knob body - sleek matte black
    final knobPaint = Paint()
      ..shader = RadialGradient(
        center: const Alignment(-0.25, -0.35),
        colors: [
          const Color(0xFF404040),
          const Color(0xFF2A2A2A),
          const Color(0xFF1C1C1C),
        ],
        stops: const [0.0, 0.5, 1.0],
      ).createShader(Rect.fromCircle(center: center, radius: radius));
    canvas.drawCircle(center, radius * 0.88, knobPaint);

    // Subtle rim highlight
    final rimPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.0
      ..shader = SweepGradient(
        startAngle: -math.pi / 3,
        colors: [
          // ignore: deprecated_member_use
          Colors.white.withOpacity(0.15),
          Colors.transparent,
          Colors.transparent,
          // ignore: deprecated_member_use
          Colors.white.withOpacity(0.08),
        ],
        stops: const [0.0, 0.25, 0.75, 1.0],
      ).createShader(Rect.fromCircle(center: center, radius: radius * 0.88));
    canvas.drawCircle(center, radius * 0.87, rimPaint);

    // Indicator line position
    final maxRad = (maxRotationDeg * math.pi) / 180.0;
    final angle = -math.pi / 2 + (value - 0.5) * maxRad;

    final indicatorStart = center +
        Offset(
          math.cos(angle) * (radius * 0.30),
          math.sin(angle) * (radius * 0.30),
        );
    final indicatorEnd = center +
        Offset(
          math.cos(angle) * (radius * 0.72),
          math.sin(angle) * (radius * 0.72),
        );

    // Clean cream indicator line
    final indicatorPaint = Paint()
      ..color = const Color(0xFFF5F0E6)  // Cream matching artwork
      ..strokeWidth = 2.5
      ..strokeCap = StrokeCap.round;
    canvas.drawLine(indicatorStart, indicatorEnd, indicatorPaint);

    // Tiny center dimple
    final dimplePaint = Paint()
      ..shader = RadialGradient(
        colors: [
          const Color(0xFF1A1A1A),
          const Color(0xFF2A2A2A),
        ],
      ).createShader(Rect.fromCircle(center: center, radius: radius * 0.1));
    canvas.drawCircle(center, radius * 0.1, dimplePaint);
  }

  @override
  bool shouldRepaint(_AggressiveKnobPainter oldDelegate) =>
      oldDelegate.value != value;
}
