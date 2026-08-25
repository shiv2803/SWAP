// Settings persistence context
import { createContext, useContext, useState, useEffect, useCallback, useMemo } from 'react';

const SettingsContext = createContext(null);

const DEFAULT_SETTINGS = {
  theme: 'tactical', // 'tactical' | 'light'
  autoReconnect: true,
  logLevel: 'all', // 'all' | 'ok' | 'warn' | 'error' | 'info'
  maxLogs: 500,
  soundEnabled: false,
  compactMode: false,
  showScanlines: true,
  showNoise: true,
  heartbeatInterval: 5000,
  requestStatusInterval: 10000
};

export function SettingsProvider({ children }) {
  const [settings, setSettings] = useState(() => {
    try {
      const saved = localStorage.getItem('swap-settings');
      return saved ? { ...DEFAULT_SETTINGS, ...JSON.parse(saved) } : DEFAULT_SETTINGS;
    } catch {
      return DEFAULT_SETTINGS;
    }
  });

  const [initialized, setInitialized] = useState(false);

  useEffect(() => {
    setInitialized(true);
  }, []);

  useEffect(() => {
    if (initialized) {
      localStorage.setItem('swap-settings', JSON.stringify(settings));
      applySettings(settings);
    }
  }, [settings, initialized]);

  const applySettings = useCallback((s) => {
    // Apply scanlines
    document.body.style.setProperty('--scanline-opacity', s.showScanlines ? '0.08' : '0');
    // Apply noise
    document.body.style.setProperty('--noise-opacity', s.showNoise ? '0.03' : '0');
    // Apply compact mode
    document.documentElement.classList.toggle('compact-mode', s.compactMode);
  }, []);

  const updateSetting = useCallback((key, value) => {
    setSettings(prev => ({ ...prev, [key]: value }));
  }, []);

  const resetSettings = useCallback(() => {
    setSettings(DEFAULT_SETTINGS);
  }, []);

  const exportSettings = useCallback(() => {
    return JSON.stringify(settings, null, 2);
  }, [settings]);

  const importSettings = useCallback((json) => {
    try {
      const parsed = JSON.parse(json);
      setSettings(prev => ({ ...prev, ...parsed }));
      return true;
    } catch {
      return false;
    }
  }, []);

  const value = useMemo(() => ({
    settings,
    updateSetting,
    resetSettings,
    exportSettings,
    importSettings,
    initialized
  }), [settings, updateSetting, resetSettings, exportSettings, importSettings, initialized]);

  return (
    <SettingsContext.Provider value={value}>
      {children}
    </SettingsContext.Provider>
  );
}

export function useSettings() {
  const context = useContext(SettingsContext);
  if (!context) throw new Error('useSettings must be used within SettingsProvider');
  return context;
}