import 'package:flutter/foundation.dart';

/// Debug-only logger.
///
/// Calls are compiled out in release because they are wrapped in `assert`.
/// Use it like:
///   dlog(() => 'message');
/// so the message is only computed in debug.
typedef _Msg = String Function();

// ignore: library_private_types_in_public_api
void dlog(_Msg msg) {
  assert(() {
    final ts = DateTime.now().toIso8601String();
    debugPrint('[dsp_com][$ts] ${msg()}');
    return true;
  }());
}

// ignore: library_private_types_in_public_api
void dlogRx(_Msg msg) => dlog(() => 'RX ${msg()}');
// ignore: library_private_types_in_public_api
void dlogTx(_Msg msg) => dlog(() => 'TX ${msg()}');
// ignore: library_private_types_in_public_api
void dlogState(_Msg msg) => dlog(() => 'STATE ${msg()}');
