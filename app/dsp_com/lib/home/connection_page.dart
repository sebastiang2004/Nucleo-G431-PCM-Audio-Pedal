import 'package:flutter/material.dart';

import 'widgets/connection_section.dart';
import 'widgets/effects_section.dart';

class ConnectionPage extends StatelessWidget {
  final List<String> ports;
  final String? selectedPort;
  final ValueChanged<String?> onPortChanged;

  final List<int> baudRates;
  final int baudRate;
  final ValueChanged<int?> onBaudChanged;

  final bool connected;
  final bool ready;
  final VoidCallback onRefreshPorts;
  final Future<void> Function() onConnectPressed;

  final String lastAction;
  final String lastDeviceLine;

  final bool distortion;
  final bool reverb;
  final bool delay;
  final ValueChanged<bool> onDistortionChanged;
  final ValueChanged<bool> onReverbChanged;
  final ValueChanged<bool> onDelayChanged;

  const ConnectionPage({
    super.key,
    required this.ports,
    required this.selectedPort,
    required this.onPortChanged,
    required this.baudRates,
    required this.baudRate,
    required this.onBaudChanged,
    required this.connected,
    required this.ready,
    required this.onRefreshPorts,
    required this.onConnectPressed,
    required this.lastAction,
    required this.lastDeviceLine,
    required this.distortion,
    required this.reverb,
    required this.delay,
    required this.onDistortionChanged,
    required this.onReverbChanged,
    required this.onDelayChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Connection'),
        actions: [
          IconButton(
            tooltip: 'Refresh ports',
            onPressed: connected ? null : onRefreshPorts,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            ConnectionSection(
              ports: ports,
              selectedPort: selectedPort,
              onPortChanged: onPortChanged,
              baudRates: baudRates,
              baudRate: baudRate,
              onBaudChanged: onBaudChanged,
              connected: connected,
              ready: ready,
              onConnectPressed: () async {
                try {
                  await onConnectPressed();
                } catch (e) {
                  if (!context.mounted) return;
                  ScaffoldMessenger.of(
                    context,
                  ).showSnackBar(SnackBar(content: Text('$e')));
                }
              },
            ),
            if (lastAction.isNotEmpty) Text('Action: $lastAction'),
            if (lastDeviceLine.isNotEmpty) Text('Device: $lastDeviceLine'),
            const SizedBox(height: 18),
            const Text(
              'Effects',
              style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 8),
            EffectsSection(
              ready: ready,
              distortion: distortion,
              reverb: reverb,
              delay: delay,
              onDistortionChanged: onDistortionChanged,
              onReverbChanged: onReverbChanged,
              onDelayChanged: onDelayChanged,
            ),
          ],
        ),
      ),
    );
  }
}
