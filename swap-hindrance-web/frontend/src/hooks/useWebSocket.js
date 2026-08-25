// WebSocket hook for connecting to backend - Production Ready
import { useState, useEffect, useCallback, useRef, useMemo } from 'react';

const WS_URL = `ws://${window.location.hostname}:3001`;
const RECONNECT_BASE_DELAY = 1000;
const RECONNECT_MAX_DELAY = 30000;
const RECONNECT_MAX_ATTEMPTS = 10;

export function useWebSocket() {
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [nodes, setNodes] = useState({});
  const [modes, setModes] = useState({});
  const [targetsConfig, setTargetsConfig] = useState({});
  const [logs, setLogs] = useState([]);
  const [selectedNode, setSelectedNode] = useState('all');
  const [connectionError, setConnectionError] = useState(null);
  const [reconnectAttempt, setReconnectAttempt] = useState(0);

  const wsRef = useRef(null);
  const reconnectTimer = useRef(null);
  const clientId = useRef(null);
  const messageQueue = useRef([]);
  const heartbeatTimer = useRef(null);
  const lastHeartbeat = useRef(0);

  // Persist selected node
  useEffect(() => {
    const saved = localStorage.getItem('swap-selected-node');
    if (saved && saved !== 'all') {
      setSelectedNode(saved);
    }
  }, []);

  useEffect(() => {
    localStorage.setItem('swap-selected-node', selectedNode);
  }, [selectedNode]);

  const clearReconnectTimer = useCallback(() => {
    if (reconnectTimer.current) {
      clearTimeout(reconnectTimer.current);
      reconnectTimer.current = null;
    }
  }, []);

  const clearHeartbeat = useCallback(() => {
    if (heartbeatTimer.current) {
      clearInterval(heartbeatTimer.current);
      heartbeatTimer.current = null;
    }
  }, []);

  const send = useCallback((msg) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(msg));
      return true;
    } else {
      messageQueue.current.push(msg);
      return false;
    }
  }, []);

  const flushQueue = useCallback(() => {
    while (messageQueue.current.length > 0) {
      const msg = messageQueue.current.shift();
      if (wsRef.current?.readyState === WebSocket.OPEN) {
        wsRef.current.send(JSON.stringify(msg));
      } else {
        messageQueue.current.unshift(msg);
        break;
      }
    }
  }, []);

  const startHeartbeat = useCallback(() => {
    clearHeartbeat();
    heartbeatTimer.current = setInterval(() => {
      if (wsRef.current?.readyState === WebSocket.OPEN) {
        const now = Date.now();
        if (now - lastHeartbeat.current > 15000) {
          send({ type: 'ping' });
        }
      }
    }, 5000);
  }, [clearHeartbeat, send]);

  const handleMessage = useCallback((msg) => {
    switch (msg.type) {
      case 'init':
        clientId.current = msg.clientId;
        setNodes(msg.nodes || {});
        setModes(msg.modes || {});
        setTargetsConfig(msg.targets || {});
        setReconnectAttempt(0);
        setConnectionError(null);
        flushQueue();
        break;

      case 'pong':
        lastHeartbeat.current = Date.now();
        break;

      case 'nodeConnected':
      case 'nodeDisconnected':
      case 'nodeRemoved':
      case 'nodeInfo': {
        const updates = msg.nodes || { [msg.nodeId]: msg.node || msg.info };
        setNodes(prev => ({ ...prev, ...updates }));
        break;
      }

      case 'nodeStatus':
      case 'telemetry':
      case 'power': {
        const nodeId = msg.nodeId;
        const data = msg.data || msg.status;
        setNodes(prev => ({
          ...prev,
          [nodeId]: {
            ...prev[nodeId],
            status: { ...prev[nodeId]?.status, ...data }
          }
        }));
        break;
      }

      case 'log':
        setLogs(prev => [
          { ...msg, timestamp: msg.timestamp || Date.now(), nodeId: msg.nodeId },
          ...prev.slice(0, 499)
        ]);
        break;

      case 'ack':
        // Command acknowledged - could show toast
        break;

      case 'status':
        if (msg.nodes) setNodes(msg.nodes);
        break;

      default:
        console.log('[WS] Unknown message type:', msg.type);
    }
  }, [flushQueue]);

  const connect = useCallback(() => {
    if (wsRef.current?.readyState === WebSocket.OPEN || wsRef.current?.readyState === WebSocket.CONNECTING) {
      return;
    }

    setConnecting(true);
    setConnectionError(null);

    try {
      const ws = new WebSocket(WS_URL);
      wsRef.current = ws;

      ws.onopen = () => {
        setConnected(true);
        setConnecting(false);
        setReconnectAttempt(0);
        setConnectionError(null);
        console.log('[WS] Connected to backend');
        startHeartbeat();
      };

      ws.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data);
          handleMessage(msg);
        } catch (e) {
          console.error('[WS] Parse error:', e);
        }
      };

      ws.onclose = (event) => {
        setConnected(false);
        setConnecting(false);
        clearHeartbeat();
        console.log('[WS] Disconnected:', event.code, event.reason);

        // Don't reconnect if clean close
        if (event.code === 1000) return;

        // Exponential backoff with jitter
        const attempt = reconnectAttempt + 1;
        setReconnectAttempt(attempt);

        if (attempt > RECONNECT_MAX_ATTEMPTS) {
          setConnectionError('Max reconnection attempts reached. Please refresh.');
          return;
        }

        const delay = Math.min(
          RECONNECT_BASE_DELAY * Math.pow(2, attempt - 1) + Math.random() * 1000,
          RECONNECT_MAX_DELAY
        );

        reconnectTimer.current = setTimeout(() => {
          connect();
        }, delay);
      };

      ws.onerror = (err) => {
        console.error('[WS] Error:', err);
        setConnectionError('Connection error. Retrying...');
      };
    } catch (err) {
      console.error('[WS] Failed to create connection:', err);
      setConnecting(false);
      setConnectionError(err.message);
    }
  }, [handleMessage, startHeartbeat, clearHeartbeat, reconnectAttempt]);

  const disconnect = useCallback(() => {
    clearReconnectTimer();
    clearHeartbeat();
    if (wsRef.current) {
      wsRef.current.close(1000, 'User disconnect');
      wsRef.current = null;
    }
    setConnected(false);
    setConnecting(false);
  }, [clearReconnectTimer, clearHeartbeat]);

  const sendCommand = useCallback((nodeId, command, params) => {
    send({ type: 'command', nodeId, command, params });
  }, [send]);

  const setMode = useCallback((nodeId, mode, targetMask) => {
    send({ type: 'mode', nodeId, mode, targets: targetMask });
  }, [send]);

  const setTargets = useCallback((nodeId, mask) => {
    send({ type: 'targets', nodeId, targets: mask });
  }, [send]);

  const requestStatus = useCallback(() => {
    send({ type: 'getStatus' });
  }, [send]);

  // Auto-connect on mount
  useEffect(() => {
    connect();
    return () => disconnect();
  }, [connect, disconnect]);

  // Handle visibility change
  useEffect(() => {
    const handleVisibility = () => {
      if (document.hidden) return;
      if (!connected && !connecting) connect();
    };
    document.addEventListener('visibilitychange', handleVisibility);
    return () => document.removeEventListener('visibilitychange', handleVisibility);
  }, [connected, connecting, connect]);

  // Computed values
  const sortedNodes = useMemo(() => {
    return Object.entries(nodes).sort(([a], [b]) => a.localeCompare(b));
  }, [nodes]);

  const nodeList = useMemo(() => sortedNodes.map(([id, node]) => ({ id, ...node })), [sortedNodes]);

  return {
    connected,
    connecting,
    connectionError,
    reconnectAttempt,
    nodes,
    nodeList,
    modes,
    targetsConfig,
    logs,
    selectedNode,
    setSelectedNode,
    sendCommand,
    setMode,
    setTargets,
    requestStatus,
    connect,
    disconnect
  };
}