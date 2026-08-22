// Export collected telemetry records as CSV matching backend `common.py` CSV_FIELDS

export function exportTelemetryCsv(records, filename = 'swap_telemetry_export.csv') {
  if (!records || records.length === 0) {
    alert('No telemetry records available to export.');
    return;
  }

  const headers = [
    'recv_ts',
    'node',
    'ts_ms',
    'active_protocol',
    'wifi_rssi',
    'wifi_loss',
    'ble_rssi',
    'lora_rssi',
    'lora_snr',
    'lora_loss',
    'rtt_ms',
  ];

  const rows = records.map((r) => [
    r.recv_ts,
    r.node,
    r.ts_ms,
    r.active_protocol,
    r.wifi_rssi,
    r.wifi_loss,
    r.ble_rssi,
    r.lora_rssi,
    r.lora_snr,
    r.lora_loss,
    r.rtt_ms,
  ]);

  const csvContent = [headers.join(','), ...rows.map((row) => row.join(','))].join('\n');

  const blob = new Blob([csvContent], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.setAttribute('href', url);
  link.setAttribute('download', filename);
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
}
