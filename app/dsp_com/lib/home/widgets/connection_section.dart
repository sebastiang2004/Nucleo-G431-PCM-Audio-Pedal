import 'package:flutter/material.dart';

class ConnectionSection extends StatelessWidget {
  final List<String> ports;
  final String? selectedPort;
  final ValueChanged<String?> onPortChanged;

  final List<int> baudRates;
  final int baudRate;
  final ValueChanged<int?> onBaudChanged;

  final bool connected;
  final bool ready;

  final VoidCallback onConnectPressed;

  const ConnectionSection({
    super.key,
    required this.ports,
    required this.selectedPort,
    required this.onPortChanged,
    required this.baudRates,
    required this.baudRate,
    required this.onBaudChanged,
    required this.connected,
    required this.ready,
    required this.onConnectPressed,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Connect',
          style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: DropdownButtonFormField<String>(
                initialValue: selectedPort,
                items: ports
                    .map(
                      (p) => DropdownMenuItem<String>(value: p, child: Text(p)),
                    )
                    .toList(growable: false),
                onChanged: connected ? null : onPortChanged,
                decoration: const InputDecoration(labelText: 'COM port'),
              ),
            ),
            const SizedBox(width: 12),
            SizedBox(
              width: 160,
              child: DropdownButtonFormField<int>(
                initialValue: baudRate,
                items: baudRates
                    .map(
                      (b) => DropdownMenuItem<int>(value: b, child: Text('$b')),
                    )
                    .toList(growable: false),
                onChanged: connected ? null : onBaudChanged,
                decoration: const InputDecoration(labelText: 'Baud'),
              ),
            ),
            const SizedBox(width: 12),
            FilledButton(
              onPressed: onConnectPressed,
              child: Text(connected ? 'Disconnect' : 'Connect'),
            ),
          ],
        ),
        const SizedBox(height: 10),
        Text(
          'Status: ${ready
              ? 'Connected'
              : connected
              ? 'Port open (waiting)'
              : 'Not connected'}',
        ),
      ],
    );
  }
}
