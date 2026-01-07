import 'dart:async';

import 'package:flutter/material.dart';
// ignore: depend_on_referenced_packages
import 'package:libserialport/libserialport.dart';

import '../presets/presets.dart';
import '../serial/serial_link.dart';
import '../utils/debouncer.dart';
import '../utils/debug_log.dart';
import 'widgets/connection_section.dart';
import 'widgets/pedal_section.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  static const String _kPedalBgAsset = 'assets/background.jpg';
  static const String _kKnobAsset = 'assets/figma/empress_knob.png';

  final SerialLink _link = SerialLink();
  // Send params only when the user stops turning the knob.
  final Debouncer _paramDebounce = Debouncer(const Duration(milliseconds: 220));

  bool _deviceReady = false;
  bool _initialSyncDone = false;

  Timer? _healthTimer;
  DateTime? _lastRxAt;

  // TX coalescing: keep only the latest desired values, and send at most one
  // command at a time (wait for OK/ERR/timeout before sending the next).
  Timer? _txAckTimer;
  _PendingCmd? _pendingCmd;

  Timer? _retryTimer;
  bool _pumpScheduled = false;

  int _lastAppliedFxMask = 0;
  int? _desiredFxMask;

  final Map<String, int> _lastAppliedParams = {};
  final Map<String, int> _desiredParams = {};

  // Anti-spam: at most 2 attempts (send + one retry) per param/value.
  final Map<String, _PsetAttempts> _psetAttempts = {};

  void _clearPendingAcks() {
    dlogState(() => 'clearPendingAcks (pending=${_pendingCmd?.type})');
    _txAckTimer?.cancel();
    _txAckTimer = null;
    _pendingCmd = null;

    _retryTimer?.cancel();
    _retryTimer = null;

    _pumpScheduled = false;

    _psetAttempts.clear();
  }

  void _scheduleRetry() {
    _retryTimer?.cancel();
    _retryTimer = Timer(const Duration(milliseconds: 80), () {
      if (!mounted) return;
      dlogState(() => 'retry pumpTx');
      _requestPump();
    });
  }

  void _requestPump() {
    if (_pumpScheduled) return;
    _pumpScheduled = true;
    scheduleMicrotask(() {
      if (!mounted) return;
      _pumpScheduled = false;
      _pumpTx();
    });
  }

  void _requestStatusSync({required String reason}) {
    if (!_link.isOpen) return;
    if (_pendingCmd != null) return;

    dlogTx(() => 'STATUS (reason=$reason)');
    _pendingCmd = _PendingCmd.status();
    _link.sendLine('STATUS');
    _txAckTimer?.cancel();
    _txAckTimer = Timer(const Duration(milliseconds: 300), () {
      if (!mounted) return;
      dlogState(() => 'STATUS timeout');
      _pendingCmd = null;
      _scheduleRetry();
    });
  }

  void _pumpTx() {
    if (!_link.isOpen || !_deviceReady) return;
    if (_pendingCmd != null) return;

    // Priority 1: FXMASK changes.
    final desiredMask = _desiredFxMask;
    if (desiredMask != null && desiredMask != _lastAppliedFxMask) {
      dlogTx(
        () =>
            'FXMASK send desired=$desiredMask lastApplied=$_lastAppliedFxMask',
      );
      _pendingCmd = _PendingCmd.fxmask(desiredMask);
      _link.sendLine('FXMASK $desiredMask');
      _txAckTimer?.cancel();
      _txAckTimer = Timer(const Duration(milliseconds: 250), () {
        if (!mounted) return;
        setState(() {
          _lastAction = 'FXMASK timeout (no ack)';
        });
        dlogState(() => 'FXMASK timeout desired=$desiredMask');
        _pendingCmd = null;
        // Don't spam resend blindly; sync state then retry only if needed.
        _requestStatusSync(reason: 'fxmask-timeout');
      });
      return;
    }

    // Priority 2: PSET changes, one param at a time.
    for (final entry in _desiredParams.entries) {
      final param = entry.key;
      final value = entry.value;
      final last = _lastAppliedParams[param];
      if (last != value) {
        final attempts = _psetAttempts[param];
        if (attempts != null &&
            attempts.value == value &&
            attempts.count >= 2) {
          continue;
        }

        _psetAttempts[param] = _PsetAttempts.bump(prev: attempts, value: value);

        dlogTx(() => 'PSET send $param=$value lastApplied=${last ?? 'null'}');
        _pendingCmd = _PendingCmd.pset(param, value);
        _link.sendLine('PSET $param $value');
        _txAckTimer?.cancel();
        _txAckTimer = Timer(const Duration(milliseconds: 250), () {
          if (!mounted) return;
          setState(() {
            _lastAction = 'PSET timeout ($param, sent $value)';
          });
          dlogState(() => 'PSET timeout $param=$value');

          _pendingCmd = null;
          // Don't spam resend blindly; sync state then retry only if needed.
          _requestStatusSync(reason: 'pset-timeout');
        });
        return;
      }
    }
  }

  void _setDesiredFxMask(int mask) {
    _desiredFxMask = mask;

    dlogState(
      () =>
          'desired FXMASK=$mask (pending=${_pendingCmd?.type}, lastApplied=$_lastAppliedFxMask)',
    );

    // Effect changes must be instant: don't let them wait behind a knob write.
    if (_pendingCmd != null && _pendingCmd!.type == _PendingCmdType.pset) {
      dlogState(() => 'preempt in-flight PSET for FXMASK');
      _txAckTimer?.cancel();
      _txAckTimer = null;
      _pendingCmd = null;
    }

    setState(() {
      _lastAction = 'Applying effects...';
    });
    _requestPump();
  }

  void _setDesiredParam(String param, int value) {
    dlogState(() => 'desired PSET $param=$value');
    _desiredParams[param] = value;
    // New user value => allow send/retry again.
    _psetAttempts.remove(param);
    _requestPump();
  }

  void _startHealthWatchdog() {
    _healthTimer?.cancel();
    _healthTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      if (!mounted) return;
      if (!_link.isOpen) return;

      // Ping only when not busy with parameter/effect updates.
      if (_pendingCmd == null &&
          (_desiredFxMask == null || _desiredFxMask == _lastAppliedFxMask)) {
        dlogTx(() => 'PING');
        _link.sendLine('PING');
      }

      // Consider the device alive if we received *any* line recently.
      // During FX/PSET bursts we may intentionally not send PINGs.
      final last = _lastRxAt;
      final isFresh =
          last != null &&
          DateTime.now().difference(last) < const Duration(seconds: 3);

      // If we haven't seen PONG/READY recently, mark as lost.
      if (_deviceReady && !isFresh) {
        dlogState(
          () =>
              'watchdog lost (lastRx=${_lastRxAt?.toIso8601String() ?? 'null'})',
        );
        setState(() {
          _deviceReady = false;
          _initialSyncDone = false;
          _lastAction = 'Device lost (no response)';
        });
        _clearPendingAcks();
      }
    });
  }

  void _stopHealthWatchdog() {
    _healthTimer?.cancel();
    _healthTimer = null;
    _lastRxAt = null;
  }

  List<String> _ports = const [];
  String? _selectedPort;

  final List<int> _baudRates = const [
    9600,
    19200,
    38400,
    57600,
    115200,
    230400,
    460800,
    921600,
  ];
  int _baudRate = 115200;

  bool _dist = false;
  bool _rev = false;
  bool _del = false;

  Presets _presets = Presets.defaults();
  String _lastDeviceLine = '';
  String _lastAction = '';


  @override
  void initState() {
    super.initState();
    _refreshPorts();
    PresetStore.load().then((p) {
      if (!mounted) return;
      setState(() => _presets = p);
    });
  }

  @override
  void dispose() {
    _paramDebounce.dispose();
    _stopHealthWatchdog();
    _clearPendingAcks();
    _link.close();
    super.dispose();
  }

  void _refreshPorts() {
    final ports = SerialPort.availablePorts;
    setState(() {
      _ports = ports;
      _selectedPort ??= ports.isNotEmpty ? ports.first : null;
    });
  }

  int _fxMask() {
    var mask = 0;
    if (_dist) mask |= 1 << 0;
    if (_rev) mask |= 1 << 1;
    if (_del) mask |= 1 << 2;
    return mask;
  }

  Future<void> _connectOrDisconnect() async {
    if (_link.isOpen) {
      dlogState(() => 'disconnect requested');
      _link.close();
      _stopHealthWatchdog();
      _clearPendingAcks();
      setState(() {
        _lastAction = 'Disconnected';
        _lastDeviceLine = '';
        _deviceReady = false;
        _initialSyncDone = false;
      });
      return;
    }

    final port = _selectedPort;
    if (port == null || port.isEmpty) return;

    dlogState(() => 'connect requested port=$port baud=$_baudRate');

    await _link.open(
      portName: port,
      baudRate: _baudRate,
      onLine: (line) {
        if (!mounted) return;
        setState(() {
          _lastRxAt = DateTime.now();
          _lastDeviceLine = line;

          dlogRx(() => line);

          // Basic device handshake: the serial port can be open even if the DSP
          // is not ready to accept commands yet.
          if (line == 'READY' || line == 'PONG' || line.startsWith('OK PING')) {
            _deviceReady = true;
            _lastAction = 'Device ready';

            dlogState(() => 'device ready (line=$line)');

            if (!_initialSyncDone) {
              _initialSyncDone = true;
              _setDesiredFxMask(_fxMask());
              _pushAllParams();
              _requestPump();
              _link.sendLine('STATUS');
            }
          }

          if (line.startsWith('STATUS ')) {
            // Example:
            // STATUS FXMASK=<n> dist_drive_q8=<n> delay_mix_q15=<n> ...
            final parts = line.split(RegExp(r'\s+'));
            for (final p in parts.skip(1)) {
              final eq = p.indexOf('=');
              if (eq <= 0) continue;
              final key = p.substring(0, eq);
              final val = int.tryParse(p.substring(eq + 1));
              if (val == null) continue;

              if (key == 'FXMASK') {
                _lastAppliedFxMask = val;
              } else {
                _lastAppliedParams[key] = val;
              }
            }

            final pending = _pendingCmd;
            if (pending != null && pending.type == _PendingCmdType.status) {
              _txAckTimer?.cancel();
              _txAckTimer = null;
              _pendingCmd = null;
              dlogState(
                () =>
                    'STATUS sync applied fx=$_lastAppliedFxMask dist=${_lastAppliedParams['dist_drive_q8']}',
              );
              _requestPump();
            }
          }

          // "See if effect is changed or not": confirm FXMASK ack.
          if (line.startsWith('OK FXMASK')) {
            final parts = line.split(RegExp(r'\s+'));
            if (parts.length >= 3) {
              final n = int.tryParse(parts[2]);
              final pending = _pendingCmd;
              if (pending != null && pending.type == _PendingCmdType.fxmask) {
                _txAckTimer?.cancel();
                _txAckTimer = null;
                _pendingCmd = null;
              }
              if (n != null) {
                _lastAppliedFxMask = n;
                _lastAction = 'Effect applied (FXMASK=$n)';
                dlogState(() => 'ack FXMASK=$n');
              }
              _requestPump();
            }
          }

          if (line.startsWith('ERR FXMASK')) {
            _txAckTimer?.cancel();
            _txAckTimer = null;
            _pendingCmd = null;
            _lastAction = 'FXMASK rejected by device';
            dlogState(() => 'ERR FXMASK');
            _requestStatusSync(reason: 'fxmask-err');
          }

          if (line.startsWith('OK PSET')) {
            final parts = line.split(RegExp(r'\s+'));
            if (parts.length >= 4) {
              final pname = parts[2];
              final v = int.tryParse(parts[3]);
              final pending = _pendingCmd;
              if (pending != null && pending.type == _PendingCmdType.pset) {
                _txAckTimer?.cancel();
                _txAckTimer = null;
                _pendingCmd = null;
              }
              if (v != null) {
                _lastAppliedParams[pname] = v;
                _psetAttempts.remove(pname);
                _lastAction = 'Param applied ($pname=$v)';
                dlogState(() => 'ack PSET $pname=$v');
              } else {
                _lastAction = 'Param applied ($pname=???)';
                dlogState(() => 'ack PSET $pname=???');
              }
              _requestPump();
            }
          }

          if (line.startsWith('ERR PSET')) {
            // Firmware doesn't tell which param failed.
            _txAckTimer?.cancel();
            _txAckTimer = null;
            _pendingCmd = null;
            _lastAction = 'PSET rejected by device';
            dlogState(() => 'ERR PSET ($line)');
            _requestStatusSync(reason: 'pset-err');
          }

          if (line.startsWith('ERR UNKNOWN')) {
            // Treat as immediate failure of whatever was in-flight; this is
            // usually command corruption / dropped bytes.
            final hadPending = _pendingCmd != null;
            _txAckTimer?.cancel();
            _txAckTimer = null;
            _pendingCmd = null;
            _lastAction = 'Device parse error';
            dlogState(() => 'ERR UNKNOWN ($line) pending=$hadPending');
            _requestStatusSync(reason: 'unknown-err');
          }
        });
      },
    );

    _lastRxAt = DateTime.now();
    _startHealthWatchdog();

    // On connect: force clean and push stored params.
    setState(() {
      _dist = false;
      _rev = false;
      _del = false;
      _deviceReady = false;
      _initialSyncDone = false;
      _lastAction = 'Port open: $port @ $_baudRate (waiting for device...)';
    });

    dlogState(() => 'port opened, starting watchdog');
    _link.sendLine('PING');
  }

  void _applyFxMask(int mask) {
    if (!_link.isOpen || !_deviceReady) {
      setState(() => _lastAction = 'Not ready: connect to device first');
      return;
    }
    _setDesiredFxMask(mask);
  }

  void _pushAllParams() {
    if (!_link.isOpen || !_deviceReady) return;
    _setDesiredParam('dist_drive_q8', _presets.distDriveQ8);
    _setDesiredParam('gain_q15', _presets.gainQ15);
    _setDesiredParam('delay_mix_q15', _presets.delayMixQ15);
    _setDesiredParam('delay_feedback_q15', _presets.delayFeedbackQ15);
    _setDesiredParam('reverb_mix_q15', _presets.reverbMixQ15);
    _setDesiredParam('reverb_feedback_q15', _presets.reverbFeedbackQ15);
    _setDesiredParam('reverb_damp_q15', _presets.reverbDampQ15);
  }

  Future<void> _persistPresets() async {
    try {
      await PresetStore.save(_presets);
    } catch (_) {
      // ignore
    }
  }

  double _q15ToPct(int q15) => (q15.clamp(0, 32768) / 32768.0) * 100.0;
  int _pctToQ15(double pct) => ((pct.clamp(0, 100) / 100.0) * 32768.0).round();

  // Master gain supports up to 200% (2.0x): 32768=100%, 65536=200%.
  double _gainToPct(int gainQ15) => (gainQ15.clamp(0, 65536) / 32768.0) * 100.0;
  int _pctToGain(double pct) => ((pct.clamp(0, 200) / 100.0) * 32768.0).round();

  double _driveToPct(int q8) => (q8.clamp(0, 131072) / 131072.0) * 100.0;
  int _pctToDrive(double pct) =>
      ((pct.clamp(0, 100) / 100.0) * 131072.0).round();

  Widget _buildPedalUi({required bool ready}) {
    // Map the 6 Figma knobs onto the existing firmware params (no firmware changes).
    // TIME            -> delay_mix_q15
    // MIX (RAMP)      -> reverb_mix_q15
    // FEEDBACK        -> delay_feedback_q15
    // OFFSSET         -> dist_drive_q8
    // BALANCE         -> reverb_feedback_q15
    // FILTER          -> reverb_damp_q15

    return PedalSection(
      ready: ready,
      pedalBgAsset: _kPedalBgAsset,
      knobAsset: _kKnobAsset,
      timePct: _q15ToPct(_presets.delayMixQ15),
      mixPct: _q15ToPct(_presets.reverbMixQ15),
      feedbackPct: _q15ToPct(_presets.delayFeedbackQ15),
      offssetPct: _driveToPct(_presets.distDriveQ8),
      balancePct: _q15ToPct(_presets.reverbFeedbackQ15),
      filterPct: _q15ToPct(_presets.reverbDampQ15),
      onTimeChanged: (pct) {
        setState(() => _presets.delayMixQ15 = _pctToQ15(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('delay_mix_q15', _presets.delayMixQ15);
          }
        });
      },
      onMixChanged: (pct) {
        setState(() => _presets.reverbMixQ15 = _pctToQ15(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('reverb_mix_q15', _presets.reverbMixQ15);
          }
        });
      },
      onFeedbackChanged: (pct) {
        setState(() => _presets.delayFeedbackQ15 = _pctToQ15(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('delay_feedback_q15', _presets.delayFeedbackQ15);
          }
        });
      },
      onOffssetChanged: (pct) {
        setState(() => _presets.distDriveQ8 = _pctToDrive(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('dist_drive_q8', _presets.distDriveQ8);
          }
        });
      },
      onBalanceChanged: (pct) {
        setState(() => _presets.reverbFeedbackQ15 = _pctToQ15(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('reverb_feedback_q15', _presets.reverbFeedbackQ15);
          }
        });
      },
      onFilterChanged: (pct) {
        setState(() => _presets.reverbDampQ15 = _pctToQ15(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('reverb_damp_q15', _presets.reverbDampQ15);
          }
        });
      },
      volumePct: _gainToPct(_presets.gainQ15),
      volumeMaxPct: 200,
      onVolumeChanged: (pct) {
        setState(() => _presets.gainQ15 = _pctToGain(pct));
        _paramDebounce.run(() {
          _persistPresets();
          if (ready) {
            _setDesiredParam('gain_q15', _presets.gainQ15);
          }
        });
      },
      distortion: _dist,
      reverb: _rev,
      delay: _del,
      onDistortionChanged: (v) {
        setState(() => _dist = v);
        _applyFxMask(_fxMask());
      },
      onReverbChanged: (v) {
        setState(() => _rev = v);
        _applyFxMask(_fxMask());
      },
      onDelayChanged: (v) {
        setState(() => _del = v);
        _applyFxMask(_fxMask());
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final connected = _link.isOpen;
    final ready = connected && _deviceReady;

    return Scaffold(
      appBar: AppBar(
        toolbarHeight: 48,
        actions: [
          IconButton(
            tooltip: 'Connection',
            icon: const Icon(Icons.settings_input_antenna),
            onPressed: () {
              showModalBottomSheet(
                context: context,
                isScrollControlled: true,
                backgroundColor: Theme.of(context).scaffoldBackgroundColor,
                shape: const RoundedRectangleBorder(
                  borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
                ),
                builder: (_) => StatefulBuilder(
                  builder: (context, setSheetState) {
                    return DraggableScrollableSheet(
                      initialChildSize: 0.6,
                      minChildSize: 0.4,
                      maxChildSize: 0.9,
                      expand: false,
                      builder: (context, scrollController) {
                        return SingleChildScrollView(
                          controller: scrollController,
                          padding: const EdgeInsets.all(16),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Center(
                                child: Container(
                                  width: 40,
                                  height: 4,
                                  margin: const EdgeInsets.only(bottom: 16),
                                  decoration: BoxDecoration(
                                    color: Colors.grey[600],
                                    borderRadius: BorderRadius.circular(2),
                                  ),
                                ),
                              ),
                              Row(
                                mainAxisAlignment:
                                    MainAxisAlignment.spaceBetween,
                                children: [
                                  const Text(
                                    'Connection',
                                    style: TextStyle(
                                      fontSize: 20,
                                      fontWeight: FontWeight.bold,
                                    ),
                                  ),
                                  IconButton(
                                    tooltip: 'Refresh ports',
                                    onPressed: connected
                                        ? null
                                        : () {
                                            _refreshPorts();
                                            setSheetState(() {});
                                          },
                                    icon: const Icon(Icons.refresh),
                                  ),
                                ],
                              ),
                              const SizedBox(height: 8),
                              ConnectionSection(
                                ports: _ports,
                                selectedPort: _selectedPort,
                                onPortChanged: (v) {
                                  setState(() => _selectedPort = v);
                                  setSheetState(() {});
                                },
                                baudRates: _baudRates,
                                baudRate: _baudRate,
                                onBaudChanged: (v) {
                                  setState(() => _baudRate = v ?? 115200);
                                  setSheetState(() {});
                                },
                                connected: connected,
                                ready: ready,
                                onConnectPressed: () async {
                                  try {
                                    await _connectOrDisconnect();
                                    setSheetState(() {});
                                  } catch (e) {
                                    if (!context.mounted) return;
                                    ScaffoldMessenger.of(context).showSnackBar(
                                      SnackBar(content: Text('$e')),
                                    );
                                  }
                                },
                              ),
                              if (_lastAction.isNotEmpty)
                                Padding(
                                  padding: const EdgeInsets.only(top: 8),
                                  child: Text('Action: $_lastAction'),
                                ),
                              if (_lastDeviceLine.isNotEmpty)
                                Text('Device: $_lastDeviceLine'),
                            ],
                          ),
                        );
                      },
                    );
                  },
                ),
              );
            },
          ),
        ],
      ),
      body: LayoutBuilder(
        builder: (context, c) {
          return Padding(
            padding: const EdgeInsets.all(16),
            child: Center(
              child: Container(
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(18),
                  boxShadow: [
                    // Subtle ground shadow (keeps pedal lifted without overwhelming)
                    BoxShadow(
                      // ignore: deprecated_member_use
                      color: Colors.black.withOpacity(0.22),
                      blurRadius: 22,
                      spreadRadius: 2,
                      offset: const Offset(0, 12),
                    ),
                    // Gentle ambient softness
                    BoxShadow(
                      // ignore: deprecated_member_use
                      color: Colors.black.withOpacity(0.12),
                      blurRadius: 42,
                      spreadRadius: 0,
                      offset: const Offset(0, 20),
                    ),
                  ],
                ),
                child: _buildPedalUi(ready: ready),
              ),
            ),
          );
        },
      ),
    );
  }
}

enum _PendingCmdType { fxmask, pset, status }

class _PendingCmd {
  final _PendingCmdType type;
  final int? fxMask;
  final String? param;
  final int? value;

  _PendingCmd._({required this.type, this.fxMask, this.param, this.value});

  factory _PendingCmd.fxmask(int mask) =>
      _PendingCmd._(type: _PendingCmdType.fxmask, fxMask: mask);

  factory _PendingCmd.pset(String param, int value) =>
      _PendingCmd._(type: _PendingCmdType.pset, param: param, value: value);

  factory _PendingCmd.status() => _PendingCmd._(type: _PendingCmdType.status);
}

class _PsetAttempts {
  final int value;
  final int count;

  const _PsetAttempts({required this.value, required this.count});

  static _PsetAttempts bump({
    required _PsetAttempts? prev,
    required int value,
  }) {
    if (prev != null && prev.value == value) {
      return _PsetAttempts(value: value, count: prev.count + 1);
    }
    return _PsetAttempts(value: value, count: 1);
  }
}
