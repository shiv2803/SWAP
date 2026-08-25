/**
 * SWAP Hindrance Control Backend
 * Bridges Web UI <-> ESP32 Nodes via WebSocket
 */

import express from 'express';
import { createServer } from 'http';
import { WebSocketServer } from 'ws';
import cors from 'cors';
import { v4 as uuidv4 } from 'uuid';

const app = express();
const server = createServer(app);
const wss = new WebSocketServer({ server });

app.use(cors());
app.use(express.json());
app.use(express.static('../frontend/dist'));

// ============ STATE ============
const nodes = new Map(); // nodeId -> { ws, info, status }
const clients = new Set(); // WebSocket clients (web UI)
const MODES = {
  0: 'IDLE',
  1: 'WIFI_DEAUTH', 2: 'WIFI_BEACON', 3: 'WIFI_PROBE', 4: 'WIFI_AUTH',
  5: 'BLE_ADV', 6: 'BLE_CONN', 7: 'BLE_PAIR',
  8: 'LORA_COLLISION', 9: 'LORA_PREAMBLE', 10: 'LORA_DUTY',
  11: 'ALL_CHAOS'
};
const TARGET_BITS = { wifi: 1, ble: 2, lora: 4 };

// ============ UTILITIES ============
function broadcastToClients(msg) {
  const data = JSON.stringify(msg);
  for (const client of clients) {
    if (client.readyState === 1) client.send(data);
  }
}

function sendToNode(nodeId, msg) {
  const node = nodes.get(nodeId);
  if (node?.ws?.readyState === 1) {
    node.ws.send(JSON.stringify(msg));
    return true;
  }
  return false;
}

function broadcastToNodes(msg) {
  for (const [id, node] of nodes) {
    if (node.ws?.readyState === 1) {
      node.ws.send(JSON.stringify(msg));
    }
  }
}

function getNodesStatus() {
  const result = {};
  for (const [id, node] of nodes) {
    result[id] = {
      id: node.info.id,
      role: node.info.role,
      connected: node.ws?.readyState === 1,
      lastSeen: node.lastSeen,
      status: node.status
    };
  }
  return result;
}

// ============ HTTP ROUTES ============
app.get('/api/health', (req, res) => {
  res.json({ ok: true, uptime: process.uptime(), nodes: nodes.size, clients: clients.size });
});

app.get('/api/nodes', (req, res) => {
  res.json(getNodesStatus());
});

app.get('/api/nodes/:id', (req, res) => {
  const node = nodes.get(req.params.id);
  if (!node) return res.status(404).json({ error: 'Node not found' });
  res.json({
    id: node.info.id,
    role: node.info.role,
    connected: node.ws?.readyState === 1,
    lastSeen: node.lastSeen,
    status: node.status
  });
});

app.post('/api/nodes/:id/command', (req, res) => {
  const node = nodes.get(req.params.id);
  if (!node) return res.status(404).json({ error: 'Node not found' });
  
  const { command, params } = req.body;
  const sent = sendToNode(req.params.id, { type: 'cmd', command, params });
  
  if (sent) {
    res.json({ ok: true, sent: true });
  } else {
    res.status(503).json({ ok: false, error: 'Node not connected' });
  }
});

app.post('/api/nodes/:id/mode', (req, res) => {
  const node = nodes.get(req.params.id);
  if (!node) return res.status(404).json({ error: 'Node not found' });
  
  const { mode, targets } = req.body;
  const sent = sendToNode(req.params.id, { type: 'mode', mode, targets });
  
  if (sent) res.json({ ok: true });
  else res.status(503).json({ ok: false, error: 'Node not connected' });
});

app.post('/api/nodes/:id/targets', (req, res) => {
  const node = nodes.get(req.params.id);
  if (!node) return res.status(404).json({ error: 'Node not found' });
  
  const { targets } = req.body;
  const sent = sendToNode(req.params.id, { type: 'targets', mask: targets });
  
  if (sent) res.json({ ok: true });
  else res.status(503).json({ ok: false, error: 'Node not connected' });
});

// Broadcast to all nodes
app.post('/api/broadcast/mode', (req, res) => {
  const { mode, targets } = req.body;
  broadcastToNodes({ type: 'mode', mode, targets });
  res.json({ ok: true, sent: nodes.size });
});

app.post('/api/broadcast/targets', (req, res) => {
  const { targets } = req.body;
  broadcastToNodes({ type: 'targets', mask: targets });
  res.json({ ok: true, sent: nodes.size });
});

app.post('/api/broadcast/stop', (req, res) => {
  broadcastToNodes({ type: 'cmd', command: '0' });
  res.json({ ok: true, sent: nodes.size });
});

// ============ WEBSOCKET: WEB UI CLIENTS ============
wss.on('connection', (ws, req) => {
  const clientId = uuidv4();
  ws.clientId = clientId;
  ws.isNode = false;
  clients.add(ws);
  
  console.log(`[UI] Client connected: ${clientId} (${clients.size} total)`);
  
  // Send initial state
  ws.send(JSON.stringify({
    type: 'init',
    nodes: getNodesStatus(),
    modes: MODES,
    targets: TARGET_BITS
  }));
  
  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      handleClientMessage(ws, msg);
    } catch (e) {
      console.error('[UI] Invalid message:', e.message);
    }
  });
  
  ws.on('close', () => {
    clients.delete(ws);
    console.log(`[UI] Client disconnected: ${clientId} (${clients.size} total)`);
  });
  
  ws.on('error', (err) => {
    console.error('[UI] WebSocket error:', err.message);
  });
});

function handleClientMessage(ws, msg) {
  switch (msg.type) {
    case 'ping':
      ws.send(JSON.stringify({ type: 'pong', timestamp: Date.now() }));
      break;
    case 'getStatus':
      ws.send(JSON.stringify({ type: 'status', nodes: getNodesStatus() }));
      break;
    case 'command':
      if (msg.nodeId === 'all') {
        broadcastToNodes({ type: 'cmd', command: msg.command, params: msg.params });
      } else {
        sendToNode(msg.nodeId, { type: 'cmd', command: msg.command, params: msg.params });
      }
      break;
    case 'mode':
      if (msg.nodeId === 'all') {
        broadcastToNodes({ type: 'mode', mode: msg.mode, targets: msg.targets });
      } else {
        sendToNode(msg.nodeId, { type: 'mode', mode: msg.mode, targets: msg.targets });
      }
      break;
    case 'targets':
      if (msg.nodeId === 'all') {
        broadcastToNodes({ type: 'targets', mask: msg.targets });
      } else {
        sendToNode(msg.nodeId, { type: 'targets', mask: msg.targets });
      }
      break;
  }
}

// ============ WEBSOCKET: ESP32 NODES (separate path) ============
// Nodes connect to ws://host:port/node?nodeId=A|B

// We'll handle node connections on a separate upgrade path
server.on('upgrade', (request, socket, head) => {
  const url = new URL(request.url, `http://${request.headers.host}`);
  
  if (url.pathname === '/node') {
    // Handle ESP32 node connection
    wss.handleUpgrade(request, socket, head, (ws) => {
      const nodeId = url.searchParams.get('nodeId') || uuidv4();
      handleNodeConnection(ws, nodeId, request);
    });
  } else {
    // Let the main WSS handle UI connections
    wss.handleUpgrade(request, socket, head, (ws) => {
      wss.emit('connection', ws, request);
    });
  }
});

function handleNodeConnection(ws, nodeId, request) {
  ws.nodeId = nodeId;
  ws.isNode = true;
  
  console.log(`[NODE] ${nodeId} connected from ${request.socket.remoteAddress}`);
  
  // Register or update node
  if (!nodes.has(nodeId)) {
    nodes.set(nodeId, {
      ws,
      info: { id: nodeId, role: nodeId },
      status: {},
      lastSeen: Date.now()
    });
  } else {
    const node = nodes.get(nodeId);
    node.ws = ws;
    node.lastSeen = Date.now();
  }
  
  // Notify UI clients
  broadcastToClients({ type: 'nodeConnected', node: getNodesStatus()[nodeId] });
  
  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      handleNodeMessage(nodeId, msg);
    } catch (e) {
      console.error(`[NODE ${nodeId}] Invalid message:`, e.message);
    }
  });
  
  ws.on('close', () => {
    const node = nodes.get(nodeId);
    if (node) {
      node.ws = null;
      node.lastSeen = Date.now();
    }
    console.log(`[NODE] ${nodeId} disconnected`);
    broadcastToClients({ type: 'nodeDisconnected', nodeId });
  });
  
  ws.on('error', (err) => {
    console.error(`[NODE ${nodeId}] Error:`, err.message);
  });
  
  // Send current config to node
  ws.send(JSON.stringify({
    type: 'config',
    nodeId,
    mode: 0,
    targets: 7
  }));
}

function handleNodeMessage(nodeId, msg) {
  const node = nodes.get(nodeId);
  if (!node) return;
  
  node.lastSeen = Date.now();
  
  switch (msg.type) {
    case 'register':
      node.info = { ...node.info, ...msg.data };
      broadcastToClients({ type: 'nodeInfo', nodeId, info: node.info });
      break;
    case 'status':
      node.status = { ...node.status, ...msg.data };
      broadcastToClients({ type: 'nodeStatus', nodeId, status: node.status });
      break;
    case 'telemetry':
      node.status = { ...node.status, telemetry: msg.data };
      broadcastToClients({ type: 'telemetry', nodeId, data: msg.data });
      break;
    case 'power':
      node.status = { ...node.status, power: msg.data };
      broadcastToClients({ type: 'power', nodeId, data: msg.data });
      break;
    case 'log':
      broadcastToClients({ type: 'log', nodeId, ...msg.data });
      break;
    case 'ack':
      broadcastToClients({ type: 'ack', nodeId, ...msg.data });
      break;
  }
}

// ============ PERIODIC TASKS ============
setInterval(() => {
  // Clean up stale nodes
  const now = Date.now();
  for (const [id, node] of nodes) {
    if (node.ws?.readyState !== 1 && now - node.lastSeen > 30000) {
      console.log(`[NODE] ${id} timed out`);
      nodes.delete(id);
      broadcastToClients({ type: 'nodeRemoved', nodeId: id });
    }
  }
  
  // Request status from all nodes
  broadcastToNodes({ type: 'requestStatus' });
}, 10000);

// ============ START ============
const PORT = process.env.PORT || 3001;
server.listen(PORT, () => {
  console.log(`
╔══════════════════════════════════════════════════════════╗
║  SWAP HINDRANCE CONTROL BACKEND                          ║
║  HTTP:  http://localhost:${PORT}                              ║
║  WS:    ws://localhost:${PORT} (UI)                           ║
║  WS:    ws://localhost:${PORT}/node?nodeId=A|B (ESP32)       ║
╚══════════════════════════════════════════════════════════╝
  `);
});