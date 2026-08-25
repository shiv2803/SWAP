/* Signal Orbit: a distinct operations dashboard that turns protocol switching into a visible central decision, with event rail, evidence rail, and telemetry band. */
import React, { useEffect, useMemo, useRef, useState } from "react";
import { Activity, ArrowUpRight, BatteryCharging, Bluetooth, ChevronDown, CircleHelp, Clock3, Gauge, Menu, MoreHorizontal, Pause, Play, Radio, RadioTower, RefreshCw, Settings2, ShieldAlert, ShieldCheck, Signal, SlidersHorizontal, Wifi, X, Zap } from "lucide-react";
import { toast } from "sonner";
import { SwapApiClient, SwapLiveClient, resolveBaseUrl, resolveWsUrl } from "../services/api";

/* Nominal per-protocol characteristics. These are datasheet properties of each
   radio standard, not live measurements — the telemetry stream carries RSSI /
   loss / SNR / RTT only. Anything measured comes from `record` below. */
const PROTOCOL_SPECS = [
  { name: "WiFi", label: "Wi‑Fi", id: 0, color: "#f4b860", throughput: "18 Mbps", power: 180, icon: Wifi, short: "Fast nearby" },
  { name: "BLE", label: "BLE", id: 1, color: "#7fe7c4", throughput: "0.8 Mbps", power: 15, icon: Bluetooth, short: "Low power" },
  { name: "LoRa", label: "LoRa", id: 2, color: "#8fb6ff", throughput: "5.5 Kbps", power: 45, icon: RadioTower, short: "Long range" },
];
const PROTOCOL_BY_ID = { 0: "WiFi", 1: "BLE", 2: "LoRa" };

function Bar({ value, color, height = 5 }: { value: number; color: string; height?: number }) { return <div className="h-[var(--bar-height)] overflow-hidden rounded-full bg-white/8" style={{ "--bar-height": `${height}px` } as React.CSSProperties}><div className="h-full rounded-full transition-all duration-500" style={{ width: `${value}%`, background: color }} /></div>; }
function Pill({ children, tone = "mint" }: { children: React.ReactNode; tone?: "mint" | "amber" | "blue" | "muted" }) { const cls = { mint: "border-mint/30 bg-mint/10 text-mint", amber: "border-amber/35 bg-amber/10 text-amber", blue: "border-blue/30 bg-blue/10 text-blue", muted: "border-line bg-panel-2 text-muted" }; return <span className={`inline-flex items-center gap-1.5 rounded-md border px-2 py-1 font-mono text-[9px] uppercase tracking-[.08em] ${cls[tone]}`}>{children}</span>; }
function SectionLabel({ children }: { children: React.ReactNode }) { return <div className="mb-3 flex items-center gap-2 font-mono text-[10px] uppercase tracking-[.14em] text-muted"><span className="h-px w-4 bg-amber" /> {children}</div>; }

/* Resolved at runtime (query param > localStorage > .env > page host), so the
   backend address is no longer baked into the bundle. Point the dashboard at a
   different node without rebuilding: ?backend=192.168.0.102:8000 */
const BASE_URL = resolveBaseUrl();
const WS_URL = resolveWsUrl(BASE_URL);


export default function Home() {
  const [dark, setDark] = useState(() => localStorage.getItem("swap-theme") !== "light");
  const [auto, setAuto] = useState(true);
  const [menu, setMenu] = useState(false);
  const [settings, setSettings] = useState(false);
  const [threshold, setThreshold] = useState(80);
  const [time, setTime] = useState(new Date());

  // Live telemetry from the SWAP backend (swap_backend/app.py).
  const [record, setRecord] = useState<any>(null);
  const [decision, setDecision] = useState<any>(null);
  const [history, setHistory] = useState<any[]>([]);
  const [events, setEvents] = useState<any[]>([]);
  const [status, setStatus] = useState("disconnected");
  const [lastPacketAt, setLastPacketAt] = useState<number | null>(null);
  const [provenance, setProvenance] = useState<string | null>(null);
  const [transport, setTransport] = useState("websocket");
  // Control-logic snapshot from the backend state machine (swap_backend/control.py):
  // which spec state the system is in, whether the switching gate is open, and the
  // outcome of every commanded transition.
  const [control, setControl] = useState<any>(null);

  const api = useRef(new SwapApiClient(BASE_URL));

  useEffect(() => localStorage.setItem("swap-theme", dark ? "dark" : "light"), [dark]);
  useEffect(() => { const t = window.setInterval(() => setTime(new Date()), 1000); return () => window.clearInterval(t); }, []);

  useEffect(() => {
    api.current.getModelInfo().then((i) => setProvenance(i.provenance || null)).catch(() => setProvenance(null));
  }, []);

  useEffect(() => {
    const ws = new SwapLiveClient(WS_URL, (payload: any) => {
      if (payload?.control) setControl(payload.control);
      if (typeof payload?.auto_switch === "boolean") setAuto(payload.auto_switch);
      const rec = payload?.record;
      // A control_event frame (a switch result) carries no telemetry record.
      if (!rec) return;
      setRecord(rec);
      setLastPacketAt(Date.now());
      if (payload.decision) setDecision(payload.decision);
      setHistory((prev) => [...prev, rec].slice(-60));
      if (payload.decision && payload.decision.protocol !== rec.active_protocol) {
        const from = PROTOCOL_BY_ID[rec.active_protocol as 0 | 1 | 2];
        const to = PROTOCOL_BY_ID[payload.decision.protocol as 0 | 1 | 2];
        const reason = payload.decision.dwell_guarded
          ? "held back by the minimum time between switches"
          : payload.decision.hysteresis_guarded
          ? "next-best route not yet clearly better"
          : `${Math.round(payload.decision.confidence * 100)}% confidence from the ${payload.decision.source === "model" ? "trained model" : "rule-based fallback"}`;
        setEvents((prev) => {
          if (prev[0]?.from === from && prev[0]?.to === to) return prev;
          return [{ time: new Date().toLocaleTimeString([], { hour12: false }), from, to, reason, color: PROTOCOL_SPECS[payload.decision.protocol].color }, ...prev].slice(0, 12);
        });
      }
    }, setStatus, { baseUrl: BASE_URL, onTransportChange: setTransport });
    ws.connect();
    return () => ws.disconnect();
  }, []);

  const activeId = record?.active_protocol ?? 0;
  const current = PROTOCOL_SPECS[activeId] ?? PROTOCOL_SPECS[0];
  const online = status === "connected";

  // Measured signal per protocol, straight off the telemetry record.
  const rssiFor = (id: number) => (id === 0 ? record?.wifi_rssi : id === 1 ? record?.ble_rssi : record?.lora_rssi);
  // Packet loss is only measured for WiFi and LoRa — the schema carries no BLE
  // loss counter, so BLE stays null rather than showing an invented number.
  const lossFor = (id: number) => (id === 0 ? record?.wifi_loss : id === 2 ? record?.lora_loss : null);

  const wave = useMemo(() => {
    const slice = history.slice(-12);
    while (slice.length < 12) slice.unshift(slice[0] ?? null);
    return slice;
  }, [history]);

  const secondsAgo = lastPacketAt ? ((Date.now() - lastPacketAt) / 1000).toFixed(1) : null;
  const confidencePct = decision?.confidence != null ? Math.round(decision.confidence * 100) : null;

  // The adaptive layer lives in the backend now (§11 of the control spec), so
  // the toggle has to reach it -- a local-only flag would leave the UI claiming
  // manual control while the UNO Q kept switching on its own.
  const setAutoSwitch = async (enabled: boolean) => {
    setAuto(enabled);
    try {
      await api.current.setAutoSwitch(enabled);
    } catch (e: any) {
      setAuto(!enabled);
      toast(`Couldn't change auto-switch: ${e.message}`);
    }
  };

  const choose = async (name: string) => {
    const spec = PROTOCOL_SPECS.find((p) => p.name === name);
    if (!spec) return;
    await setAutoSwitch(false);
    try {
      const result = await api.current.forceProtocol("a", spec.id);
      toast(
        result.deferred
          ? `${spec.label} queued — ${result.reason}`
          : `Node A switching to ${spec.label} — 5-second trial running.`,
      );
    } catch (e: any) {
      toast(`Couldn't switch: ${e.message}`);
    }
  };

  // The control machine is the authority on what the system is doing; the model
  // only says what it would prefer. Narrate the machine first when it is doing
  // something other than sitting in normal adaptive operation.
  const controlState = control?.state ?? null;
  const controlLabel = !online
    ? "no link"
    : controlState === "boot"
    ? "waiting for nodes"
    : controlState === "wait_link"
    ? "waiting for link"
    : controlState === "switching"
    ? `trialing ${control?.requested_protocol_name?.toLowerCase() ?? "protocol"}`
    : controlState === "recovering"
    ? "recovering link"
    : controlState === "ready_default"
    ? "starting wi-fi"
    : "system nominal";
  const controlTone: "mint" | "amber" | "blue" | "muted" =
    !online || controlState === "recovering" ? "amber"
    : controlState === "switching" ? "blue"
    : controlState === "stable" ? "mint"
    : "muted";

  const controlReason = !control
    ? null
    : control.state === "boot" || control.state === "wait_link"
    ? `${control.state_reason}. LoRa is the boot link; nothing switches until both nodes are up and the link reports connected.`
    : control.state === "switching"
    ? `Trialing ${control.requested_protocol_name} for ${control.transition_timeout_s}s. If it doesn't carry traffic in time both nodes roll back to ${control.previous_protocol_name}.`
    : control.state === "recovering"
    ? `${control.current_protocol_name} dropped. The nodes get 5 seconds to bring it back before rolling back to ${control.previous_protocol_name}.`
    : control.pending_command
    ? `Holding a switch to ${control.pending_command.protocol_name} — ${control.switch_blocked_reason ?? "waiting for the guard to clear"}.`
    : !control.switch_allowed
    ? `Switching is blocked: ${control.switch_blocked_reason}.`
    : null;

  // Real transitions the nodes actually executed, with the trial outcome. Falls
  // back to the model-recommendation narration on firmware too old to report
  // switch results.
  const trail = useMemo(() => {
    const transitions = control?.recent_transitions ?? [];
    if (!transitions.length) return events;
    const outcomeColor: Record<string, string> = { success: "var(--mint)", failed: "var(--danger)", timeout: "var(--danger)", rejected: "var(--amber)" };
    return transitions.map((t: any) => ({
      time: new Date(t.at * 1000).toLocaleTimeString([], { hour12: false }),
      from: t.attempted_name,
      to: t.resulting_name,
      color: outcomeColor[t.outcome] ?? "var(--muted)",
      reason:
        t.outcome === "success"
          ? `Trial passed — ${t.resulting_name} committed (${t.source}).`
          : t.outcome === "failed"
          ? `${t.attempted_name} didn't connect within the 5s trial — rolled back to ${t.resulting_name}.`
          : t.outcome === "timeout"
          ? `No result from the nodes in time — assumed failed, back on ${t.resulting_name}.`
          : `Nodes refused the switch${t.reason ? ` (${t.reason})` : ""} — stayed on ${t.resulting_name}.`,
    }));
  }, [control, events]);

  const reasonLine = controlReason ? controlReason : !record
    ? "Waiting for the first telemetry frame from the backend."
    : !decision
    ? `${current.label} is carrying the link. The model needs a few more samples before it can weigh in.`
    : decision.dwell_guarded
    ? `Holding ${current.label}. A switch is due, but SWAP waits out a minimum dwell time so the link doesn't flap.`
    : decision.hysteresis_guarded
    ? `Holding ${current.label}. The next-best route isn't clearly better yet, so switching would cost more than it gains.`
    : decision.protocol !== activeId
    ? `SWAP wants to move to ${PROTOCOL_BY_ID[decision.protocol as 0 | 1 | 2]} — it scores better than ${current.label} on the current readings.`
    : `${current.label} is the best route right now across reach, loss, and round-trip time.`;

  return <div className={`orbit-app ${dark ? "is-dark" : "is-light"}`}>
    <div className="flex min-h-screen">
    <aside className={`orbit-nav ${menu ? "open" : ""}`}><div className="brand"><div className="brand-mark"><span /><span /><span /></div><div><strong>SWAP</strong><small>adaptive link layer</small></div></div><div className="nav-caption">Control room</div><nav><a className="selected" href="#orbit"><Activity size={15} /> Command view</a><a href="#compare"><Signal size={15} /> Protocols <b>03</b></a><a href="#security"><ShieldCheck size={15} /> Link integrity</a><a href="#perception"><Radio size={15} /> Perception</a><a href="#events"><Clock3 size={15} /> Switch log <b className="badge">{String(events.length).padStart(2, "0")}</b></a></nav><div className="nav-footer"><div className="mini-readout"><span>NODE PAIR</span><strong>A <i>↔</i> B</strong><small>{online ? "Both nodes responding" : "Waiting for telemetry"}</small></div><button onClick={() => setSettings(true)}><Settings2 size={15} /> Display settings</button></div></aside>
    {menu && <button className="scrim" onClick={() => setMenu(false)} aria-label="Close navigation" />}
    <main className="orbit-main"><header className="orbit-header"><button className="menu-button" onClick={() => setMenu(true)} aria-label="Open navigation"><Menu size={18} /></button><div className="stream"><span className="pulse" /> {online ? "LIVE STREAM" : "AWAITING STREAM"} <em>·</em> {time.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })}</div><div className="header-actions"><Pill tone={controlTone}><span className="pulse small" /> {controlLabel}</Pill><button className="icon-button" onClick={() => setSettings(true)} aria-label="Open display settings"><SlidersHorizontal size={15} /></button><div className="avatar">AM</div></div></header>
      <div className="orbit-wrap">
        <div className="page-top"><div><div className="eyebrow">SWAP / command view / {online ? "live" : "offline"}</div><h1>Signal orbit</h1><p>See the connection, the alternatives, and the reason behind every switch.</p></div><div className="top-actions"><span className="last-packet">last packet <b>{secondsAgo ? `${secondsAgo}s ago` : "—"}</b></span><button className="outline-button" onClick={() => api.current.getStatus().then(() => toast("Backend reachable — telemetry is current.")).catch((e) => toast(`Backend unreachable: ${e.message}`))}><RefreshCw size={13} /> Refresh</button></div></div>
        <section id="orbit" className="hero-orbit"><div className="orbit-visual"><div className="radar-grid" /><div className="sweep" /><div className="orbit-ring ring-one" /><div className="orbit-ring ring-two" /><div className="orbit-node node-wifi" style={{ color: PROTOCOL_SPECS[0].color }}><Wifi size={14} /><span>WiFi</span></div><div className="orbit-node node-ble" style={{ color: PROTOCOL_SPECS[1].color }}><Bluetooth size={14} /><span>BLE</span></div><div className="orbit-node node-lora" style={{ color: PROTOCOL_SPECS[2].color }}><RadioTower size={14} /><span>LoRa</span></div><div className="orbit-core"><span className="core-glow" /><Radio size={20} /><small>ACTIVE LINK</small><strong>{record ? current.label : "—"}</strong><em>{auto ? "auto selected" : "manual"}</em></div><svg className="route-line" viewBox="0 0 500 300" preserveAspectRatio="none"><path d="M250 150 C335 90 386 116 405 68" /><path d="M250 150 C170 114 118 150 90 102" /><path d="M250 150 C317 202 360 215 402 230" /></svg></div><div className="hero-copy"><div className="decision-tag"><span className="pulse" /> route decision / {events[0]?.time ?? "—"}</div><h2>{record ? <>{current.short.split(" ")[0]} wins<br /><span>this round.</span></> : <>Waiting for<br /><span>first frame.</span></>}</h2><p>{reasonLine}</p><div className="decision-score"><div><span>Decision confidence</span><strong>{confidencePct != null ? `${confidencePct}%` : "—"}</strong></div><Bar value={confidencePct ?? 0} color="var(--mint)" height={4} /></div><div className="hero-foot"><div><span>decision source</span><strong>{decision ? (decision.source === "model" ? "model" : "rule-based") : "—"}</strong></div><div><span>mode</span><Pill tone="blue">{auto ? "auto-switch" : "manual"}</Pill></div></div></div></section>

        <section className="status-row"><StatusItem icon={Signal} label="RSSI" value={record ? `${Math.round(rssiFor(activeId))} dBm` : "—"} note="active link signal" trend={record ? "live" : "—"} /><StatusItem icon={Zap} label="Throughput" value={current.throughput} note="nominal for this radio" trend="spec" /><StatusItem icon={Gauge} label="Latency" value={record ? `${record.rtt_ms} ms` : "—"} note="round trip" trend={record ? "live" : "—"} /><StatusItem icon={ShieldCheck} label="Link quality" value={decision?.lqi != null ? `${decision.lqi}%` : "—"} note="model link-quality index" trend={decision ? "live" : "—"} /></section>

        <div className="content-grid"><section id="compare" className="panel compare-panel"><SectionLabel>Compare the routes</SectionLabel><div className="section-head"><div><h3>Protocol radar</h3><p>What SWAP could use next.</p></div><button className="text-button">live <ChevronDown size={13} /></button></div><div className="protocol-list">{PROTOCOL_SPECS.map((p) => { const Icon = p.icon; const isActive = p.id === activeId; const loss = lossFor(p.id); const lossPct = loss != null ? Math.round(loss * 100) : null; const rssi = rssiFor(p.id); return <button key={p.name} className={`protocol-row ${isActive ? "active" : ""}`} onClick={() => choose(p.name)}><div className="protocol-name"><span className="protocol-icon" style={{ color: p.color, background: `${p.color}18` }}><Icon size={15} /></span><div><strong>{p.label}</strong><small>{p.short}</small></div></div><div className="protocol-state">{isActive ? <Pill tone="blue">active</Pill> : <Pill tone="muted">standby</Pill>}</div><div className="protocol-stat"><span>Packet loss <b>{lossPct != null ? `${lossPct}%` : "not measured"}</b></span><Bar value={lossPct ?? 0} color={lossPct != null && lossPct > 60 ? "var(--danger)" : p.color} /></div><div className="protocol-stat"><span>Nominal rate <b>{p.throughput}</b></span><span className="sub-stat">RSSI {rssi != null ? `${Math.round(rssi)} dBm` : "—"}</span></div><ChevronDown className="row-arrow" size={14} /></button>; })}</div><div className="matrix-note"><CircleHelp size={14} /> <span><b>How to read this:</b> the best route balances reach, speed, power, and a clean signal. Active does not always mean fastest. Rate is the radio's nominal spec; RSSI and loss are measured live.</span></div></section>

          <section id="security" className="panel security-panel"><SectionLabel>Trust layer</SectionLabel><div className="section-head"><div><h3>Link integrity</h3><p>Derived from measured signal, not threat intel.</p></div><Pill tone="muted">measured</Pill></div><div className="security-list">{PROTOCOL_SPECS.map((p) => { const rssi = rssiFor(p.id); const loss = lossFor(p.id); const known = rssi != null && rssi > -900; const degraded = known && (rssi < -95 || (loss != null && loss > 0.2)); return <div className="security-row" key={p.name}><div className="security-proto"><span className={`threat-dot ${degraded ? "warn" : ""}`} /> <strong>{p.label}</strong></div><div className={`threat-text ${degraded ? "warn" : ""}`}>{!known ? "NO DATA" : degraded ? "DEGRADED" : "HEALTHY"}</div><span className="security-detail">{!known ? "Not reported by this node" : degraded ? "Weak signal or high loss" : "Within usable margin"}</span></div>; })}</div><div className="security-callout"><ShieldAlert size={15} /><div><strong>No intrusion detection yet</strong><span>SWAP measures link health, not jamming or spoofing. Those checks aren't implemented in the firmware.</span></div></div></section>

          <section className="panel chart-panel"><SectionLabel>Radio environment</SectionLabel><div className="section-head"><div><h3>Signal waterfall</h3><p>Each band · last {wave.filter(Boolean).length} frames</p></div><div className="chart-legend"><span><i className="low" /> strong</span><span><i className="mid" /> fair</span><span><i className="high" /> weak</span></div></div><div className="waterfall"><WaterfallRow label="WiFi" color={PROTOCOL_SPECS[0].color} values={wave.map((r) => r?.wifi_rssi)} /><WaterfallRow label="BLE" color={PROTOCOL_SPECS[1].color} values={wave.map((r) => r?.ble_rssi)} /><WaterfallRow label="LoRa" color={PROTOCOL_SPECS[2].color} values={wave.map((r) => r?.lora_rssi)} /></div><div className="chart-axis"><span>older</span><span /><span /><span>now</span></div></section>

          <section id="events" className="panel event-panel"><SectionLabel>Decision trail</SectionLabel><div className="section-head"><div><h3>Switch log</h3><p>Every change has a reason.</p></div><button className="icon-button" onClick={() => toast("Export is available when persistent telemetry is connected.")}><MoreHorizontal size={15} /></button></div><div className="event-list">{trail.length === 0 ? <p style={{ color: "var(--muted)", fontSize: 11 }}>No switches yet — this fills in as SWAP changes route.</p> : trail.map((e, i) => <div className="event" key={`${e.time}-${i}`}><div className="event-dot" style={{ background: e.color }} /><div className="event-time">{e.time}</div><div className="event-route"><b>{e.from}</b><ArrowUpRight size={12} /><b style={{ color: e.color }}>{e.to}</b></div><div className="event-reason">{e.reason}</div></div>)}</div><div className="field-note"><span>FIELD NOTE</span> A switch is a decision, not a failure.</div></section>

          <section className="panel resource-panel"><SectionLabel>Power + reliability</SectionLabel><div className="section-head"><div><h3>Resource pulse</h3><p>Nominal draw per radio — the nodes don't report battery yet.</p></div><Pill tone={online ? "mint" : "muted"}>Node A ↔ B {online ? "online" : "offline"}</Pill></div><div className="resource-grid"><div className="battery-stack">{PROTOCOL_SPECS.map((p) => <div className="battery-row" key={p.name}><span>{p.label}</span><div><Bar value={Math.round((p.power / 200) * 100)} color={p.color} height={7} /></div><b>{p.power} mW</b></div>)}<div className="battery-note"><BatteryCharging size={14} /> Battery telemetry <b>not reported</b> by the current firmware</div></div><div className="reliability-mini"><MiniMetric label="WiFi loss" value={record?.wifi_loss != null ? `${(record.wifi_loss * 100).toFixed(1)}%` : "—"} /><MiniMetric label="LoRa loss" value={record?.lora_loss != null ? `${(record.lora_loss * 100).toFixed(1)}%` : "—"} /><MiniMetric label="LoRa SNR" value={record?.lora_snr != null ? `${record.lora_snr.toFixed(1)} dB` : "—"} /></div></div></section>

          <section id="perception" className="panel perception-panel"><SectionLabel>Perception layer</SectionLabel><div className="section-head"><div><h3>Radio forensics</h3><p>Everything the firmware reports beyond signal strength. Blank means that radio doesn't measure it.</p></div><Pill tone={record?.crc_ok != null ? "mint" : "muted"}>{record ? current.label : "—"}</Pill></div><div className="perception-grid"><PerceptionCell label="CRC status" value={record?.crc_ok == null ? null : record.crc_ok ? "PASS" : "FAIL"} tone={record?.crc_ok === false ? "bad" : "good"} hint="packet integrity check" /><PerceptionCell label="Time on air" value={record?.time_on_air_ms != null ? `${Math.round(record.time_on_air_ms)} ms` : null} hint="airtime per packet" /><PerceptionCell label="TX power" value={record?.tx_power_dbm != null ? `${record.tx_power_dbm} dBm` : null} hint="transmit level" /><PerceptionCell label="Channel" value={record?.channel != null ? `${record.channel}` : null} hint="wi-fi channel" /><PerceptionCell label="Spread factor" value={record?.spreading_factor != null ? `SF${record.spreading_factor}` : null} hint="lora modulation" /><PerceptionCell label="Bandwidth" value={record?.bandwidth_khz != null ? `${record.bandwidth_khz} kHz` : null} hint="lora channel width" /><PerceptionCell label="Coding rate" value={record?.coding_rate != null ? `4/${record.coding_rate}` : null} hint="lora error coding" /><PerceptionCell label="SNR" value={record?.lora_snr != null && record.lora_snr > -900 ? `${record.lora_snr.toFixed(1)} dB` : null} hint="signal vs noise" /></div><div className="perception-sub"><SectionLabel>Measured by active probing</SectionLabel><div className="perception-grid"><PerceptionCell label="Packet loss" value={record?.packet_loss != null ? `${(record.packet_loss * 100).toFixed(1)}%` : null} tone={record?.packet_loss != null && record.packet_loss > 0.2 ? "bad" : "good"} hint={record?.probes_sent != null ? `${record.probes_acked}/${record.probes_sent} probes acked` : "probe/ack loop"} /><PerceptionCell label="Round trip" value={record?.rtt_ms != null && record.probes_sent != null ? `${Math.round(record.rtt_ms)} ms` : null} hint="probe → ack" /><PerceptionCell label="Jitter" value={record?.jitter_ms != null ? `${record.jitter_ms.toFixed(1)} ms` : null} hint="rtt variation" /><PerceptionCell label="Throughput" value={record?.throughput_bps != null ? (record.throughput_bps > 1000 ? `${(record.throughput_bps / 1000).toFixed(1)} kbps` : `${Math.round(record.throughput_bps)} bps`) : null} hint="confirmed round-trip" /><PerceptionCell label="Max RTT" value={record?.max_rtt_ms != null ? `${Math.round(record.max_rtt_ms)} ms` : null} hint="worst in window" /><PerceptionCell label="Stability" value={record?.stability_db != null ? `±${record.stability_db.toFixed(1)} dB` : null} hint="rssi std deviation" /><PerceptionCell label="Success rate" value={record?.packet_success_rate != null ? `${(record.packet_success_rate * 100).toFixed(1)}%` : null} hint="probes answered" /><PerceptionCell label="Retry rate" value={null} hint="" /></div></div><div className="matrix-note"><CircleHelp size={14} /> <span><b>Retry rate</b> stays blank on purpose: neither the Wi‑Fi/BLE stacks nor the SX1262 expose a retransmission counter at this layer, and the probe loop doesn't retry — so there's nothing honest to count.</span></div></section>

          <section className="panel override-panel"><SectionLabel>Operator controls</SectionLabel><div className="override-layout"><div><h3>Manual override</h3><p>Test a route or let SWAP decide automatically.</p></div><div className="override-actions"><button className={`control-button ${auto ? "on" : ""}`} onClick={() => setAutoSwitch(!auto)}>{auto ? <Play size={13} /> : <Pause size={13} />} Auto-switch {auto ? "on" : "off"}</button>{PROTOCOL_SPECS.map((p) => <button key={p.name} className={`control-button ${!auto && activeId === p.id ? "chosen" : ""}`} onClick={() => choose(p.name)}>{p.label}</button>)}</div><div className="threshold-control"><div><span>Congestion threshold</span><b>{threshold}%</b></div><input type="range" min="40" max="95" value={threshold} onChange={(e) => setThreshold(Number(e.target.value))} /><small>Display preference only — the backend's own switching thresholds are set server-side.</small></div></div></section></div>
        <footer>SWAP / smart wireless adaptive protocol system <span>{provenance ? `model: ${provenance}` : "model provenance unavailable"} · {BASE_URL.replace(/^https?:\/\//, "")} · {transport}</span></footer>
      </div>
    </main></div>
    {settings && <Settings dark={dark} setDark={setDark} onClose={() => setSettings(false)} />}
  </div>;
}
function StatusItem({ icon: Icon, label, value, note, trend }: { icon: any; label: string; value: string; note: string; trend: string }) { return <div className="status-item"><span className="status-icon"><Icon size={14} /></span><div><span>{label}</span><strong>{value}</strong><small>{note}</small></div><em>{trend}</em></div>; }
function WaterfallRow({ label, color, values }: { label: string; color: string; values: (number | undefined)[] }) { return <div className="water-row"><span>{label}</span><div className="water-cells">{values.map((v, i) => { if (v == null || v <= -900) return <i key={i} style={{ background: "var(--line)", opacity: .4 }} />; const strength = Math.max(0, Math.min(100, ((v + 110) / 60) * 100)); return <i key={i} style={{ background: strength < 25 ? "var(--danger)" : strength < 55 ? "var(--amber)" : color, opacity: .35 + strength / 130 }} />; })}</div></div>; }
function MiniMetric({ label, value }: { label: string; value: string }) { return <div><span>{label}</span><strong>{value}</strong></div>; }
/* A null value means the active radio genuinely doesn't report this field —
   shown as a dash, never as a stand-in number. */
function PerceptionCell({ label, value, hint, tone }: { label: string; value: string | null; hint: string; tone?: "good" | "bad" }) { return <div className={`perception-cell ${value == null ? "empty" : ""} ${tone === "bad" ? "bad" : ""}`}><span>{label}</span><strong>{value ?? "—"}</strong><small>{value == null ? "not reported" : hint}</small></div>; }
function Settings({ dark, setDark, onClose }: { dark: boolean; setDark: (v: boolean) => void; onClose: () => void }) { return <div className="settings-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}><div className="settings-drawer"><div className="drawer-head"><div><span className="eyebrow">Display settings</span><h2>Shape your console.</h2></div><button className="icon-button" onClick={onClose}><X size={16} /></button></div><p>Personalize the view without changing the adaptive radio logic.</p><div className="setting-row"><div><strong>{dark ? "Dark electronics" : "Light lab"}</strong><small>{dark ? "Graphite surfaces for low-light monitoring." : "Bright surfaces for daytime work."}</small></div><button className={`switch ${dark ? "on" : ""}`} role="switch" aria-checked={dark} onClick={() => setDark(!dark)}><span /></button></div><div className="setting-row"><div><strong>Live data pulse</strong><small>Show the subtle activity animation in the orbit.</small></div><span className="setting-value">ON</span></div><div className="setting-row"><div><strong>Plain-language notes</strong><small>Keep helpful explanations beside the numbers.</small></div><span className="setting-value">ON</span></div><div className="drawer-note"><CircleHelp size={15} /> The future should feel understandable. SWAP keeps the engineering visible without making you decode it.</div><button className="done-button" onClick={onClose}>Done</button></div></div>; }