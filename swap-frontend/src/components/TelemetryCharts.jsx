import React, { useState } from 'react';
import {
  ResponsiveContainer,
  LineChart,
  Line,
  XAxis,
  YAxis,
  Tooltip,
  CartesianGrid,
  Legend,
} from 'recharts';
import { TrendingUp, Sliders, Eye } from 'lucide-react';

export function TelemetryCharts({ historyRecords, selectedNodeFilter }) {
  const [chartMetricTab, setChartMetricTab] = useState('rssi');
  const [isSmoothed, setIsSmoothed] = useState(true);

  // Filter records based on node selection
  const filteredHistory = historyRecords.filter((rec) => {
    if (selectedNodeFilter === 'all') return true;
    return rec.node === selectedNodeFilter;
  });

  const rawSlice = filteredHistory.slice(-40);

  // Calculate moving average over 5 samples if smoothed is active
  const chartData = rawSlice.map((r, idx, arr) => {
    const timeStr = r.recv_ts
      ? new Date(r.recv_ts * 1000).toLocaleTimeString([], { hour12: false, minute: '2-digit', second: '2-digit' })
      : `#${idx + 1}`;

    if (!isSmoothed || idx < 2) {
      return {
        time: timeStr,
        node: r.node?.toUpperCase(),
        wifi_rssi: r.wifi_rssi,
        ble_rssi: r.ble_rssi,
        lora_rssi: r.lora_rssi,
        wifi_loss: Number((r.wifi_loss * 100).toFixed(1)),
        lora_loss: Number((r.lora_loss * 100).toFixed(1)),
        lora_snr: r.lora_snr,
        rtt_ms: r.rtt_ms,
      };
    }

    // 5-sample window mean
    const windowSlice = arr.slice(Math.max(0, idx - 4), idx + 1);
    const avgWifiRssi = windowSlice.reduce((acc, x) => acc + x.wifi_rssi, 0) / windowSlice.length;
    const avgBleRssi = windowSlice.reduce((acc, x) => acc + x.ble_rssi, 0) / windowSlice.length;
    const avgLoraRssi = windowSlice.reduce((acc, x) => acc + x.lora_rssi, 0) / windowSlice.length;
    const avgRtt = windowSlice.reduce((acc, x) => acc + x.rtt_ms, 0) / windowSlice.length;
    const avgLoraSnr = windowSlice.reduce((acc, x) => acc + x.lora_snr, 0) / windowSlice.length;

    return {
      time: timeStr,
      node: r.node?.toUpperCase(),
      wifi_rssi: Number(avgWifiRssi.toFixed(1)),
      ble_rssi: Number(avgBleRssi.toFixed(1)),
      lora_rssi: Number(avgLoraRssi.toFixed(1)),
      lora_snr: Number(avgLoraSnr.toFixed(1)),
      rtt_ms: Math.round(avgRtt),
    };
  });

  return (
    <div className="glass-panel rounded-2xl p-6 border border-slate-800">
      <div className="flex flex-col sm:flex-row sm:items-center justify-between gap-4 mb-6 pb-4 border-b border-slate-800/80">
        <div className="flex items-center gap-3">
          <div className="p-2 rounded-xl bg-purple-500/10 border border-purple-500/30 text-purple-400">
            <TrendingUp className="w-5 h-5" />
          </div>
          <div>
            <h3 className="font-bold text-slate-100 text-base flex items-center gap-2">
              High-Precision Telemetry Analytics
              {isSmoothed && (
                <span className="text-[10px] font-mono px-2 py-0.5 rounded-md bg-cyan-500/10 text-cyan-400 border border-cyan-500/30 font-semibold">
                  Window Smoothed
                </span>
              )}
            </h3>
            <p className="text-xs text-slate-400">Real-time multi-interface signal streams & feature extraction</p>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-3">
          {/* Moving Average Toggle */}
          <button
            onClick={() => setIsSmoothed(!isSmoothed)}
            className={`px-3 py-1.5 rounded-xl text-xs font-semibold border transition-all flex items-center gap-1.5 ${
              isSmoothed
                ? 'bg-cyan-500/10 border-cyan-500/40 text-cyan-400'
                : 'bg-slate-900 border-slate-800 text-slate-400 hover:text-slate-200'
            }`}
            title="Toggle between raw signal noise and 5-sample windowed feature smoothing"
          >
            <Sliders className="w-3.5 h-3.5" />
            {isSmoothed ? '5-Sample Window Mean' : 'Raw Telemetry'}
          </button>

          {/* Metric Selector Tabs */}
          <div className="flex items-center bg-slate-900 p-1 rounded-xl border border-slate-800 text-xs">
            {[
              { id: 'rssi', label: 'Signal RSSI (dBm)' },
              { id: 'latency', label: 'RTT Latency (ms)' },
              { id: 'snr', label: 'LoRa SNR (dB)' },
            ].map((tab) => (
              <button
                key={tab.id}
                onClick={() => setChartMetricTab(tab.id)}
                className={`px-3 py-1.5 rounded-lg font-medium transition-all ${
                  chartMetricTab === tab.id
                    ? 'bg-slate-800 text-cyan-400 font-bold border border-slate-700 shadow-sm'
                    : 'text-slate-400 hover:text-slate-200'
                }`}
              >
                {tab.label}
              </button>
            ))}
          </div>
        </div>
      </div>

      {/* Chart Canvas Container */}
      <div className="h-[280px] w-full">
        {chartData.length === 0 ? (
          <div className="h-full flex items-center justify-center text-slate-500 font-mono text-xs">
            Waiting for live telemetry frames...
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            {chartMetricTab === 'rssi' ? (
              <LineChart data={chartData} margin={{ top: 10, right: 10, left: -20, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                <XAxis dataKey="time" stroke="#64748b" tick={{ fontSize: 10 }} />
                <YAxis domain={[-110, -40]} stroke="#64748b" tick={{ fontSize: 10 }} />
                <Tooltip content={<CustomTooltip unit="dBm" />} />
                <Legend wrapperStyle={{ fontSize: '11px', paddingTop: '10px' }} />
                <Line type="monotone" dataKey="wifi_rssi" name="WiFi RSSI" stroke="#38bdf8" strokeWidth={2} dot={false} isAnimationActive={false} />
                <Line type="monotone" dataKey="ble_rssi" name="BLE RSSI" stroke="#c084fc" strokeWidth={2} dot={false} isAnimationActive={false} />
                <Line type="monotone" dataKey="lora_rssi" name="LoRa RSSI" stroke="#fbbf24" strokeWidth={2} dot={false} isAnimationActive={false} />
              </LineChart>
            ) : chartMetricTab === 'latency' ? (
              <LineChart data={chartData} margin={{ top: 10, right: 10, left: -20, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                <XAxis dataKey="time" stroke="#64748b" tick={{ fontSize: 10 }} />
                <YAxis stroke="#64748b" tick={{ fontSize: 10 }} />
                <Tooltip content={<CustomTooltip unit="ms" />} />
                <Legend wrapperStyle={{ fontSize: '11px', paddingTop: '10px' }} />
                <Line type="monotone" dataKey="rtt_ms" name="RTT Latency (ms)" stroke="#10b981" strokeWidth={2.5} dot={false} isAnimationActive={false} />
              </LineChart>
            ) : (
              <LineChart data={chartData} margin={{ top: 10, right: 10, left: -20, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                <XAxis dataKey="time" stroke="#64748b" tick={{ fontSize: 10 }} />
                <YAxis stroke="#64748b" tick={{ fontSize: 10 }} />
                <Tooltip content={<CustomTooltip unit="dB" />} />
                <Legend wrapperStyle={{ fontSize: '11px', paddingTop: '10px' }} />
                <Line type="monotone" dataKey="lora_snr" name="LoRa SNR (dB)" stroke="#f59e0b" strokeWidth={2.5} dot={false} isAnimationActive={false} />
              </LineChart>
            )}
          </ResponsiveContainer>
        )}
      </div>
    </div>
  );
}

function CustomTooltip({ active, payload, label, unit }) {
  if (active && payload && payload.length) {
    return (
      <div className="bg-slate-900/95 border border-slate-700/80 backdrop-blur-md rounded-xl p-3 shadow-xl text-xs font-mono">
        <div className="text-slate-400 font-semibold mb-1.5 border-b border-slate-800 pb-1">{label}</div>
        {payload.map((entry, index) => (
          <div key={`item-${index}`} className="flex items-center gap-2 py-0.5" style={{ color: entry.color }}>
            <span className="w-2 h-2 rounded-full" style={{ backgroundColor: entry.color }} />
            <span className="font-medium text-slate-300">{entry.name}:</span>
            <span className="font-bold">{entry.value} {unit}</span>
          </div>
        ))}
      </div>
    );
  }
  return null;
}
