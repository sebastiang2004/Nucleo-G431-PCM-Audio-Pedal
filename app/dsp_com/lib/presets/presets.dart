import 'dart:convert';
import 'dart:io';

class Presets {
  Presets({
    required this.distDriveQ8,
    required this.gainQ15,
    required this.delayMixQ15,
    required this.delayFeedbackQ15,
    required this.reverbMixQ15,
    required this.reverbFeedbackQ15,
    required this.reverbDampQ15,
  });

  int distDriveQ8;
  int gainQ15;
  int delayMixQ15;
  int delayFeedbackQ15;
  int reverbMixQ15;
  int reverbFeedbackQ15;
  int reverbDampQ15;

  static Presets defaults() => Presets(
    distDriveQ8: 40960,
    gainQ15: 32768,
    delayMixQ15: 11469,
    delayFeedbackQ15: 16384,
    reverbMixQ15: 9830,
    reverbFeedbackQ15: 22000,
    reverbDampQ15: 8192,
  );

  Map<String, dynamic> toJson() => {
    'master': {'gain_q15': gainQ15},
    'distortion': {'dist_drive_q8': distDriveQ8},
    'delay': {
      'delay_mix_q15': delayMixQ15,
      'delay_feedback_q15': delayFeedbackQ15,
    },
    'reverb': {
      'reverb_mix_q15': reverbMixQ15,
      'reverb_feedback_q15': reverbFeedbackQ15,
      'reverb_damp_q15': reverbDampQ15,
    },
  };

  static Presets fromJson(Map<String, dynamic> json) {
    final p = Presets.defaults();

    final master = json['master'];
    if (master is Map && master['gain_q15'] is num) {
      p.gainQ15 = (master['gain_q15'] as num).round();
    }

    final dist = json['distortion'];
    if (dist is Map && dist['dist_drive_q8'] is num) {
      p.distDriveQ8 = (dist['dist_drive_q8'] as num).round();
    }

    final delay = json['delay'];
    if (delay is Map) {
      if (delay['delay_mix_q15'] is num) {
        p.delayMixQ15 = (delay['delay_mix_q15'] as num).round();
      }
      if (delay['delay_feedback_q15'] is num) {
        p.delayFeedbackQ15 = (delay['delay_feedback_q15'] as num).round();
      }
    }

    final reverb = json['reverb'];
    if (reverb is Map) {
      if (reverb['reverb_mix_q15'] is num) {
        p.reverbMixQ15 = (reverb['reverb_mix_q15'] as num).round();
      }
      if (reverb['reverb_feedback_q15'] is num) {
        p.reverbFeedbackQ15 = (reverb['reverb_feedback_q15'] as num).round();
      }
      if (reverb['reverb_damp_q15'] is num) {
        p.reverbDampQ15 = (reverb['reverb_damp_q15'] as num).round();
      }
    }

    return p;
  }
}

class PresetStore {
  static File _file() {
    return File(
      '${Directory.current.path}${Platform.pathSeparator}effects_presets.json',
    );
  }

  static Future<Presets> load() async {
    try {
      final f = _file();
      if (!await f.exists()) {
        return Presets.defaults();
      }
      final txt = await f.readAsString();
      final obj = jsonDecode(txt);
      if (obj is Map<String, dynamic>) {
        return Presets.fromJson(obj);
      }
      return Presets.defaults();
    } catch (_) {
      return Presets.defaults();
    }
  }

  static Future<void> save(Presets presets) async {
    final f = _file();
    final txt = const JsonEncoder.withIndent('  ').convert(presets.toJson());
    await f.writeAsString(txt);
  }
}
