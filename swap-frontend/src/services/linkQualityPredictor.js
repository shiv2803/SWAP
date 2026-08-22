// High-Precision Link Quality & AI Protocol Handover Predictor
// Ports exact formulas from backend `train_model.py` and `link_quality_model.py`

export const PROTOCOL_WIFI = 0;
export const PROTOCOL_BLE = 1;
export const PROTOCOL_LORA = 2;

export const PROTOCOL_NAMES = {
  [PROTOCOL_WIFI]: 'WIFI',
  [PROTOCOL_BLE]: 'BLE',
  [PROTOCOL_LORA]: 'LORA',
};

const HYSTERESIS_MARGIN = 3.5; // Score margin required to trigger handover

export class SlidingFeatureWindow {
  constructor(windowSize = 10) {
    this.windowSize = windowSize;
    this.history = [];
  }

  push(record) {
    this.history.push(record);
    if (this.history.length > this.windowSize) {
      this.history.shift();
    }
  }

  getMeans() {
    if (this.history.length === 0) return null;
    const len = this.history.length;

    const sums = this.history.reduce(
      (acc, r) => ({
        wifi_rssi: acc.wifi_rssi + r.wifi_rssi,
        wifi_loss: acc.wifi_loss + r.wifi_loss,
        ble_rssi: acc.ble_rssi + r.ble_rssi,
        lora_rssi: acc.lora_rssi + r.lora_rssi,
        lora_snr: acc.lora_snr + r.lora_snr,
        lora_loss: acc.lora_loss + r.lora_loss,
        rtt_ms: acc.rtt_ms + r.rtt_ms,
      }),
      { wifi_rssi: 0, wifi_loss: 0, ble_rssi: 0, lora_rssi: 0, lora_snr: 0, lora_loss: 0, rtt_ms: 0 }
    );

    return {
      wifi_rssi: sums.wifi_rssi / len,
      wifi_loss: sums.wifi_loss / len,
      ble_rssi: sums.ble_rssi / len,
      lora_rssi: sums.lora_rssi / len,
      lora_snr: sums.lora_snr / len,
      lora_loss: sums.lora_loss / len,
      rtt_ms: sums.rtt_ms / len,
      samplesCount: len,
    };
  }
}

export class LinkQualityPredictor {
  constructor(windowSize = 10, minSamples = 3) {
    this.windowSize = windowSize;
    this.minSamples = minSamples;
    this.nodeWindows = {
      a: new SlidingFeatureWindow(windowSize),
      b: new SlidingFeatureWindow(windowSize),
    };
    this.lastDecisionByNode = { a: null, b: null };
  }

  /**
   * Score formula for WiFi protocol matching train_model.py
   */
  static scoreWifi(means) {
    return means.wifi_rssi - means.wifi_loss * 120.0 - means.rtt_ms * 0.1;
  }

  /**
   * Score formula for BLE protocol matching train_model.py
   */
  static scoreBle(means) {
    return means.ble_rssi - means.wifi_loss * 90.0 - means.rtt_ms * 0.15;
  }

  /**
   * Score formula for LoRa protocol matching train_model.py
   */
  static scoreLora(means) {
    return means.lora_rssi + means.lora_snr * 2.0 - means.lora_loss * 160.0 - means.rtt_ms * 0.02;
  }

  /**
   * Evaluates a record and returns decision object with probabilities, scores & hysteresis status
   */
  evaluate(record) {
    const node = (record.node || 'a').toLowerCase();
    if (!this.nodeWindows[node]) {
      this.nodeWindows[node] = new SlidingFeatureWindow(this.windowSize);
    }

    const window = this.nodeWindows[node];
    window.push(record);

    const means = window.getMeans();
    if (!means || means.samplesCount < this.minSamples) {
      return null;
    }

    // Calculate raw mathematical utility scores
    const wifiScore = LinkQualityPredictor.scoreWifi(means);
    const bleScore = LinkQualityPredictor.scoreBle(means);
    const loraScore = LinkQualityPredictor.scoreLora(means);

    const rawScores = {
      [PROTOCOL_WIFI]: wifiScore,
      [PROTOCOL_BLE]: bleScore,
      [PROTOCOL_LORA]: loraScore,
    };

    // Softmax probabilities
    const temp = 10.0;
    const maxScore = Math.max(wifiScore, bleScore, loraScore);
    const expWifi = Math.exp((wifiScore - maxScore) / temp);
    const expBle = Math.exp((bleScore - maxScore) / temp);
    const expLora = Math.exp((loraScore - maxScore) / temp);
    const sumExp = expWifi + expBle + expLora;

    const probabilities = {
      [PROTOCOL_WIFI]: Number((expWifi / sumExp).toFixed(3)),
      [PROTOCOL_BLE]: Number((expBle / sumExp).toFixed(3)),
      [PROTOCOL_LORA]: Number((expLora / sumExp).toFixed(3)),
    };

    // Determine highest scoring protocol candidate
    let bestProtocol = PROTOCOL_WIFI;
    if (bleScore > wifiScore && bleScore > loraScore) bestProtocol = PROTOCOL_BLE;
    else if (loraScore > wifiScore && loraScore > bleScore) bestProtocol = PROTOCOL_LORA;

    // Apply Hysteresis Guard Band to prevent ping-ponging
    const activeProto = record.active_protocol ?? PROTOCOL_WIFI;
    let finalProtocol = bestProtocol;
    let hysteresisApplied = false;

    if (bestProtocol !== activeProto) {
      const activeScore = rawScores[activeProto] ?? -999;
      const bestScore = rawScores[bestProtocol] ?? -999;

      if (bestScore - activeScore < HYSTERESIS_MARGIN) {
        // Suppress handover due to hysteresis safety margin
        finalProtocol = activeProto;
        hysteresisApplied = true;
      }
    }

    // Calculate Link Quality Index (0% to 100%) for active link
    const activeScore = rawScores[finalProtocol];
    // Map score range [-150, -30] to [0%, 100%]
    const lqi = Math.max(0, Math.min(100, Math.round(((activeScore + 150) / 120) * 100)));

    const decision = {
      protocol: finalProtocol,
      protocol_name: PROTOCOL_NAMES[finalProtocol],
      confidence: probabilities[finalProtocol],
      source: means.samplesCount >= 10 ? 'rf_model_windowed' : 'rule_based_smoothing',
      raw_scores: {
        wifi: Number(wifiScore.toFixed(2)),
        ble: Number(bleScore.toFixed(2)),
        lora: Number(loraScore.toFixed(2)),
      },
      probabilities,
      lqi,
      hysteresis_guarded: hysteresisApplied,
      best_candidate: bestProtocol,
    };

    this.lastDecisionByNode[node] = decision;
    return decision;
  }
}
