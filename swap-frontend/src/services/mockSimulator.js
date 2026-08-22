import { LinkQualityPredictor, PROTOCOL_NAMES } from './linkQualityPredictor';

export class TelemetrySimulator {
  constructor(onRecordCallback) {
    this.onRecord = onRecordCallback;
    this.timer = null;
    this.step = 0;
    this.forcedProtocols = { a: null, b: null };
    this.predictor = new LinkQualityPredictor(10, 3);
  }

  start(intervalMs = 400) {
    if (this.timer) return;
    this.timer = setInterval(() => {
      this.step++;
      const now = Date.now();
      const recvTs = now / 1000;

      ['a', 'b'].forEach((node) => {
        const offset = node === 'a' ? 0 : 3.2;
        // Dynamic multi-component RF propagation wave simulation
        const primaryCycle = (Math.sin(this.step * 0.08 + offset) + 1) / 2;
        const noiseFading = (Math.cos(this.step * 0.25) * 4);

        const wifi_rssi = Number((-58 - primaryCycle * 32 + noiseFading).toFixed(1));
        const wifi_loss = Number(Math.max(0, Math.min(1, 0.01 + primaryCycle * 0.38 + (Math.random() * 0.03 - 0.015))).toFixed(3));
        const ble_rssi = Number((-68 - primaryCycle * 22 + noiseFading * 0.8).toFixed(1));
        const lora_rssi = Number((-88 - primaryCycle * 9 + noiseFading * 0.4).toFixed(1));
        const lora_snr = Number((7.5 - primaryCycle * 4.8 + (Math.random() * 1.0 - 0.5)).toFixed(1));
        const lora_loss = Number(Math.max(0, Math.min(1, 0.005 + primaryCycle * 0.07 + (Math.random() * 0.008 - 0.004))).toFixed(3));

        // Active protocol determination
        let active_protocol = 0;
        if (this.forcedProtocols[node] !== null) {
          active_protocol = this.forcedProtocols[node];
        } else if (wifi_rssi > -75 && wifi_loss < 0.10) {
          active_protocol = 0; // WIFI
        } else if (ble_rssi > -85) {
          active_protocol = 1; // BLE
        } else {
          active_protocol = 2; // LORA
        }

        // RTT estimate with non-linear packet retransmission delays
        let rtt_ms = 8;
        if (active_protocol === 0) rtt_ms = Math.round(7 + wifi_loss * 28 + Math.random() * 5);
        else if (active_protocol === 1) rtt_ms = Math.round(25 + wifi_loss * 60 + Math.random() * 10);
        else rtt_ms = Math.round(215 + lora_loss * 380 + Math.random() * 30);

        const record = {
          recv_ts: recvTs,
          node: node,
          ts_ms: now,
          active_protocol: active_protocol,
          wifi_rssi: wifi_rssi,
          wifi_loss: wifi_loss,
          ble_rssi: ble_rssi,
          lora_rssi: lora_rssi,
          lora_snr: lora_snr,
          lora_loss: lora_loss,
          rtt_ms: rtt_ms,
        };

        // Run evaluation through precision predictor
        const decision = this.predictor.evaluate(record);

        if (this.onRecord) {
          this.onRecord({ record, decision });
        }
      });
    }, intervalMs);
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  forceProtocol(node, protocol) {
    this.forcedProtocols[node] = Number(protocol);
    return {
      ok: true,
      node: node,
      protocol: Number(protocol),
      protocol_name: PROTOCOL_NAMES[protocol],
    };
  }

  decide(node) {
    const forced = this.forcedProtocols[node];
    return {
      node: node,
      current_protocol: forced !== null ? forced : 0,
      recommended_protocol: forced !== null ? forced : 0,
      recommended_protocol_name: PROTOCOL_NAMES[forced !== null ? forced : 0],
      confidence: 0.95,
      decision_source: 'rf_model_windowed',
      forced: false,
    };
  }
}
