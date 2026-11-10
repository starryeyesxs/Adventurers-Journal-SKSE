// ============================================================
//  Adventurer's Journal — script.js
//  True two-page book spread architecture (1.6.3)
// ============================================================

// ── Save data supplied by C++ ──
let journalData = {
  characterName: 'The last dragonborn',
  date: 'Date unknown',
  active: [],
  completed: []
};

// ── State ──
let currentTab = 'active';        // 'active' | 'completed' | 'notes' | 'sketches'
let currentView = 'list';          // 'list' | 'detail' | 'notes' | 'notes-detail' | 'sketches' | 'sketch-detail'
let currentQuest = null;
let currentSpreadPage = 0;         // which page of 16-per-spread we're on
let currentSketchIndex = 0;
let currentSketchSpreadPage = 0;
let portraitInspectionOpen = false;
let fieldNotes = [];
let currentNoteIndex = -1;
let saveGeneration = 0;

const QUESTS_PER_SPREAD = 16;
const QUESTS_PER_PAGE   = 8;      // 8 left + 8 right = 16 per spread
const SKETCHES_PER_PAGE = 4;
const SKETCHES_PER_SPREAD = 8;

// ── Sound bridge (DO NOT MODIFY) ──
function triggerSkyrimSound(soundType) {
  if (typeof window.playJournalSound === 'function') {
    window.playJournalSound(soundType);
  }
}

let sketches = [];
let sketchDataLoaded = false;
let nativeSketchMetadata = {};

// ── DOM refs (resolved once) ──
const el = {
  // Left page panels
  spreadQuestList:        document.getElementById('spread-quest-list'),
  spreadDetailLeft:       document.getElementById('spread-quest-detail-left'),
  spreadNotesLeft:        document.getElementById('spread-notes-left'),
  spreadSketchesLeft:     document.getElementById('spread-sketches-left'),
  spreadSketchDetailLeft: document.getElementById('spread-sketch-detail-left'),
  // Right page panels
  spreadQuestListRight:   document.getElementById('spread-quest-list-right'),
  spreadDetailRight:      document.getElementById('spread-quest-detail-right'),
  spreadNotesRight:       document.getElementById('spread-notes-right'),
  spreadSketchesRight:    document.getElementById('spread-sketches-right'),
  spreadSketchDetailRight: document.getElementById('spread-sketch-detail-right'),
  // Quest list
  listTitle:              document.getElementById('list-title'),
  listTitleRight:         document.getElementById('list-title-right'),
  questCount:             document.getElementById('quest-count'),
  untrackAllBtn:          document.getElementById('untrack-all-btn'),
  questCountRight:        document.getElementById('quest-count-right'),
  questListLeft:          document.getElementById('quest-list-left'),
  questListRight:         document.getElementById('quest-list-right'),
  emptyState:             document.getElementById('empty-state'),
  characterName:          document.getElementById('character-name'),
  // Quest detail
  detailEyebrow:          document.getElementById('detail-eyebrow'),
  entryTitle:             document.getElementById('entry-title'),
  entryDate:              document.getElementById('entry-date'),
  entryBody:              document.getElementById('entry-body'),
  trackBtn:               document.getElementById('track-btn'),
  trackSeal:              document.getElementById('track-seal'),
  trackLabel:             document.getElementById('track-label'),
  objectivesTitle:        document.getElementById('objectives-title'),
  objectivesList:         document.getElementById('objectives-list'),
  // Field notes
  fieldNotesListLeft:     document.getElementById('field-notes-list-left'),
  fieldNotesListRight:    document.getElementById('field-notes-list-right'),
  fieldNotesCount:        document.getElementById('field-notes-count'),
  notes:                  document.getElementById('personal-notes'),
  notesCharacterName:     document.getElementById('notes-character-name'),
  notesEmptyState:        document.getElementById('notes-empty-state'),
  newNoteBtn:             document.getElementById('new-note-btn'),
  noteDetailTitle:        document.getElementById('note-detail-title'),
  noteDetailDate:         document.getElementById('note-detail-date'),
  noteDetailOverflow:     document.getElementById('note-detail-overflow'),
  // Sketches
  sketchListLeft:         document.getElementById('sketch-list-left'),
  sketchListRight:        document.getElementById('sketch-list-right'),
  sketchCount:            document.getElementById('sketch-count'),
  sketchLoading:          document.getElementById('sketch-loading'),
  sketchEmpty:            document.getElementById('sketch-empty'),
  sketchEmptyRight:       document.getElementById('sketch-empty-right'),
  sketchImage:            document.getElementById('sketch-image'),
  sketchDetailTitle:      document.getElementById('sketch-detail-title'),
  sketchViewEmpty:        document.getElementById('sketch-view-empty'),
  sketchFilterBtn:        document.getElementById('sketch-filter-btn'),
  portraitInspectBtn:     document.getElementById('portrait-inspect-btn'),
  portraitInspection:     document.getElementById('portrait-inspection'),
  portraitInspectionTitle: document.getElementById('portrait-inspection-title'),
  portraitInspectionImage: document.getElementById('portrait-inspection-image'),
  portraitReturnBtn:      document.getElementById('portrait-return-btn'),
  portraitInkDisplay:     document.getElementById('portrait-ink-display'),
  portraitInkTitle:       document.getElementById('portrait-ink-title'),
  portraitInkMemory:      document.getElementById('portrait-ink-memory'),
  portraitInkEditor:      document.getElementById('portrait-ink-editor'),
  portraitInkAction:      document.getElementById('portrait-ink-action'),
  previousSketch:         document.getElementById('previous-sketch'),
  nextSketch:             document.getElementById('next-sketch'),
  sketchPage:             document.getElementById('sketch-page'),
  sketchTitle:            document.getElementById('sketch-title'),
  sketchCaption:          document.getElementById('sketch-caption'),
  // Nav
  previousPage:           document.getElementById('previous-page'),
  nextPage:               document.getElementById('next-page'),
};

// ── Helpers ──
function escapeHtml(value) {
  return String(value)
    .replaceAll('&', '&amp;').replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;').replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

function getFieldNoteTitle(note) {
  const text = String(note || '').trim();
  const firstLine = text.split(/\r?\n/, 1)[0].trim();
  const title = (text.includes('\n') || text.includes('\r'))
    ? firstLine
    : text.split(/\s+/).slice(0, 5).join(' ');
  return title.length > 32 ? `${title.slice(0, 29).trimEnd()}...` : title;
}

// ── Panel visibility utility ──
// All panels hidden; show only the given panel IDs (left, right)
function showSpread(leftId, rightId) {
  const leftPanels  = ['spread-quest-list','spread-quest-detail-left','spread-notes-left','spread-notes-detail-left','spread-sketches-left','spread-sketch-detail-left'];
  const rightPanels = ['spread-quest-list-right','spread-quest-detail-right','spread-notes-right','spread-notes-detail-right','spread-sketches-right','spread-sketch-detail-right'];
  leftPanels.forEach(id => {
    const p = document.getElementById(id);
    if (p) p.classList.toggle('hidden', id !== leftId);
  });
  rightPanels.forEach(id => {
    const p = document.getElementById(id);
    if (p) p.classList.toggle('hidden', id !== rightId);
  });
}

// ── Spread pagination buttons ──
function updateNavButtons() {
  if (currentView === 'sketch-detail') {
    el.previousPage.classList.add('hidden');
    el.nextPage.classList.add('hidden');
    return;
  }
  // In quest detail: ← returns to list, no next
  if (currentView === 'detail') {
    el.previousPage.classList.remove('hidden');
    el.nextPage.classList.add('hidden');
    return;
  }
  // In note detail: ← returns to notes index, no next
  if (currentView === 'notes-detail') {
    el.previousPage.classList.remove('hidden');
    el.nextPage.classList.add('hidden');
    return;
  }
  if (currentTab === 'active' || currentTab === 'completed') {
    const quests = journalData[currentTab] || [];
    const totalSpreads = Math.max(1, Math.ceil(quests.length / QUESTS_PER_SPREAD));
    el.previousPage.classList.toggle('hidden', currentSpreadPage === 0);
    el.nextPage.classList.toggle('hidden', currentSpreadPage >= totalSpreads - 1);
    return;
  }
  if (currentTab === 'notes' && currentView === 'notes') {
    const totalSpreads = Math.max(1, Math.ceil(fieldNotes.length / (NOTES_PER_PAGE * 2)));
    el.previousPage.classList.toggle('hidden', currentSpreadPage === 0);
    el.nextPage.classList.toggle('hidden', currentSpreadPage >= totalSpreads - 1);
    return;
  }
  if (currentTab === 'sketches' && currentView === 'sketches') {
    const totalSpreads = Math.max(1, Math.ceil(sketches.length / SKETCHES_PER_SPREAD));
    el.previousPage.classList.toggle('hidden', currentSketchSpreadPage === 0);
    el.nextPage.classList.toggle('hidden', currentSketchSpreadPage >= totalSpreads - 1);
    return;
  }
  el.previousPage.classList.add('hidden');
  el.nextPage.classList.add('hidden');
}

// ══════════════════════════════════════════════════════════════
//  QUEST LIST SPREAD
// ══════════════════════════════════════════════════════════════
// Tracked quests are pinned first; C++ supplies the original order for stable re-sorting.
function sortActiveQuests() {
  journalData.active.sort((a, b) =>
    (Number(Boolean(b.tracked)) - Number(Boolean(a.tracked))) ||
    ((a.order ?? 0) - (b.order ?? 0)));
}

function formatDateLine(quest) {
  const parts = [];
  if (quest.acceptedDate)  parts.push(`Accepted ${quest.acceptedDate}`);
  parts.push(quest.completedDate ? `Completed ${quest.completedDate}` : 'No date recorded');
  return parts.join(' · ');
}

function renderQuestListSpread(category, spreadPage) {
  currentView = 'list';
  currentSpreadPage = spreadPage;
  showSpread('spread-quest-list', 'spread-quest-list-right');

  const quests = Array.isArray(journalData[category]) ? journalData[category] : [];
  const label  = category === 'active' ? 'Active Quests' : 'Completed Quests';
  el.listTitle.textContent      = label;
  el.listTitleRight.textContent = quests.length > QUESTS_PER_PAGE ? label + ' (cont.)' : '\u00A0';
  el.questCount.textContent     = quests.length;
  el.questCountRight.textContent = '';
  el.characterName.textContent  = journalData.characterName || 'The last dragonborn';
  el.untrackAllBtn.classList.toggle('hidden', category !== 'active');
  el.untrackAllBtn.disabled = category !== 'active' || !quests.some(quest => quest.tracked);

  const start = spreadPage * QUESTS_PER_SPREAD;
  const pageQuests = quests.slice(start, start + QUESTS_PER_SPREAD);
  const leftQuests  = pageQuests.slice(0, QUESTS_PER_PAGE);
  const rightQuests = pageQuests.slice(QUESTS_PER_PAGE);

  el.emptyState.classList.toggle('hidden', quests.length > 0);

  el.questListLeft.innerHTML  = '';
  el.questListRight.innerHTML = '';

  const globalOffset = start;
  function buildItem(quest, idx) {
    const li = document.createElement('li');
    const globalIdx = globalOffset + idx;
    const classes = ['quest-item'];
    if (quest.tracked) classes.push('tracked');
    if (category === 'active') {
      if (quest.tracked) classes.push('pinned');
      else if (globalIdx > 0 && quests[globalIdx - 1].tracked && idx % QUESTS_PER_PAGE !== 0) classes.push('pin-divider');
    }
    const dateLine = category === 'completed' ? formatDateLine(quest) : '';
    if (dateLine) classes.push('has-dates');
    li.className = classes.join(' ');
    li.innerHTML = `<span class="quest-main"><span class="quest-title">${escapeHtml(quest.title || 'Untitled')}</span>` +
      (dateLine ? `<span class="quest-dates" title="${escapeHtml(dateLine)}">${escapeHtml(dateLine)}</span>` : '') +
      `</span><span class="quest-mark">${globalIdx + 1}</span>`;
    li.addEventListener('click', () => {
      triggerSkyrimSound('focus');
      openQuestDetail(quest);
    });
    return li;
  }

  leftQuests.forEach((q, i)  => el.questListLeft.appendChild(buildItem(q, i)));
  rightQuests.forEach((q, i) => el.questListRight.appendChild(buildItem(q, QUESTS_PER_PAGE + i)));

  updateNavButtons();
}

// ══════════════════════════════════════════════════════════════
//  QUEST DETAIL SPREAD
// ══════════════════════════════════════════════════════════════
function openQuestDetail(quest) {
  currentQuest = quest;
  currentView = 'detail';
  showSpread('spread-quest-detail-left', 'spread-quest-detail-right');

  el.detailEyebrow.textContent   = currentTab === 'completed' ? 'Completed Quest' : 'Active Quest';
  el.entryTitle.textContent      = quest.title || 'Untitled';
  el.objectivesTitle.textContent = 'OBJECTIVES';
  el.entryDate.innerHTML         = '';
  const detailDates = [];
  if (quest.acceptedDate) detailDates.push(['Accepted', quest.acceptedDate]);
  if (currentTab === 'completed') detailDates.push(['Completed', quest.completedDate || 'No date recorded']);
  if (detailDates.length === 0 && currentTab !== 'completed') {
    detailDates.push(['Accepted', 'No date recorded']);
  }
  detailDates.forEach(([label, value]) => {
    const line = document.createElement('span');
    line.className = 'entry-date-line';
    const labelEl = document.createElement('span');
    labelEl.className = 'entry-date-label';
    labelEl.textContent = label;
    line.append(labelEl, document.createTextNode(` ${value}`));
    el.entryDate.appendChild(line);
  });
  el.entryDate.classList.toggle('hidden', detailDates.length === 0);
  el.entryBody.textContent       = quest.body  || 'No notes written for this entry.';

  // Objectives
  el.objectivesList.innerHTML = '';
  const objectives = Array.isArray(quest.objectives) ? quest.objectives : [];
  if (objectives.length === 0) {
    const li = document.createElement('li');
    li.textContent = 'No objectives recorded.';
    li.style.opacity = '0.55';
    el.objectivesList.appendChild(li);
  } else {
    objectives.forEach(obj => {
      const li = document.createElement('li');
      li.textContent = obj.text || 'Unrecorded objective';
      if (obj.done) li.classList.add('done');
      el.objectivesList.appendChild(li);
    });
  }

  // Track button
  el.trackBtn.classList.toggle('hidden', currentTab !== 'active');
  renderTrackButton(quest);

  updateNavButtons();
}

function renderTrackButton(quest) {
  const tracked = Boolean(quest.tracked);
  el.trackBtn.classList.toggle('is-tracked', tracked);
  el.trackSeal.textContent = tracked ? '✦' : '⬟';
  el.trackLabel.textContent = tracked ? 'Untrack Quest' : 'Track Quest';
}

// ══════════════════════════════════════════════════════════════
//  TAB SWITCHING
// ══════════════════════════════════════════════════════════════
function switchTab(tab, btnElement) {
  flushSketchMetadataSave();
  resetPortraitInspection();
  currentTab = tab;
  currentSpreadPage = 0;
  triggerSkyrimSound('focus');

  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  btnElement.classList.add('active');

  if (tab === 'active' || tab === 'completed') {
    currentView = 'list';
    renderQuestListSpread(tab, 0);
  } else if (tab === 'notes') {
    currentView = 'notes';
    renderFieldNotesSpread();
    updateNavButtons();
  } else if (tab === 'sketches') {
    currentView = 'sketches';
    currentSketchSpreadPage = 0;
    showSpread('spread-sketches-left', 'spread-sketches-right');
    renderSketchGallery();
    updateNavButtons();
    if (typeof window.requestJournalScreenshots === 'function') {
      window.requestJournalScreenshots();
    }
  }
}

// ══════════════════════════════════════════════════════════════
//  FIELD NOTES — two-page spread with list → detail model
// ══════════════════════════════════════════════════════════════
const NOTES_PER_PAGE = 8;

function saveJournalState() {
  // Merge into the cached native metadata so entries for screenshots not currently
  // listed are kept, and later screenshot reloads re-apply the latest titles.
  const sketchMetadata = nativeSketchMetadata;
  sketches.forEach(sketch => {
    sketchMetadata[sketch.id] = {
      title: sketch.title || '',
      caption: sketch.caption || '',
      charcoal: Boolean(sketch.charcoal),
      inkSet: Boolean(sketch.inkSet)
    };
  });
  if (typeof window.saveJournalUserData !== 'function') return;
  window.saveJournalUserData(JSON.stringify({ fieldNotes, sketchMetadata, saveGeneration }));
}

function buildNoteItem(note, index) {
  const li = document.createElement('li');
  li.className = `quest-item${index === currentNoteIndex ? ' selected' : ''}`;
  const title = getFieldNoteTitle(note);
  li.innerHTML = `<span class="field-note-label"><span class="field-note-title">${escapeHtml(title)}</span><span class="field-note-date">${escapeHtml(journalData.date || 'Date unknown')}</span></span><span class="quest-mark">${index + 1}</span>`;
  li.addEventListener('click', () => openNoteDetail(index));
  return li;
}

function renderFieldNotesSpread() {
  currentView = 'notes';
  showSpread('spread-notes-left', 'spread-notes-right');
  if (el.notesCharacterName) el.notesCharacterName.textContent = journalData.characterName || 'The last dragonborn';
  el.fieldNotesCount.textContent = fieldNotes.length;
  el.notesEmptyState.classList.toggle('hidden', fieldNotes.length > 0);

  el.fieldNotesListLeft.innerHTML  = '';
  el.fieldNotesListRight.innerHTML = '';
  const start = currentSpreadPage * NOTES_PER_PAGE * 2;
  fieldNotes.slice(start, start + NOTES_PER_PAGE).forEach((note, i) => el.fieldNotesListLeft.appendChild(buildNoteItem(note, start + i)));
  fieldNotes.slice(start + NOTES_PER_PAGE, start + NOTES_PER_PAGE * 2)
    .forEach((note, i) => el.fieldNotesListRight.appendChild(buildNoteItem(note, start + NOTES_PER_PAGE + i)));
  updateNavButtons();
}

function openNoteDetail(index) {
  currentNoteIndex = index;
  triggerSkyrimSound('focus');
  currentView = 'notes-detail';
  showSpread('spread-notes-detail-left', 'spread-notes-detail-right');
  el.noteDetailTitle.textContent = getFieldNoteTitle(fieldNotes[index] || '') || 'Untitled entry';
  el.noteDetailDate.textContent  = journalData.date || 'Date unknown';
  el.notes.value = fieldNotes[index] || '';
  el.notes.focus();
  updateNavButtons();
}

// Kept for compatibility — called by updateJournalFromSkyrim
function renderFieldNotes() {
  if (currentTab !== 'notes') return;
  if (currentView === 'notes') renderFieldNotesSpread();
}

function saveCurrentFieldNote() {
  const note = el.notes.value.trim();
  if (!note) return;
  if (currentNoteIndex >= 0) {
    fieldNotes[currentNoteIndex] = note;
  } else {
    fieldNotes.push(note);
    currentNoteIndex = fieldNotes.length - 1;
  }
  saveJournalState();
  el.notes.blur();
  renderFieldNotesSpread();
}

// ══════════════════════════════════════════════════════════════
//  SKETCHES
// ══════════════════════════════════════════════════════════════
function renderSketchGallery() {
  el.sketchListLeft.innerHTML = '';
  el.sketchListRight.innerHTML = '';
  el.sketchCount.textContent = sketches.length;
  el.sketchLoading.classList.toggle('hidden', sketchDataLoaded);
  el.sketchEmpty.classList.toggle('hidden', !sketchDataLoaded || sketches.length > 0);
  el.sketchEmptyRight.classList.toggle('hidden', !sketchDataLoaded || sketches.length === 0 || sketches.length > SKETCHES_PER_PAGE);
  const start = currentSketchSpreadPage * SKETCHES_PER_SPREAD;
  const spreadSketches = sketches.slice(start, start + SKETCHES_PER_SPREAD);
  spreadSketches.forEach((sketch, spreadIndex) => {
    const index = start + spreadIndex;
    const button = document.createElement('button');
    button.className = `sketch-thumb${index === currentSketchIndex ? ' selected' : ''}`;
    button.type = 'button';
    const title = sketch.title || sketch.caption || `Portrait ${index + 1}`;
    button.setAttribute('aria-label', title);
    button.innerHTML = `<img src="${escapeHtml(sketch.src)}" alt=""><span>${escapeHtml(title)}</span>`;
    button.addEventListener('click', () => {
      currentSketchIndex = index;
      triggerSkyrimSound('focus');
      openSketchDetail(index);
    });
    (spreadIndex < SKETCHES_PER_PAGE ? el.sketchListLeft : el.sketchListRight).appendChild(button);
  });
  updateNavButtons();
}

function openSketchDetail(index) {
  currentSketchIndex = index;
  currentView = 'sketch-detail';
  showSpread('spread-sketch-detail-left', 'spread-sketch-detail-right');
  renderSelectedSketch();
  updateNavButtons();
}

function renderSelectedSketch() {
  const sketch = sketches[currentSketchIndex];
  const has = Boolean(sketch);
  el.sketchImage.classList.toggle('hidden', !has);
  el.sketchViewEmpty.classList.toggle('hidden', has);
  el.previousSketch.disabled = !has || currentSketchIndex === 0;
  el.nextSketch.disabled = !has || currentSketchIndex >= sketches.length - 1;
  if (!has) {
    el.sketchPage.textContent = '0 / 0';
    el.sketchTitle.value = '';
    el.sketchCaption.value = '';
    return;
  }
  el.sketchImage.src = sketch.src;
  el.sketchImage.alt = sketch.title || sketch.caption || `Portrait ${currentSketchIndex + 1}`;
  el.sketchDetailTitle.textContent = el.sketchImage.alt;
  el.sketchTitle.value = sketch.title || '';
  el.sketchCaption.value = sketch.caption || '';
  el.sketchPage.textContent = `${currentSketchIndex + 1} / ${sketches.length}`;
  renderPortraitInk(sketch);
  applySketchFilter();
  renderPortraitInspection(sketch);
}

function changeSketch(direction) {
  const next = currentSketchIndex + direction;
  if (next < 0 || next >= sketches.length) return;
  flushSketchMetadataSave();
  resetPortraitInspection();
  currentSketchIndex = next;
  if (currentView === 'sketches') {
    currentSketchSpreadPage = Math.floor(next / SKETCHES_PER_SPREAD);
    renderSketchGallery();
  }
  renderSelectedSketch();
}

function applySketchFilter() {
  const sketch = sketches[currentSketchIndex];
  const charcoal = Boolean(sketch && sketch.charcoal);
  el.sketchImage.classList.toggle('charcoal-sketch', charcoal);
  el.portraitInspectionImage.classList.toggle('charcoal-sketch', charcoal);
  el.sketchFilterBtn.textContent = charcoal ? 'Original Color' : 'Charcoal Portrait';
}

function renderPortraitInk(sketch) {
  const hasSavedInk = Boolean(sketch.inkSet && !sketch.inkEditing);
  el.portraitInkDisplay.classList.toggle('hidden', !hasSavedInk);
  el.portraitInkEditor.classList.toggle('hidden', hasSavedInk);
  el.portraitInkTitle.textContent = sketch.title || '';
  el.portraitInkMemory.textContent = sketch.caption || '';
  el.portraitInkAction.textContent = sketch.inkSet && !sketch.inkEditing ? 'Amend Entry' : 'Set Ink';
  el.portraitInkAction.classList.toggle('amend-entry', sketch.inkSet && !sketch.inkEditing);
}

function savePortraitInk() {
  const sketch = sketches[currentSketchIndex];
  if (!sketch) return;
  sketch.title = el.sketchTitle.value.trim();
  sketch.caption = el.sketchCaption.value.trim();
  sketch.inkSet = true;
  sketch.inkEditing = false;
  renderPortraitInk(sketch);
  saveSketchMetadata();
}

function expandPortraitInspection() {
  const sketch = sketches[currentSketchIndex];
  if (!sketch || portraitInspectionOpen) return;
  portraitInspectionOpen = true;
  renderPortraitInspection(sketch);
  el.portraitInspection.classList.remove('hidden');
  triggerSkyrimSound('focus');
}

function renderPortraitInspection(sketch) {
  if (!sketch) return;
  el.portraitInspectionTitle.textContent = sketch.title || `Portrait ${currentSketchIndex + 1}`;
  el.portraitInspectionImage.src = sketch.src;
  el.portraitInspectionImage.alt = el.portraitInspectionTitle.textContent;
  el.portraitInspectionImage.classList.toggle('charcoal-sketch', Boolean(sketch.charcoal));
  el.portraitInspection.classList.toggle('charcoal-mode', Boolean(sketch.charcoal));
}

function resetPortraitInspection() {
  if (!portraitInspectionOpen) return;
  portraitInspectionOpen = false;
  el.portraitInspection.classList.add('hidden');
  triggerSkyrimSound('focus');
}

const SKETCH_TITLE_SAVE_DELAY_MS = 400;
let sketchTitleSaveTimer = null;

function scheduleSketchMetadataSave() {
  clearTimeout(sketchTitleSaveTimer);
  sketchTitleSaveTimer = setTimeout(saveSketchMetadata, SKETCH_TITLE_SAVE_DELAY_MS);
}

function flushSketchMetadataSave() {
  if (sketchTitleSaveTimer === null) return;
  saveSketchMetadata();
}

function saveSketchMetadata() {
  clearTimeout(sketchTitleSaveTimer);
  sketchTitleSaveTimer = null;
  const metadata = {};
  sketches.forEach(sketch => {
    metadata[sketch.id] = {
      title: sketch.title || '',
      caption: sketch.caption || '',
      charcoal: Boolean(sketch.charcoal),
      inkSet: Boolean(sketch.inkSet)
    };
  });
  saveJournalState();
}

// ══════════════════════════════════════════════════════════════
//  DOM READY
// ══════════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', () => {
  // Field notes now live in the SKSE cosave; drop copies left by older versions
  // so they cannot leak between saves.
  localStorage.removeItem('skyrim_field_notes_entries');
  localStorage.removeItem('skyrim_field_notes');
  currentNoteIndex = -1;

  // Field notes keydown — Enter saves, Shift+Enter inserts newline
  el.notes.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      saveCurrentFieldNote();
    }
  });

  // Sketch title and memory live-save
  el.sketchTitle.addEventListener('input', () => {
    const sketch = sketches[currentSketchIndex];
    if (!sketch) return;
    sketch.title = el.sketchTitle.value;
    el.sketchDetailTitle.textContent = sketch.title || `Portrait ${currentSketchIndex + 1}`;
    scheduleSketchMetadataSave();
  });
  el.sketchTitle.addEventListener('blur', flushSketchMetadataSave);
  el.sketchTitle.addEventListener('keydown', event => {
    if (event.key === 'Enter') {
      event.preventDefault();
      flushSketchMetadataSave();
      el.sketchTitle.blur();
    }
  });
  el.sketchCaption.addEventListener('input', () => {
    const sketch = sketches[currentSketchIndex];
    if (!sketch) return;
    sketch.caption = el.sketchCaption.value;
  });
  el.sketchCaption.addEventListener('keydown', event => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      el.sketchCaption.blur();
      savePortraitInk();
    }
  });
  el.portraitInkAction.addEventListener('click', () => {
    const sketch = sketches[currentSketchIndex];
    if (!sketch) return;
    if (sketch.inkSet && !sketch.inkEditing) {
      sketch.inkEditing = true;
      el.sketchTitle.value = sketch.title || '';
      el.sketchCaption.value = sketch.caption || '';
      renderPortraitInk(sketch);
      return;
    }
    savePortraitInk();
  });

  // Sketch nav
  el.previousSketch.addEventListener('click', () => changeSketch(-1));
  el.nextSketch.addEventListener('click', () => changeSketch(1));
  el.portraitInspectBtn.addEventListener('click', expandPortraitInspection);
  el.sketchImage.addEventListener('click', expandPortraitInspection);
  el.sketchImage.addEventListener('keydown', event => {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      expandPortraitInspection();
    }
  });
  el.portraitReturnBtn.addEventListener('click', resetPortraitInspection);
  el.portraitInspectionImage.addEventListener('click', resetPortraitInspection);
  el.sketchFilterBtn.addEventListener('click', () => {
    const sketch = sketches[currentSketchIndex];
    if (!sketch) return;
    sketch.charcoal = !sketch.charcoal;
    applySketchFilter();
    saveSketchMetadata();
  });

  // New note button — creates blank entry and opens it in detail view
  if (el.newNoteBtn) {
    el.newNoteBtn.addEventListener('click', () => {
      fieldNotes.push('');
      currentNoteIndex = fieldNotes.length - 1;
      saveJournalState();
      triggerSkyrimSound('focus');
      openNoteDetail(currentNoteIndex);
    });
  }

  // Spread pagination
  el.previousPage.addEventListener('click', () => {
    triggerSkyrimSound('focus');
    if (currentView === 'detail') {
      currentView = 'list';
      renderQuestListSpread(currentTab, currentSpreadPage);
      return;
    }
    if (currentView === 'notes-detail') {
      currentNoteIndex = -1;
      renderFieldNotesSpread();
      updateNavButtons();
      return;
    }
    if (currentView === 'sketch-detail') {
      flushSketchMetadataSave();
      currentView = 'sketches';
      currentSketchSpreadPage = Math.floor(currentSketchIndex / SKETCHES_PER_SPREAD);
      renderSketchGallery();
      return;
    }
    if (currentTab === 'sketches' && currentView === 'sketches') {
      if (currentSketchSpreadPage > 0) {
        currentSketchSpreadPage--;
        renderSketchGallery();
      }
      return;
    }
    if (currentSpreadPage > 0) {
      currentSpreadPage--;
      if (currentTab === 'notes') {
        renderFieldNotesSpread();
      } else {
        renderQuestListSpread(currentTab, currentSpreadPage);
      }
    }
  });
  el.nextPage.addEventListener('click', () => {
    if (currentTab === 'notes') {
      const totalSpreads = Math.ceil(fieldNotes.length / (NOTES_PER_PAGE * 2));
      if (currentSpreadPage < totalSpreads - 1) {
        currentSpreadPage++;
        triggerSkyrimSound('focus');
        renderFieldNotesSpread();
      }
      return;
    }
    if (currentTab === 'sketches') {
      const totalSpreads = Math.ceil(sketches.length / SKETCHES_PER_SPREAD);
      if (currentSketchSpreadPage < totalSpreads - 1) {
        currentSketchSpreadPage++;
        triggerSkyrimSound('focus');
        renderSketchGallery();
      }
      return;
    }
    const quests = journalData[currentTab] || [];
    const totalSpreads = Math.ceil(quests.length / QUESTS_PER_SPREAD);
    if (currentSpreadPage < totalSpreads - 1) {
      currentSpreadPage++;
      triggerSkyrimSound('focus');
      renderQuestListSpread(currentTab, currentSpreadPage);
    }
  });

  // Mouse-wheel spread pagination on the book cover
  document.querySelector('.book-cover').addEventListener('wheel', (e) => {
    if (!['active', 'completed', 'notes', 'sketches'].includes(currentTab) ||
        currentView === 'detail' || currentView === 'notes-detail' || currentView === 'sketch-detail') return;
    e.preventDefault();
    const itemCount = currentTab === 'notes' ? fieldNotes.length :
      currentTab === 'sketches' ? sketches.length : (journalData[currentTab] || []).length;
    const itemsPerSpread = currentTab === 'notes' ? NOTES_PER_PAGE * 2 :
      currentTab === 'sketches' ? SKETCHES_PER_SPREAD : QUESTS_PER_SPREAD;
    const totalSpreads = Math.max(1, Math.ceil(itemCount / itemsPerSpread));
    const spreadPage = currentTab === 'sketches' ? currentSketchSpreadPage : currentSpreadPage;
    if (e.deltaY > 0 && spreadPage < totalSpreads - 1) {
      if (currentTab === 'sketches') currentSketchSpreadPage++;
      else currentSpreadPage++;
      triggerSkyrimSound('focus');
      if (currentTab === 'notes') renderFieldNotesSpread();
      else if (currentTab === 'sketches') renderSketchGallery();
      else renderQuestListSpread(currentTab, currentSpreadPage);
    } else if (e.deltaY < 0 && spreadPage > 0) {
      if (currentTab === 'sketches') currentSketchSpreadPage--;
      else currentSpreadPage--;
      triggerSkyrimSound('focus');
      if (currentTab === 'notes') renderFieldNotesSpread();
      else if (currentTab === 'sketches') renderSketchGallery();
      else renderQuestListSpread(currentTab, currentSpreadPage);
    }
  }, { passive: false });

  // Track/untrack button
  el.trackBtn.addEventListener('click', () => {
    if (!currentQuest || !saveGeneration) return;
    triggerSkyrimSound('focus');
    currentQuest.tracked = !currentQuest.tracked;
    sortActiveQuests();
    renderTrackButton(currentQuest);
    if (typeof window.toggleQuestTracking === 'function') {
      window.toggleQuestTracking(JSON.stringify({ id: currentQuest.id, saveGeneration }));
    }
  });

  el.untrackAllBtn.addEventListener('click', () => {
    if (!saveGeneration || !journalData.active.some(quest => quest.tracked)) return;
    journalData.active.forEach(quest => { quest.tracked = false; });
    sortActiveQuests();
    renderQuestListSpread('active', currentSpreadPage);
    if (typeof window.clearAllQuestTracking === 'function') {
      window.clearAllQuestTracking(JSON.stringify({ saveGeneration }));
    }
  });

  // Typing state bridge
  function notifyTypingState(active) {
    if (typeof window.setTypingState === 'function') window.setTypingState(active);
  }
  function isTextInput(element) {
    return element instanceof Element && element.matches('input, textarea');
  }
  function updateTypingState() {
    notifyTypingState(isTextInput(document.activeElement));
  }
  document.addEventListener('focusin', (e) => {
    if (isTextInput(e.target)) updateTypingState();
  });
  document.addEventListener('focusout', (e) => {
    if (isTextInput(e.target)) setTimeout(updateTypingState, 0);
  });

  // Initial render
  renderQuestListSpread('active', 0);
  renderSketchGallery();
  if (typeof window.requestJournalUserData === 'function') window.requestJournalUserData();
});

// ══════════════════════════════════════════════════════════════
//  PRISMA UI / C++ BRIDGE HOOKS
// ══════════════════════════════════════════════════════════════

window.onJournalOpen  = () => {
  if (typeof window.requestJournalUserData === 'function') {
    window.requestJournalUserData();
  }
};  // sound played by C++
window.onJournalClose = () => { flushSketchMetadataSave(); };  // sound played by C++

// C++ pushes fresh quest data via this function every time the journal opens
// or a quest stage event fires.
window.updateJournalFromSkyrim = (incomingJsonData) => {
  try {
    const parsed = typeof incomingJsonData === 'string' ? JSON.parse(incomingJsonData) : incomingJsonData;

    journalData = {
      characterName: parsed.characterName || 'The last dragonborn',
      date:          parsed.date          || 'Date unknown',
      active:    (Array.isArray(parsed.active)    ? parsed.active    : []).map(q => ({ ...q, tracked: q.tracked === true })),
      completed: (Array.isArray(parsed.completed) ? parsed.completed : []).map(q => ({ ...q, tracked: q.tracked === true }))
    };

    el.characterName.textContent = journalData.characterName;
    if (el.notesCharacterName) el.notesCharacterName.textContent = journalData.characterName;
    renderFieldNotes();

    if (currentTab === 'active' || currentTab === 'completed') {
      if (currentView === 'list') {
        renderQuestListSpread(currentTab, currentSpreadPage);
      } else if (currentView === 'detail' && currentQuest) {
        const all = [...journalData.active, ...journalData.completed];
        const refreshed = all.find(q => q.id === currentQuest.id);
        if (refreshed) {
          openQuestDetail(refreshed);
        } else {
          currentView = 'list';
          renderQuestListSpread(currentTab, 0);
        }
      }
    }
  } catch (err) {
    console.error('Failed to parse Skyrim quest data:', err);
  }
};

window.loadNativeUserData = (data) => {
  const parsed = data || {};
  saveGeneration = parsed.saveGeneration;
  fieldNotes = Array.isArray(parsed.fieldNotes) ? parsed.fieldNotes.filter(note => typeof note === 'string') : [];
  if (parsed.sketchMetadataExists && parsed.sketchMetadata) {
    nativeSketchMetadata = parsed.sketchMetadata;
    sketches.forEach(applySketchMetadata);
  }
  currentNoteIndex = fieldNotes.length > 0 ? Math.min(currentNoteIndex, fieldNotes.length - 1) : -1;
  if (currentTab === 'notes' && currentView === 'notes') renderFieldNotesSpread();
  if (currentTab === 'sketches' && currentView === 'sketches') renderSketchGallery();
};

window.resetJournalSaveState = () => {
  saveGeneration = 0;
  fieldNotes = [];
  currentNoteIndex = -1;
  currentQuest = null;
  currentTab = 'active';
  currentView = 'list';
  currentSpreadPage = 0;
  el.notes.value = '';
  window.updateJournalFromSkyrim({ active: [], completed: [] });
};

// C++ screenshot paths for sketches tab
window.loadScreenshotsFromSkyrim = (fileList) => {
  flushSketchMetadataSave();
  try {
    const parsed = typeof fileList === 'string' ? JSON.parse(fileList) : fileList;
    const incoming = Array.isArray(parsed) ? parsed : Array.from(parsed || []);
    sketches = incoming.map((item, index) => {
      const src = typeof item === 'string' ? item : (item.path || item.filePath || item.src || item.url);
      const id  = typeof item === 'string' ? src   : (item.id   || src || `sketch-${index + 1}`);
      return {
        id,
        src,
        title: typeof item === 'string' ? '' : (item.title || item.caption || item.filename || ''),
        caption: typeof item === 'string' ? '' : (item.memory || '')
      };
    }).filter(s => Boolean(s.src));
    sketchDataLoaded = true;
    sketches.forEach(applySketchMetadata);
    currentSketchIndex = 0;
    renderSketchGallery();
    if (currentView === 'sketch-detail') renderSelectedSketch();
  } catch (err) { console.error('Failed to load Skyrim sketches:', err); }
};

function applySketchMetadata(sketch) {
  const metadata = nativeSketchMetadata[sketch.id];
  if (metadata && typeof metadata.title === 'string') sketch.title = metadata.title;
  else if (metadata && typeof metadata.caption === 'string') sketch.title = metadata.caption;
  if (metadata && typeof metadata.memory === 'string') sketch.caption = metadata.memory;
  else if (metadata && typeof metadata.title === 'string' && typeof metadata.caption === 'string') sketch.caption = metadata.caption;
  sketch.charcoal = Boolean(metadata && metadata.charcoal);
  sketch.inkSet = metadata && typeof metadata.inkSet === 'boolean'
    ? metadata.inkSet
    : Boolean(sketch.title || sketch.caption);
}

// Escape key closes journal via C++
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !e.target.matches('input, textarea')) {
    if (currentView === 'detail' && (currentTab === 'active' || currentTab === 'completed')) {
      currentView = 'list';
      renderQuestListSpread(currentTab, currentSpreadPage);
      return;
    }
    if (currentView === 'notes-detail') {
      currentNoteIndex = -1;
      renderFieldNotesSpread();
      updateNavButtons();
      return;
    }
    if (typeof window.requestJournalClose === 'function') {
      window.requestJournalClose('close');
    }
  }
});
