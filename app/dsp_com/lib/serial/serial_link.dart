import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

// ignore: depend_on_referenced_packages
import 'package:libserialport/libserialport.dart';

class SerialLink {
  SerialPort? _port;
  SerialPortReader? _reader;
  StreamSubscription<Uint8List>? _sub;

  final StringBuffer _rxBuf = StringBuffer();

  bool get isOpen => _port?.isOpen ?? false;
  String? get portName => _port?.name;

  Future<void> open({
    required String portName,
    required int baudRate,
    required void Function(String line) onLine,
  }) async {
    close();

    final port = SerialPort(portName);
    if (!port.openReadWrite()) {
      throw StateError('Failed to open $portName');
    }

    final config = SerialPortConfig();
    config.baudRate = baudRate;
    config.bits = 8;
    config.stopBits = 1;
    config.parity = SerialPortParity.none;
    config.setFlowControl(SerialPortFlowControl.none);
    port.config = config;

    _port = port;
    _reader = SerialPortReader(port);
    _sub = _reader!.stream.listen((data) {
      _ingest(data, onLine);
    });
  }

  void _ingest(Uint8List data, void Function(String line) onLine) {
    for (final b in data) {
      if (b == 10) {
        final line = _rxBuf.toString().trim();
        _rxBuf.clear();
        if (line.isNotEmpty) {
          onLine(line);
        }
      } else if (b == 13) {
        // ignore CR
      } else {
        _rxBuf.writeCharCode(b);
      }
    }
  }

  void sendLine(String line) {
    final port = _port;
    if (port == null || !port.isOpen) return;
    final bytes = Uint8List.fromList(utf8.encode('$line\n'));
    var offset = 0;
    while (offset < bytes.length) {
      final chunk = Uint8List.sublistView(bytes, offset);
      final written = port.write(chunk);
      if (written <= 0) {
        // If we can't write, bail to avoid spinning.
        break;
      }
      offset += written;
    }
  }

  void close() {
    _sub?.cancel();
    _sub = null;
    _reader = null;
    final p = _port;
    _port = null;
    if (p != null && p.isOpen) {
      p.close();
    }
    _rxBuf.clear();
  }
}
