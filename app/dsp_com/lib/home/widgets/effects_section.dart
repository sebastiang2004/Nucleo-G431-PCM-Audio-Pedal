import 'package:flutter/material.dart';

class EffectsSection extends StatelessWidget {
  final bool ready;
  final bool distortion;
  final bool reverb;
  final bool delay;

  final ValueChanged<bool> onDistortionChanged;
  final ValueChanged<bool> onReverbChanged;
  final ValueChanged<bool> onDelayChanged;

  const EffectsSection({
    super.key,
    required this.ready,
    required this.distortion,
    required this.reverb,
    required this.delay,
    required this.onDistortionChanged,
    required this.onReverbChanged,
    required this.onDelayChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        FilterChip(
          label: const Text('Distortion'),
          selected: distortion,
          materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
          visualDensity: VisualDensity.compact,
          onSelected: ready ? onDistortionChanged : null,
        ),
        FilterChip(
          label: const Text('Reverb'),
          selected: reverb,
          materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
          visualDensity: VisualDensity.compact,
          onSelected: ready ? onReverbChanged : null,
        ),
        FilterChip(
          label: const Text('Delay'),
          selected: delay,
          materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
          visualDensity: VisualDensity.compact,
          onSelected: ready ? onDelayChanged : null,
        ),
      ],
    );
  }
}
