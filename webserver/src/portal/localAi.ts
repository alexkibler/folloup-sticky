import type { PortalDom } from './dom';
import type { LocalAiModuleResponse, LocalAiModuleSettings, StatusType } from './types';

export function createLocalAiController(deps: {
  dom: PortalDom;
  fetchJson: (path: string, init?: RequestInit) => Promise<LocalAiModuleResponse>;
  notify: (message: string, type?: StatusType) => void;
  updateUi: () => void;
}) {
  let busy = false;
  const { dom } = deps;

  function applySettings(settings?: LocalAiModuleSettings) {
    if (!settings) return;
    dom.transcriptionUrlInput.value = settings.transcription_url;
    dom.transcriptionModelInput.value = settings.transcription_model;
    dom.ollamaUrlInput.value = settings.ollama_url;
    dom.ollamaModelInput.value = settings.model_name;
    dom.localAiApiKeyInput.value = '';
    dom.localAiApiKeyInput.setAttribute('placeholder', settings.has_key
      ? 'Token saved; leave blank to keep it' : 'Enter Diction pairing token');
  }

  async function checkServers() {
    busy = true;
    deps.updateUi();
    deps.notify('Checking local servers…', 'info');
    try {
      await deps.fetchJson('/api/settings/ai/check', { method: 'POST' });
      for (let attempt = 0; attempt < 100; attempt++) {
        await new Promise(resolve => setTimeout(resolve, 500));
        const data = await deps.fetchJson('/api/runtime/ai');
        if (!data.runtime?.request_in_flight) {
          deps.notify(data.runtime?.last_error_message || data.runtime?.last_status_message || 'Connect Wi-Fi first.',
            data.runtime?.ready ? 'success' : 'warning');
          return;
        }
      }
      deps.notify('Server check is taking longer than expected.', 'warning');
    } catch (error) {
      deps.notify(error instanceof Error ? error.message : 'Server check failed.', 'error');
    } finally {
      busy = false;
      deps.updateUi();
    }
  }

  async function saveSettings() {
    busy = true;
    deps.updateUi();
    try {
      const settings: Record<string, string> = {
        transcription_url: dom.transcriptionUrlInput.value.trim(),
        transcription_model: dom.transcriptionModelInput.value.trim(),
        ollama_url: dom.ollamaUrlInput.value.trim(),
        model_name: dom.ollamaModelInput.value.trim(),
      };
      const key = dom.localAiApiKeyInput.value.trim();
      if (key) settings.api_key = key;
      const data = await deps.fetchJson('/api/settings/ai', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(settings),
      });
      applySettings(data.settings);
      deps.notify('Local AI settings saved.', 'success');
    } catch (error) {
      deps.notify(error instanceof Error ? error.message : 'Could not save settings.', 'error');
    } finally {
      busy = false;
      deps.updateUi();
    }
  }

  async function clearToken() {
    busy = true;
    deps.updateUi();
    try {
      const data = await deps.fetchJson('/api/settings/ai', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ api_key: '' }),
      });
      applySettings(data.settings);
      deps.notify('Pairing token cleared.', 'success');
    } catch (error) {
      deps.notify(error instanceof Error ? error.message : 'Could not clear token.', 'error');
    } finally {
      busy = false;
      deps.updateUi();
    }
  }

  return { applySettings, saveSettings, checkServers, clearToken, isBusy: () => busy };
}
