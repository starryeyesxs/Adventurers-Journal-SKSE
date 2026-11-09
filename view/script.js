// ============================================================
//  Adventurer's Journal — script.js
//  True two-page book spread architecture (1.6.3)
// ============================================================

// ── Sample data (replaced by C++ via updateJournalFromSkyrim) ──
let journalData = {
  characterName: 'The last dragonborn',
  date: 'Date unknown',
  active: [
    {
      id: "mg07", tracked: false,
      title: "The Staff of Magnus",
      date: "17th of Last Seed, 4E 201",
      body: "Paratus Decimius confirmed the Staff of Magnus is buried deep within the ruins of Labyrinthian. I need to push past the outer chambers, survive whatever traps the ancient Nords left behind, and retrieve it before Ancano destabilizes the Eye.",
      objectives: [
        { text: "Enter the depths of Labyrinthian", done: true },
        { text: "Defeat the Skeletal Dragon in the chasm", done: true },
        { text: "Claim the Staff of Magnus from Morokei", done: false }
      ]
    },
    {
      id: "bounty01", tracked: false,
      title: "Hunt: The Pale Lady",
      date: "15th of Last Seed, 4E 201",
      body: "Locals in Dawnstar spoke of an ancient spirit haunting Frostmere Crypt. No glowing markers—just follow the mountain pass southwest of the city and watch for the cairn stones. Bring silver or enchanted steel.",
      objectives: [
        { text: "Locate Frostmere Crypt southwest of Dawnstar", done: false },
        { text: "Banish the Pale Lady", done: false }
      ]
    }
  ],
  completed: [
    {
      id: "mg06", tracked: false,
      title: "Revealing the Unseen",
      date: "16th of Last Seed, 4E 201",
      body: "Fought through the Falmer infestation in Mzulft and realigned the Oculory lenses. Paratus blamed the College for blocking his projection and is already plotting to report back to the Synod.",
      objectives: [
        { text: "Recover the Focusing Crystal", done: true },
        { text: "Align the Oculory mirrors", done: true },
        { text: "Report back to Savos Aren", done: true }
      ]
    }
  ]
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
    li.className = `quest-item${quest.tracked ? ' tracked' : ''}`;
    li.innerHTML = `<span>${escapeHtml(quest.title || 'Untitled')}</span><span class="quest-mark">${globalOffset + idx + 1}</span>`;
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
  el.entryDate.textContent       = quest.date  || journalData.date || 'Date unknown';
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

function saveFieldNotes() {
  localStorage.setItem('skyrim_field_notes_entries', JSON.stringify(fieldNotes));
}

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
  window.saveJournalUserData(JSON.stringify({ fieldNotes, sketchMetadata }));
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
  saveFieldNotes();
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
  // Load saved field notes
  const savedNotes = localStorage.getItem('skyrim_field_notes_entries');
  if (savedNotes) {
    try {
      const parsed = JSON.parse(savedNotes);
      fieldNotes = Array.isArray(parsed) ? parsed.filter(n => typeof n === 'string') : [];
    } catch (e) { console.error('Failed to load field notes:', e); }
  } else {
    const legacy = localStorage.getItem('skyrim_field_notes');
    if (legacy) fieldNotes = [legacy];
  }
  currentNoteIndex = fieldNotes.length > 0 ? 0 : -1;

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
      saveFieldNotes();
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
    if (!currentQuest) return;
    triggerSkyrimSound('focus');
    currentQuest.tracked = !currentQuest.tracked;
    renderTrackButton(currentQuest);
    if (typeof window.toggleQuestTracking === 'function') {
      window.toggleQuestTracking(currentQuest.id);
    }
  });

  el.untrackAllBtn.addEventListener('click', () => {
    if (!journalData.active.some(quest => quest.tracked)) return;
    journalData.active.forEach(quest => { quest.tracked = false; });
    renderQuestListSpread('active', currentSpreadPage);
    if (typeof window.clearAllQuestTracking === 'function') {
      window.clearAllQuestTracking();
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

    // Preserve local 'tracked' state across data refreshes from C++
    const prevTracked = new Map();
    (journalData.active    || []).forEach(q => prevTracked.set(q.id, q.tracked));
    (journalData.completed || []).forEach(q => prevTracked.set(q.id, q.tracked));

    journalData = {
      characterName: parsed.characterName || 'The last dragonborn',
      date:          parsed.date          || 'Date unknown',
      active:    (Array.isArray(parsed.active)    ? parsed.active    : []).map(q => ({ ...q, tracked: typeof q.tracked === 'boolean' ? q.tracked : Boolean(prevTracked.get(q.id)) })),
      completed: (Array.isArray(parsed.completed) ? parsed.completed : []).map(q => ({ ...q, tracked: typeof q.tracked === 'boolean' ? q.tracked : Boolean(prevTracked.get(q.id)) }))
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
  const localNotes = fieldNotes.slice();
  const localMetadata = {};
  for (let index = 0; index < localStorage.length; index++) {
    const key = localStorage.key(index);
    if (!key || !key.startsWith('skyrim_sketch_caption_')) continue;
    const id = key.slice('skyrim_sketch_caption_'.length);
    localMetadata[id] = { caption: localStorage.getItem(key) || '' };
  }

  if (parsed.fieldNotesExists) {
    fieldNotes = Array.isArray(parsed.fieldNotes) ? parsed.fieldNotes.filter(note => typeof note === 'string') : [];
  } else if (localNotes.length || localStorage.getItem('skyrim_field_notes')) {
    const legacy = localNotes.length ? localNotes :
      [localStorage.getItem('skyrim_field_notes')].filter(Boolean);
    fieldNotes = legacy;
    if (typeof window.saveJournalUserData === 'function') {
      window.saveJournalUserData(JSON.stringify({ fieldNotes, sketchMetadata: localMetadata }));
    }
  }
  if (parsed.sketchMetadataExists && parsed.sketchMetadata) {
    nativeSketchMetadata = parsed.sketchMetadata;
    sketches.forEach(applySketchMetadata);
  }
  currentNoteIndex = fieldNotes.length > 0 ? Math.min(currentNoteIndex, fieldNotes.length - 1) : -1;
  if (currentTab === 'notes' && currentView === 'notes') renderFieldNotesSpread();
  if (currentTab === 'sketches' && currentView === 'sketches') renderSketchGallery();
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
