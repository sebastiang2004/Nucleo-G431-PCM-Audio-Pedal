import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'image_knob.dart';

class PedalSection extends StatelessWidget {
  final bool ready;

  final String pedalBgAsset;
  final String knobAsset;

  final double timePct;
  final double mixPct;
  final double feedbackPct;
  final double offssetPct;
  final double balancePct;
  final double filterPct;

  final ValueChanged<double> onTimeChanged;
  final ValueChanged<double> onMixChanged;
  final ValueChanged<double> onFeedbackChanged;
  final ValueChanged<double> onOffssetChanged;
  final ValueChanged<double> onBalanceChanged;
  final ValueChanged<double> onFilterChanged;

  final double volumePct;
  final double volumeMaxPct;
  final ValueChanged<double> onVolumeChanged;

  // Effect toggles
  final bool distortion;
  final bool reverb;
  final bool delay;
  final ValueChanged<bool> onDistortionChanged;
  final ValueChanged<bool> onReverbChanged;
  final ValueChanged<bool> onDelayChanged;

  const PedalSection({
    super.key,
    required this.ready,
    required this.pedalBgAsset,
    required this.knobAsset,
    required this.timePct,
    required this.mixPct,
    required this.feedbackPct,
    required this.offssetPct,
    required this.balancePct,
    required this.filterPct,
    required this.onTimeChanged,
    required this.onMixChanged,
    required this.onFeedbackChanged,
    required this.onOffssetChanged,
    required this.onBalanceChanged,
    required this.onFilterChanged,
    required this.volumePct,
    required this.volumeMaxPct,
    required this.onVolumeChanged,
    required this.distortion,
    required this.reverb,
    required this.delay,
    required this.onDistortionChanged,
    required this.onReverbChanged,
    required this.onDelayChanged,
  });

  // Color scheme matching the Japanese artwork background
  static const _pedalBg = Color(0xFF1A1A1A);
  static const _pedalBgGradientTop = Color(0xFF2D2D2D);
  static const _accentRed = Color(0xFFC62828);     // Deep crimson (matches red sun)
  static const _accentOrange = Color(0xFFD84315);  // Burnt orange
  static const _accentGreen = Color(0xFFFFAB00);   // Warm amber LED
  static const _labelColor = Color(0xFFE6DDCF);    // Warm off-white (readable, not pure white)

  Widget _buildFootswitch({
    required String label,
    required bool active,
    required VoidCallback onTap,
  }) {
    final ledColor = active ? _accentGreen : Colors.grey[800]!;
    final glowColor = active
        // ignore: deprecated_member_use
        ? _accentGreen.withOpacity(0.6)
        : Colors.transparent;

    return GestureDetector(
      onTap: onTap,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          // LED indicator
          Container(
            width: 10,
            height: 10,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: ledColor,
              boxShadow: active
                  ? [
                      BoxShadow(
                        color: glowColor,
                        blurRadius: 8,
                        spreadRadius: 2,
                      ),
                    ]
                  : null,
            ),
          ),
          const SizedBox(height: 4),
          // Footswitch button
          Container(
            width: 50,
            height: 20,
            decoration: BoxDecoration(
              gradient: LinearGradient(
                begin: Alignment.topCenter,
                end: Alignment.bottomCenter,
                colors: [
                  const Color(0xFF4A4A4A),
                  const Color(0xFF2A2A2A),
                  const Color(0xFF1A1A1A),
                ],
              ),
              borderRadius: BorderRadius.circular(4),
              border: Border.all(
                color: active ? _accentOrange : Colors.grey[700]!,
                width: 1,
              ),
              boxShadow: [
                BoxShadow(
                  // ignore: deprecated_member_use
                  color: Colors.black.withOpacity(0.5),
                  blurRadius: 4,
                  offset: const Offset(0, 2),
                ),
              ],
            ),
          ),
          const SizedBox(height: 4),
          Text(
            label,
            style: TextStyle(
              fontSize: 9,
              fontWeight: FontWeight.w700,
              // ignore: deprecated_member_use
              color: active ? _accentOrange : _labelColor.withOpacity(0.7),
              letterSpacing: 0.5,
            ),
          ),
        ],
      ),
    );
  }

  Widget _imgPedalKnob({
    required BuildContext context,
    required String label,
    required double valuePct,
    required double size,
    required ValueChanged<double> onChanged,
    double maxPct = 100,
  }) {
    return Opacity(
      opacity: ready ? 1 : 0.85,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          ImageKnob(
            assetPath: knobAsset,
            size: size,
            valuePct: valuePct,
            minPct: 0,
            maxPct: maxPct,
            onChanged: onChanged,
          ),
          const SizedBox(height: 6),
          Text(
            label,
            textAlign: TextAlign.center,
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w800,
              color: _labelColor,
              letterSpacing: 0.5,
              height: 1.05,
              shadows: [
                // Dark outline for visibility on light areas
                Shadow(offset: const Offset(-1, -1), color: Colors.black54, blurRadius: 1),
                Shadow(offset: const Offset(1, -1), color: Colors.black54, blurRadius: 1),
                Shadow(offset: const Offset(-1, 1), color: Colors.black54, blurRadius: 1),
                Shadow(offset: const Offset(1, 1), color: Colors.black54, blurRadius: 1),
                // Light glow for visibility on dark areas
                Shadow(color: Colors.white38, blurRadius: 3),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildPedalImage(BuildContext context) {
    Theme.of(context);

    return ClipRRect(
      borderRadius: BorderRadius.circular(18),
      child: LayoutBuilder(
        builder: (context, c) {
          final w = c.maxWidth;
          final h = c.maxHeight;

          final knobSmall = w * 0.24;
          final knobLarge = w * 0.30;

          // Approximate placement based on the Figma frame size (358x606).
          final x1 = w * 0.19;
          final x2 = w * 0.50;
          final x3 = w * 0.81;
          final yTop = h * 0.12;
          final yMid = h * 0.36;
          final yBottom = h * 0.68;

          Positioned placeKnob({
            required double cx,
            required double cy,
            required double size,
            required Widget child,
          }) {
            return Positioned(
              left: cx - size / 2,
              top: cy - size / 2,
              width: size,
              child: child,
            );
          }

          return Stack(
            children: [
              // Aggressive dark gradient background
              Positioned.fill(
                child: Image.asset(
                  pedalBgAsset,
                  fit: BoxFit.cover,
                  errorBuilder: (context, _, _) {
                    return DecoratedBox(
                      decoration: BoxDecoration(
                        gradient: LinearGradient(
                          begin: Alignment.topCenter,
                          end: Alignment.bottomCenter,
                          colors: [_pedalBgGradientTop, _pedalBg],
                        ),
                        border: Border.all(
                          // ignore: deprecated_member_use
                          color: _accentRed.withOpacity(0.6),
                          width: 2,
                        ),
                        borderRadius: BorderRadius.circular(16),
                      ),
                    );
                  },
                ),
              ),

              // Knobs
              placeKnob(
                cx: x1,
                cy: yTop,
                size: knobSmall,
                child: _imgPedalKnob(
                  context: context,
                  label: 'DELAY\nMIX',
                  valuePct: timePct,
                  size: knobSmall,
                  onChanged: onTimeChanged,
                ),
              ),
              placeKnob(
                cx: x2,
                cy: yTop,
                size: knobSmall,
                child: _imgPedalKnob(
                  context: context,
                  label: 'REVERB\nMIX',
                  valuePct: mixPct,
                  size: knobSmall,
                  onChanged: onMixChanged,
                ),
              ),
              placeKnob(
                cx: x3,
                cy: yTop,
                size: knobSmall,
                child: _imgPedalKnob(
                  context: context,
                  label: 'DELAY\nFB',
                  valuePct: feedbackPct,
                  size: knobSmall,
                  onChanged: onFeedbackChanged,
                ),
              ),
              placeKnob(
                cx: x1,
                cy: yMid,
                size: knobSmall,
                child: _imgPedalKnob(
                  context: context,
                  label: 'DRIVE',
                  valuePct: offssetPct,
                  size: knobSmall,
                  onChanged: onOffssetChanged,
                ),
              ),
              placeKnob(
                cx: x2,
                cy: yMid,
                size: knobLarge,
                child: _imgPedalKnob(
                  context: context,
                  label: 'REVERB\nDECAY',
                  valuePct: balancePct,
                  size: knobLarge,
                  onChanged: onBalanceChanged,
                ),
              ),
              placeKnob(
                cx: x3,
                cy: yMid,
                size: knobSmall,
                child: _imgPedalKnob(
                  context: context,
                  label: 'REVERB\nDAMP',
                  valuePct: filterPct,
                  size: knobSmall,
                  onChanged: onFilterChanged,
                ),
              ),

              // Master Volume inside the pedal container (centered).
              placeKnob(
                cx: w * 0.50,
                cy: yBottom,
                size: (w * 0.34).clamp(90.0, 130.0),
                child: _imgPedalKnob(
                  context: context,
                  label: 'MASTER\nVOLUME',
                  valuePct: volumePct,
                  size: (w * 0.34).clamp(90.0, 130.0),
                  maxPct: volumeMaxPct,
                  onChanged: onVolumeChanged,
                ),
              ),

              // Effect footswitch buttons
              Positioned(
                left: w * 0.05,
                right: w * 0.05,
                bottom: h * 0.02,
                child: Row(
                  mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                  children: [
                    _buildFootswitch(
                      label: 'DIST',
                      active: distortion,
                      onTap: () => onDistortionChanged(!distortion),
                    ),
                    _buildFootswitch(
                      label: 'REVERB',
                      active: reverb,
                      onTap: () => onReverbChanged(!reverb),
                    ),
                    _buildFootswitch(
                      label: 'DELAY',
                      active: delay,
                      onTap: () => onDelayChanged(!delay),
                    ),
                  ],
                ),
              ),
            ],
          );
        },
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, c) {
        const pedalAspect = 358 / 606;

        final maxW = c.maxWidth.isFinite ? c.maxWidth : 420.0;
        final maxH = c.maxHeight.isFinite ? c.maxHeight : 760.0;

        final targetMaxWidth = (maxW * 0.95).clamp(320.0, 560.0);

        // Account for Card padding (12 on each side).
        final innerMaxW = math.max(0.0, targetMaxWidth - 24.0);
        final innerMaxH = math.max(0.0, maxH - 24.0);

        final pedalWidth = math.min(innerMaxW, innerMaxH * pedalAspect);
        final pedalHeight = pedalWidth / pedalAspect;

        return Center(
          child: ConstrainedBox(
            constraints: BoxConstraints(maxWidth: targetMaxWidth),
            child: Card(
              clipBehavior: Clip.antiAlias,
              color: _pedalBg,
              elevation: 12,
              // ignore: deprecated_member_use
              shadowColor: _accentRed.withOpacity(0.4),
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(20),
                side: BorderSide(
                  // ignore: deprecated_member_use
                  color: _accentOrange.withOpacity(0.5),
                  width: 1.5,
                ),
              ),
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: SizedBox(
                  width: pedalWidth,
                  height: pedalHeight,
                  child: _buildPedalImage(context),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}
