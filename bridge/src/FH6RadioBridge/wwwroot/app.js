const $ = (id) => document.getElementById(id);
let config = null;
let saving = false;
let mediaSessionsSaving = false;

const api = async (url, options = {}) => {
  const res = await fetch(url, { headers: { 'content-type': 'application/json' }, ...options });
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return await res.json();
};

const fmtDb = (value) => `${Number(value).toFixed(1)} dB`;
const fmtLevel = (value) => Number.isFinite(Number(value)) ? Number(value).toFixed(3) : '—';

function updateEqVisibility(enabled) {
  const eqControls = $('eqControls');
  if (!eqControls) return;
  eqControls.classList.toggle('hidden', !enabled);
}

function updateMediaSessionControls(selection) {
  const auto = $('mediaSessionAuto');
  const select = $('mediaSessionSelect');
  if (!auto || !select || !selection) return;

  const isAuto = String(selection.selectionMode ?? 'Auto').toLowerCase() === 'auto';
  auto.checked = isAuto;
  select.disabled = isAuto;
}

async function loadConfig() {
  config = await api('/api/config');
  $('raceStartAction').value = config.playbackAutomation.raceStartAction;
  $('quickStationSkip').checked = config.playbackAutomation.quickStationSkip;
  $('loudnessNormalization').checked = config.dsp.loudnessNormalization;
  $('equalizerEnabled').checked = config.dsp.equalizerEnabled;
  updateEqVisibility(config.dsp.equalizerEnabled);
  $('volume').value = config.audio.volume;
  $('volumeOut').value = Number(config.audio.volume).toFixed(2);
  for (const slider of document.querySelectorAll('.eqBand')) {
    slider.value = config.dsp[slider.dataset.key] ?? 0;
    slider.nextElementSibling.value = fmtDb(slider.value);
  }
}

async function savePatch(patch) {
  saving = true;
  try {
    config = await api('/api/config', { method: 'PUT', body: JSON.stringify(patch) });
  } finally {
    saving = false;
  }
}

async function refreshMediaSessions() {
  const target = $('mediaSessionTarget');
  const select = $('mediaSessionSelect');
  const auto = $('mediaSessionAuto');
  const count = $('mediaSessionCount');
  if (!target || !select || !auto) return;

  try {
    const response = await api('/api/media-sessions');
    const sessions = response.sessions ?? [];
    const isAuto = String(response.selectionMode ?? 'Auto').toLowerCase() === 'auto';

    select.innerHTML = '';
    let selectedText = isAuto ? 'Auto-select best playing session' : 'Manual selection';

    for (const session of sessions) {
      const option = document.createElement('option');
      option.value = session.sourceAppUserModelId;
      option.textContent = `${session.sourceAppUserModelId} — ${session.playbackStatus} — ${session.artist} - ${session.title}`;
      option.selected = Boolean(session.isSelected);
      select.appendChild(option);

      if (session.isSelected) {
        selectedText = option.textContent;
      }
    }

    if (sessions.length === 0) {
      const option = document.createElement('option');
      option.value = '';
      option.textContent = 'No Windows media sessions found';
      select.appendChild(option);
      selectedText = 'No session';
    }

    target.textContent = selectedText;
    if (count) count.textContent = `${sessions.length} session(s)`;
    if (!mediaSessionsSaving) {
      auto.checked = isAuto;
      select.disabled = isAuto || sessions.length === 0;
    }
  } catch (error) {
    target.textContent = `Unavailable: ${error.message}`;
    if (count) count.textContent = '—';
  }
}

async function refreshState() {
  try {
    const state = await api('/api/state');
    $('connection').textContent = 'online';
    $('connection').className = 'pill online';
    $('title').textContent = state.metadata?.title ?? 'Unknown';
    $('artist').textContent = state.metadata?.artist ?? 'Unknown';
    $('source').textContent = `${state.metadata?.sourceAppUserModelId ?? 'Unknown'} • ${state.metadata?.playbackStatus ?? 'Unknown'}`;
    $('capture').textContent = state.captureDevice ?? '—';
    $('sampleRate').textContent = `${state.sampleRate} Hz / ${state.channels} ch`;
    $('writerAge').textContent = state.writerAgeMs >= 0 ? `${state.writerAgeMs} ms` : '—';
    $('underruns').textContent = state.underrunCount ?? '—';
    $('peak').textContent = `${fmtLevel(state.peakL)} / ${fmtLevel(state.peakR)}`;
    $('rms').textContent = `${fmtLevel(state.rmsL)} / ${fmtLevel(state.rmsR)}`;

    if (!saving && state.playbackAutomation && state.dsp) {
      $('raceStartAction').value = state.playbackAutomation.raceStartAction;
      $('quickStationSkip').checked = state.playbackAutomation.quickStationSkip;
      $('loudnessNormalization').checked = state.dsp.loudnessNormalization;
      $('equalizerEnabled').checked = state.dsp.equalizerEnabled;
      updateEqVisibility(state.dsp.equalizerEnabled);
    }
  } catch {
    $('connection').textContent = 'offline';
    $('connection').className = 'pill offline';
  }
}

function wireEvents() {
  document.querySelectorAll('[data-control]').forEach((button) => {
    button.addEventListener('click', async () => {
      button.disabled = true;
      try { await api(`/api/control/${button.dataset.control}`, { method: 'POST' }); }
      finally { button.disabled = false; }
    });
  });

  $('raceStartAction').addEventListener('change', (e) => savePatch({ playbackAutomation: { raceStartAction: e.target.value } }));
  $('quickStationSkip').addEventListener('change', (e) => savePatch({ playbackAutomation: { quickStationSkip: e.target.checked } }));
  $('loudnessNormalization').addEventListener('change', (e) => savePatch({ dsp: { loudnessNormalization: e.target.checked } }));
  $('equalizerEnabled').addEventListener('change', (e) => {
    updateEqVisibility(e.target.checked);
    savePatch({ dsp: { equalizerEnabled: e.target.checked } });
  });
  $('volume').addEventListener('input', (e) => { $('volumeOut').value = Number(e.target.value).toFixed(2); });
  $('volume').addEventListener('change', (e) => savePatch({ audio: { volume: Number(e.target.value) } }));

  $('refreshMediaSessions')?.addEventListener('click', () => refreshMediaSessions().catch(console.error));
  $('mediaSessionAuto')?.addEventListener('change', async (e) => {
    mediaSessionsSaving = true;
    try {
      if (e.target.checked) {
        await api('/api/media-sessions/auto', { method: 'POST' });
      } else {
        const selected = $('mediaSessionSelect')?.value;
        if (selected) {
          await api('/api/media-sessions/select', {
            method: 'POST',
            body: JSON.stringify({ sourceAppUserModelId: selected })
          });
        }
      }
      await refreshMediaSessions();
    } finally {
      mediaSessionsSaving = false;
    }
  });
  $('mediaSessionSelect')?.addEventListener('change', async (e) => {
    if (!e.target.value) return;
    mediaSessionsSaving = true;
    try {
      await api('/api/media-sessions/select', {
        method: 'POST',
        body: JSON.stringify({ sourceAppUserModelId: e.target.value })
      });
      await refreshMediaSessions();
    } finally {
      mediaSessionsSaving = false;
    }
  });

  for (const slider of document.querySelectorAll('.eqBand')) {
    slider.addEventListener('input', (e) => { e.target.nextElementSibling.value = fmtDb(e.target.value); });
    slider.addEventListener('change', (e) => savePatch({ dsp: { [e.target.dataset.key]: Number(e.target.value) } }));
  }
}

loadConfig().catch(console.error).finally(() => {
  wireEvents();
  refreshState();
  refreshMediaSessions().catch(console.error);
  setInterval(refreshState, 1000);
  setInterval(() => refreshMediaSessions().catch(console.error), 3000);
});
