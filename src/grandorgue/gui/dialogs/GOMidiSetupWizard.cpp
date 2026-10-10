/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOMidiSetupWizard.h"

#include <map>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include "combinations/control/GOGeneralButtonControl.h"
#include "config/GOConfig.h"
#include "config/GOConfigMidiObject.h"
#include "control/GOButtonControl.h"
#include "midi/GOMidiMap.h"
#include "midi/GOMidiSystem.h"
#include "midi/elements/GOMidiReceiver.h"
#include "midi/ports/GOMidiPort.h"
#include "model/GOEnclosure.h"
#include "model/GOManual.h"

#include "GOOrganController.h"

enum {
  ID_WIZ_BACK = 7300,
  ID_WIZ_NEXT,
  ID_WIZ_SKIP,
  ID_WIZ_REPEAT,
  ID_WIZ_PREV_STEP,
  ID_WIZ_REFRESH,
  ID_WIZ_TIMER,
};

// MIDI controllers that never identify a pedal or a footswitch
static const int CC_BANK_SELECT_MSB = 0;
static const int CC_BANK_SELECT_LSB = 32;
static const int CC_FIRST_CHANNEL_MODE = 120;

// The delays before going to the next step after a successful detection
static const int ADVANCE_DELAY_BUTTON_MS = 700;
static const int ADVANCE_DELAY_CONTINUOUS_MS = 1500;

BEGIN_EVENT_TABLE(GOMidiSetupWizard, wxDialog)
EVT_BUTTON(ID_WIZ_BACK, GOMidiSetupWizard::OnBack)
EVT_BUTTON(ID_WIZ_NEXT, GOMidiSetupWizard::OnNext)
EVT_BUTTON(ID_WIZ_SKIP, GOMidiSetupWizard::OnSkip)
EVT_BUTTON(ID_WIZ_REPEAT, GOMidiSetupWizard::OnRepeat)
EVT_BUTTON(ID_WIZ_PREV_STEP, GOMidiSetupWizard::OnPrevStep)
EVT_BUTTON(ID_WIZ_REFRESH, GOMidiSetupWizard::OnRefreshDevices)
EVT_TIMER(ID_WIZ_TIMER, GOMidiSetupWizard::OnAdvanceTimer)
END_EVENT_TABLE()

GOMidiSetupWizard::GOMidiSetupWizard(
  wxWindow *parent,
  GOOrganController &organController,
  GOMidiSystem &midiSystem)
  : wxDialog(
      parent,
      wxID_ANY,
      _("MIDI setup wizard"),
      wxDefaultPosition,
      wxSize(900, 680),
      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    r_OrganController(organController),
    r_MidiSystem(midiSystem),
    m_LearnPos(-1),
    m_FirstToeStudTarget(0),
    m_IsAdvancePending(false),
    m_AdvanceTimer(this, ID_WIZ_TIMER) {
  BuildButtonChoices();
  BuildTargets();

  wxBoxSizer *const topSizer = new wxBoxSizer(wxVERTICAL);

  m_book = new wxSimplebook(this, wxID_ANY);
  m_book->AddPage(CreateDevicesPage(m_book), wxEmptyString);
  m_book->AddPage(CreateSelectPage(m_book), wxEmptyString);
  m_book->AddPage(CreateLearnPage(m_book), wxEmptyString);
  m_book->AddPage(CreateSummaryPage(m_book), wxEmptyString);
  topSizer->Add(m_book, 1, wxEXPAND | wxALL, 10);

  topSizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

  wxBoxSizer *const buttons = new wxBoxSizer(wxHORIZONTAL);

  buttons->Add(new wxButton(this, wxID_CANCEL, _("Cancel")), 0, wxALL, 5);
  buttons->AddStretchSpacer();
  m_BackButton = new wxButton(this, ID_WIZ_BACK, _("< &Back"));
  buttons->Add(m_BackButton, 0, wxALL, 5);
  m_NextButton = new wxButton(this, ID_WIZ_NEXT, _("&Next >"));
  buttons->Add(m_NextButton, 0, wxALL, 5);
  topSizer->Add(buttons, 0, wxEXPAND | wxALL, 5);

  SetSizer(topSizer);
  SetMinSize(wxSize(760, 560));
  Layout();
  CentreOnParent();

  FillDeviceList();
  SetPage(PAGE_DEVICES);

  m_listener.Register(&r_MidiSystem);
  m_listener.SetCallback(this);
}

GOMidiSetupWizard::~GOMidiSetupWizard() {
  m_AdvanceTimer.Stop();
  m_listener.SetCallback(nullptr);
  m_listener.Unregister();
}

/*
 * Building the list of elements to learn
 */

void GOMidiSetupWizard::BuildButtonChoices() {
  m_ButtonChoices.clear();

  // the built-in generals of the combination setter: they exist in every organ
  for (unsigned generalN = 1; generalN <= 10; generalN++) {
    GOButtonControl *pButton = r_OrganController.GetButtonControl(
      wxString::Format(wxT("General%02u"), generalN), false);

    if (pButton)
      m_ButtonChoices.push_back(
        {wxString::Format(_("General %u (combination setter)"), generalN),
         pButton});
  }

  const struct {
    const wxChar *name;
    wxString label;
  } setterButtons[] = {
    {wxT("Prev"), _("Sequencer: previous combination")},
    {wxT("Next"), _("Sequencer: next combination")},
    {wxT("Current"), _("Sequencer: current combination")},
    {wxT("GC"), _("General cancel (G.C.)")},
    {wxT("Set"), _("Set (store a combination)")},
  };

  for (const auto &def : setterButtons) {
    GOButtonControl *pButton
      = r_OrganController.GetButtonControl(def.name, false);

    if (pButton)
      m_ButtonChoices.push_back({def.label, pButton});
  }

  // the generals defined by the sample set itself
  for (unsigned n = r_OrganController.GetGeneralCount(), generalI = 0;
       generalI < n;
       generalI++) {
    GOGeneralButtonControl *pGeneral = r_OrganController.GetGeneral(generalI);

    if (pGeneral)
      m_ButtonChoices.push_back(
        {wxString::Format(_("Organ general: %s"), pGeneral->GetName()),
         pGeneral});
  }
}

void GOMidiSetupWizard::BuildTargets() {
  GOConfig &config = r_OrganController.GetSettings();

  m_targets.clear();

  // the pedalboard and the manuals
  for (unsigned n = r_OrganController.GetODFManualCount(),
                manualI = r_OrganController.GetFirstManualIndex();
       manualI < n;
       manualI++) {
    GOManual *pManual = r_OrganController.GetManual(manualI);

    if (pManual) {
      Target t;
      const int inputNumber = pManual->GetMidiInputNumber();

      t.m_kind = KIND_KEYBOARD;
      t.p_object = pManual;
      if (manualI == 0) {
        t.m_label = wxString::Format(_("Pedalboard (%s)"), pManual->GetName());
        t.m_instruction
          = _("Press any key of the pedalboard.\n\nThe wizard detects the MIDI "
              "device and the channel the pedalboard is sending on.");
      } else {
        t.m_label
          = wxString::Format(_("Manual %u (%s)"), manualI, pManual->GetName());
        t.m_instruction = wxString::Format(
          _("Press any key of manual %u (manuals are counted from the bottom: "
            "manual 1 is the lowest one).\n\nThe wizard detects the MIDI "
            "device and the channel this keyboard is sending on."),
          manualI);
      }
      if (inputNumber > 0)
        t.p_BuiltinInitial = config.FindMidiInitialObject(
          GOMidiObject::OBJECT_TYPE_MANUAL, inputNumber);
      m_targets.push_back(t);
    }
  }

  // the expression pedals (enclosures)
  for (unsigned n = r_OrganController.GetEnclosureCount(), enclosureI = 0;
       enclosureI < n;
       enclosureI++) {
    GOEnclosure *pEnclosure = r_OrganController.GetEnclosureElement(enclosureI);

    if (pEnclosure) {
      Target t;
      const int inputNumber = pEnclosure->GetMidiInputNumber();

      t.m_kind = KIND_CONTINUOUS;
      t.p_object = pEnclosure;
      t.m_label = wxString::Format(
        _("Expression pedal %u (%s)"), enclosureI + 1, pEnclosure->GetName());
      t.m_instruction = wxString::Format(
        _("Move the expression pedal that has to control \"%s\" slowly from "
          "fully closed to fully open.\n\nOn a Behringer FCB1010 this is "
          "usually pedal A."),
        pEnclosure->GetName());
      // only the first enclosure is selected by default
      t.m_IsSelected = enclosureI == 0;
      if (inputNumber > 0)
        t.p_BuiltinInitial = config.FindMidiInitialObject(
          GOMidiObject::OBJECT_TYPE_ENCLOSURE, inputNumber);
      m_targets.push_back(t);
    }
  }

  // the crescendo pedal of the combination setter
  GOEnclosure *pCrescendo = r_OrganController.GetEnclosure(wxT("Swell"));

  if (pCrescendo) {
    Target t;

    t.m_kind = KIND_CONTINUOUS;
    t.p_object = pCrescendo;
    t.m_label = _("Crescendo pedal");
    t.m_instruction
      = _("Move the pedal you want to use as crescendo pedal slowly from "
          "minimum to maximum.\n\nOn a Behringer FCB1010 this is usually "
          "pedal B.");
    m_targets.push_back(t);
  }

  // the toe studs. The actual function is chosen on the selection page
  m_FirstToeStudTarget = m_targets.size();
  for (unsigned studN = 1; studN <= N_TOE_STUDS; studN++) {
    Target t;

    t.m_kind = KIND_BUTTON;
    t.m_label = wxString::Format(_("Toe stud %u"), studN);
    t.m_IsSelected = !m_ButtonChoices.empty();
    m_targets.push_back(t);
  }
}

/*
 * Pages
 */

static wxStaticText *create_title(wxWindow *parent, const wxString &text) {
  wxStaticText *const pTitle = new wxStaticText(parent, wxID_ANY, text);
  wxFont font = pTitle->GetFont();

  font.SetPointSize(font.GetPointSize() + 3);
  font.SetWeight(wxFONTWEIGHT_BOLD);
  pTitle->SetFont(font);
  return pTitle;
}

static const int TEXT_WRAP_WIDTH = 820;

static wxStaticText *create_text(wxWindow *parent, const wxString &text) {
  wxStaticText *const pText = new wxStaticText(parent, wxID_ANY, text);

  pText->Wrap(TEXT_WRAP_WIDTH);
  return pText;
}

wxWindow *GOMidiSetupWizard::CreateDevicesPage(wxWindow *parent) {
  wxPanel *const page = new wxPanel(parent);
  wxBoxSizer *const sizer = new wxBoxSizer(wxVERTICAL);

  sizer->Add(create_title(page, _("1. MIDI devices")), 0, wxBOTTOM, 8);
  sizer->Add(
    create_text(
      page,
      _("These MIDI input devices were found on this computer. Play a few "
        "keys, move the pedals and press the footswitches: every source "
        "that sends MIDI appears in the lower list together with its "
        "channel.")),
    0,
    wxEXPAND | wxBOTTOM,
    8);

  m_DeviceList = new wxListCtrl(
    page,
    wxID_ANY,
    wxDefaultPosition,
    wxSize(-1, 120),
    wxLC_REPORT | wxLC_SINGLE_SEL);
  m_DeviceList->AppendColumn(_("MIDI input device"), wxLIST_FORMAT_LEFT, 380);
  m_DeviceList->AppendColumn(_("State"), wxLIST_FORMAT_LEFT, 120);
  m_DeviceList->AppendColumn(_("Activity"), wxLIST_FORMAT_LEFT, 180);
  sizer->Add(m_DeviceList, 0, wxEXPAND | wxBOTTOM, 4);

  wxBoxSizer *const refreshSizer = new wxBoxSizer(wxHORIZONTAL);

  m_NoDeviceText = new wxStaticText(page, wxID_ANY, wxEmptyString);
  refreshSizer->Add(m_NoDeviceText, 1, wxALIGN_CENTER_VERTICAL);
  refreshSizer->Add(new wxButton(page, ID_WIZ_REFRESH, _("&Refresh")), 0);
  sizer->Add(refreshSizer, 0, wxEXPAND | wxBOTTOM, 8);

  sizer->Add(
    new wxStaticText(page, wxID_ANY, _("Detected MIDI sources:")),
    0,
    wxBOTTOM,
    4);
  m_SourceList = new wxListCtrl(
    page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
  m_SourceList->AppendColumn(_("Device"), wxLIST_FORMAT_LEFT, 230);
  m_SourceList->AppendColumn(_("Channel"), wxLIST_FORMAT_LEFT, 65);
  m_SourceList->AppendColumn(_("Message"), wxLIST_FORMAT_LEFT, 185);
  m_SourceList->AppendColumn(_("Events"), wxLIST_FORMAT_RIGHT, 60);
  m_SourceList->AppendColumn(_("Last event"), wxLIST_FORMAT_LEFT, 220);
  sizer->Add(m_SourceList, 1, wxEXPAND | wxBOTTOM, 8);

  sizer->Add(
    create_text(
      page,
      _("If a device is missing or disabled, enable it in Settings > MIDI "
        "Devices and open this wizard again.")),
    0,
    wxEXPAND);

  page->SetSizer(sizer);
  return page;
}

wxWindow *GOMidiSetupWizard::CreateSelectPage(wxWindow *parent) {
  wxScrolledWindow *const page = new wxScrolledWindow(parent);
  wxBoxSizer *const sizer = new wxBoxSizer(wxVERTICAL);

  page->SetScrollRate(0, 10);
  sizer->Add(
    create_title(page, _("2. What do you want to configure?")), 0, wxBOTTOM, 8);
  sizer->Add(
    create_text(
      page,
      wxString::Format(
        _("Organ: %s. Untick the elements your console does not have. In "
          "the next step the wizard asks you to touch every selected "
          "element in turn."),
        r_OrganController.GetOrganName())),
    0,
    wxEXPAND | wxBOTTOM,
    8);

  wxStaticBoxSizer *const keysBox
    = new wxStaticBoxSizer(wxVERTICAL, page, _("Keyboards"));
  wxStaticBoxSizer *const pedalsBox = new wxStaticBoxSizer(
    wxVERTICAL, page, _("Expression and crescendo pedals"));

  m_TargetChecks.clear();
  for (unsigned targetI = 0; targetI < m_FirstToeStudTarget; targetI++) {
    Target &t = m_targets[targetI];
    wxStaticBoxSizer *const box
      = t.m_kind == KIND_KEYBOARD ? keysBox : pedalsBox;
    wxCheckBox *const check
      = new wxCheckBox(box->GetStaticBox(), wxID_ANY, t.m_label);

    check->SetValue(t.m_IsSelected);
    box->Add(check, 0, wxALL, 3);
    m_TargetChecks.push_back(check);
  }
  wxBoxSizer *const columns = new wxBoxSizer(wxHORIZONTAL);
  wxBoxSizer *const leftColumn = new wxBoxSizer(wxVERTICAL);

  leftColumn->Add(keysBox, 0, wxEXPAND | wxBOTTOM, 8);
  leftColumn->Add(pedalsBox, 0, wxEXPAND);
  columns->Add(leftColumn, 1, wxEXPAND | wxRIGHT, 10);

  wxStaticBoxSizer *const studsBox
    = new wxStaticBoxSizer(wxVERTICAL, page, _("Toe studs / footswitches"));
  wxFlexGridSizer *const grid = new wxFlexGridSizer(2, 4, 10);
  wxArrayString choiceLabels;

  for (const ButtonChoice &choice : m_ButtonChoices)
    choiceLabels.Add(choice.m_label);

  // default functions: generals 1-8, then sequencer previous and next
  const wxChar *const defaultNames[N_TOE_STUDS] = {
    wxT("General01"),
    wxT("General02"),
    wxT("General03"),
    wxT("General04"),
    wxT("General05"),
    wxT("General06"),
    wxT("General07"),
    wxT("General08"),
    wxT("Prev"),
    wxT("Next")};

  m_ToeStudChecks.clear();
  m_ToeStudChoices.clear();
  grid->AddGrowableCol(1, 1);
  for (unsigned studI = 0; studI < N_TOE_STUDS; studI++) {
    Target &t = m_targets[m_FirstToeStudTarget + studI];
    wxCheckBox *const check
      = new wxCheckBox(studsBox->GetStaticBox(), wxID_ANY, t.m_label);
    wxChoice *const choice = new wxChoice(
      studsBox->GetStaticBox(),
      wxID_ANY,
      wxDefaultPosition,
      wxSize(260, -1),
      choiceLabels);
    GOButtonControl *pDefault
      = r_OrganController.GetButtonControl(defaultNames[studI], false);
    int selection = choiceLabels.IsEmpty() ? wxNOT_FOUND : 0;

    for (unsigned n = m_ButtonChoices.size(), choiceI = 0; choiceI < n;
         choiceI++)
      if (pDefault && m_ButtonChoices[choiceI].p_button == pDefault)
        selection = choiceI;
    choice->SetSelection(selection);
    check->SetValue(t.m_IsSelected);
    check->Enable(!choiceLabels.IsEmpty());
    choice->Enable(!choiceLabels.IsEmpty());
    grid->Add(check, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(choice, 1, wxEXPAND);
    m_ToeStudChecks.push_back(check);
    m_ToeStudChoices.push_back(choice);
  }
  studsBox->Add(grid, 0, wxEXPAND | wxALL, 5);
  columns->Add(studsBox, 1, wxEXPAND);
  sizer->Add(columns, 0, wxEXPAND | wxBOTTOM, 8);
  sizer->Add(
    create_text(
      page,
      _("Behringer FCB1010: in the factory setting the ten footswitches send "
        "Program Change messages and the two pedals send Control Change "
        "messages. Both are detected automatically, no reprogramming is "
        "needed.")),
    0,
    wxEXPAND);

  page->SetSizer(sizer);
  page->FitInside();
  return page;
}

wxWindow *GOMidiSetupWizard::CreateLearnPage(wxWindow *parent) {
  wxPanel *const page = new wxPanel(parent);
  wxBoxSizer *const sizer = new wxBoxSizer(wxVERTICAL);

  sizer->Add(
    create_title(page, _("3. Touch the console elements")), 0, wxBOTTOM, 12);
  m_StepGauge = new wxGauge(page, wxID_ANY, 100);
  sizer->Add(m_StepGauge, 0, wxEXPAND | wxBOTTOM, 12);

  m_StepTitle = new wxStaticText(page, wxID_ANY, wxEmptyString);
  wxFont font = m_StepTitle->GetFont();

  font.SetPointSize(font.GetPointSize() + 6);
  font.SetWeight(wxFONTWEIGHT_BOLD);
  m_StepTitle->SetFont(font);
  sizer->Add(m_StepTitle, 0, wxEXPAND | wxBOTTOM, 12);

  m_StepInstruction = new wxStaticText(page, wxID_ANY, wxEmptyString);
  font = m_StepInstruction->GetFont();
  font.SetPointSize(font.GetPointSize() + 2);
  m_StepInstruction->SetFont(font);
  sizer->Add(m_StepInstruction, 0, wxEXPAND | wxBOTTOM, 20);

  m_StepStatus = new wxStaticText(page, wxID_ANY, wxEmptyString);
  font = m_StepStatus->GetFont();
  font.SetPointSize(font.GetPointSize() + 2);
  font.SetWeight(wxFONTWEIGHT_BOLD);
  m_StepStatus->SetFont(font);
  sizer->Add(m_StepStatus, 0, wxEXPAND | wxBOTTOM, 20);

  sizer->AddStretchSpacer();

  wxBoxSizer *const stepButtons = new wxBoxSizer(wxHORIZONTAL);

  m_PrevStepButton
    = new wxButton(page, ID_WIZ_PREV_STEP, _("<< &Previous element"));
  m_RepeatButton = new wxButton(page, ID_WIZ_REPEAT, _("&Learn again"));
  m_SkipButton = new wxButton(page, ID_WIZ_SKIP, _("&Skip this element >>"));
  stepButtons->Add(m_PrevStepButton, 0, wxRIGHT, 5);
  stepButtons->AddStretchSpacer();
  stepButtons->Add(m_RepeatButton, 0, wxRIGHT, 5);
  stepButtons->Add(m_SkipButton, 0);
  sizer->Add(stepButtons, 0, wxEXPAND);

  page->SetSizer(sizer);
  return page;
}

wxWindow *GOMidiSetupWizard::CreateSummaryPage(wxWindow *parent) {
  wxPanel *const page = new wxPanel(parent);
  wxBoxSizer *const sizer = new wxBoxSizer(wxVERTICAL);

  sizer->Add(create_title(page, _("4. Summary")), 0, wxBOTTOM, 8);
  sizer->Add(
    create_text(
      page,
      _("These assignments will be applied. Elements that were skipped keep "
        "their current MIDI settings.")),
    0,
    wxEXPAND | wxBOTTOM,
    8);
  m_SummaryList = new wxListCtrl(
    page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
  m_SummaryList->AppendColumn(_("Element"), wxLIST_FORMAT_LEFT, 360);
  m_SummaryList->AppendColumn(_("MIDI assignment"), wxLIST_FORMAT_LEFT, 420);
  sizer->Add(m_SummaryList, 1, wxEXPAND | wxBOTTOM, 8);

  m_ToInitialCheck = new wxCheckBox(
    page,
    wxID_ANY,
    _("Use these assignments for all organs (store them in the Initial MIDI "
      "settings)"));
  m_ToInitialCheck->SetValue(true);
  sizer->Add(m_ToInitialCheck, 0, wxBOTTOM, 4);

  m_SaveOrganCheck = new wxCheckBox(
    page, wxID_ANY, _("Save the settings of the current organ now"));
  m_SaveOrganCheck->SetValue(true);
  sizer->Add(m_SaveOrganCheck, 0);

  page->SetSizer(sizer);
  return page;
}

/*
 * Device monitoring
 */

wxString GOMidiSetupWizard::DeviceName(unsigned deviceId) const {
  wxString name = r_MidiSystem.GetMidiMap().GetDeviceLogicalNameById(deviceId);

  return name.IsEmpty() ? wxString::Format(_("device %u"), deviceId) : name;
}

wxString GOMidiSetupWizard::DescribeEvent(const GOMidiEvent &event) const {
  wxString what;

  switch (event.GetMidiType()) {
  case GOMidiEvent::MIDI_NOTE:
    what = wxString::Format(_("note %d"), event.GetKey());
    break;
  case GOMidiEvent::MIDI_CTRL_CHANGE:
    what = wxString::Format(
      _("controller CC%d = %d"), event.GetKey(), event.GetValue());
    break;
  case GOMidiEvent::MIDI_PGM_CHANGE:
    what = wxString::Format(_("program change %d"), event.GetKey());
    break;
  default:
    what = event.ToString(r_MidiSystem.GetMidiMap());
  }
  return wxString::Format(
    _("%s, channel %d, %s"),
    DeviceName(event.GetDevice()),
    event.GetChannel(),
    what);
}

void GOMidiSetupWizard::FillDeviceList() {
  unsigned nActive = 0;

  const ptr_vector<GOMidiPort> &ports = r_MidiSystem.GetInDevices();

  m_DeviceList->DeleteAllItems();
  for (unsigned n = ports.size(), portI = 0; portI < n; portI++) {
    const GOMidiPort *pPort = ports[portI];
    const long row = m_DeviceList->InsertItem(
      m_DeviceList->GetItemCount(), pPort->GetName());
    wxString state;

    if (pPort->IsActive()) {
      state = _("active");
      nActive++;
    } else
      state = pPort->IsToUse() ? _("not opened") : _("disabled");
    m_DeviceList->SetItem(row, 1, state);
    m_DeviceList->SetItem(row, 2, wxEmptyString);
    m_DeviceList->SetItemData(row, pPort->IsActive() ? pPort->GetID() : 0);
  }
  if (!ports.size())
    m_NoDeviceText->SetLabel(_("No MIDI input device found."));
  else if (!nActive)
    m_NoDeviceText->SetLabel(
      _("No MIDI input device is active: enable them in Settings."));
  else
    m_NoDeviceText->SetLabel(
      wxString::Format(_("%u active MIDI input device(s)."), nActive));
}

void GOMidiSetupWizard::UpdateSourceList(const GOMidiEvent &event) {
  const GOMidiEvent::MidiType type = event.GetMidiType();

  if (
    type == GOMidiEvent::MIDI_NOTE || type == GOMidiEvent::MIDI_CTRL_CHANGE
    || type == GOMidiEvent::MIDI_PGM_CHANGE) {
    int row = -1;

    for (unsigned n = m_SeenSources.size(), sourceI = 0; sourceI < n;
         sourceI++) {
      const SeenSource &s = m_SeenSources[sourceI];

      // the controllers are listed separately: each one is a pedal
      if (
        s.m_device == event.GetDevice() && s.m_channel == event.GetChannel()
        && s.m_type == type
        && (type != GOMidiEvent::MIDI_CTRL_CHANGE || s.m_key == event.GetKey()))
        row = sourceI;
    }

    const wxString last = DescribeEvent(event).AfterLast(',').Trim(false);

    if (row < 0) {
      wxString typeName;

      switch (type) {
      case GOMidiEvent::MIDI_NOTE:
        typeName = _("Notes");
        break;
      case GOMidiEvent::MIDI_CTRL_CHANGE:
        typeName = wxString::Format(_("Control Change CC%d"), event.GetKey());
        break;
      default:
        typeName = _("Program Change");
      }
      m_SeenSources.push_back(
        {event.GetDevice(),
         event.GetChannel(),
         type,
         event.GetKey(),
         0,
         wxEmptyString});
      row = m_SeenSources.size() - 1;
      m_SourceList->InsertItem(row, DeviceName(event.GetDevice()));
      m_SourceList->SetItem(
        row, 1, wxString::Format(wxT("%d"), event.GetChannel()));
      m_SourceList->SetItem(row, 2, typeName);
    }

    SeenSource &s = m_SeenSources[row];

    s.m_nEvents++;
    s.m_LastText = last;
    m_SourceList->SetItem(row, 3, wxString::Format(wxT("%u"), s.m_nEvents));
    m_SourceList->SetItem(row, 4, last);

    // show the activity also in the device list
    for (long n = m_DeviceList->GetItemCount(), rowI = 0; rowI < n; rowI++)
      if (
        m_DeviceList->GetItemData(rowI) == event.GetDevice()
        && event.GetDevice() != 0)
        m_DeviceList->SetItem(
          rowI,
          2,
          wxString::Format(_("receiving on channel %d"), event.GetChannel()));
  }
}

/*
 * Learning
 */

bool GOMidiSetupWizard::isEventSuitable(
  const GOMidiEvent &event, TargetKind kind) {
  const GOMidiEvent::MidiType type = event.GetMidiType();
  const bool isNoteOn = type == GOMidiEvent::MIDI_NOTE && event.GetValue() > 0;
  const bool isUsableCtrl = type == GOMidiEvent::MIDI_CTRL_CHANGE
    && event.GetKey() != CC_BANK_SELECT_MSB
    && event.GetKey() != CC_BANK_SELECT_LSB
    && event.GetKey() < CC_FIRST_CHANNEL_MODE;
  bool isSuitable = false;

  switch (kind) {
  case KIND_KEYBOARD:
    isSuitable = isNoteOn;
    break;
  case KIND_CONTINUOUS:
    isSuitable = isUsableCtrl;
    break;
  case KIND_BUTTON:
    isSuitable = isNoteOn || (isUsableCtrl && event.GetValue() > 0)
      || type == GOMidiEvent::MIDI_PGM_CHANGE;
    break;
  }
  return isSuitable;
}

int GOMidiSetupWizard::FindAssignedTarget(
  const GOMidiEvent &event, int exceptTarget) const {
  const GOMidiEvent::MidiType type = event.GetMidiType();
  int found = -1;

  for (unsigned n = m_targets.size(), targetI = 0; targetI < n && found < 0;
       targetI++) {
    const Target &t = m_targets[targetI];
    const GOMidiReceiverEventPattern &p = t.m_pattern;

    if (
      (int)targetI != exceptTarget && t.m_IsSelected && t.m_IsDetected
      && p.deviceId == event.GetDevice() && p.channel == event.GetChannel()) {
      bool isSame = false;

      if (t.m_kind == KIND_KEYBOARD)
        // the whole channel belongs to the keyboard
        isSame = type == GOMidiEvent::MIDI_NOTE;
      else if (p.type == MIDI_M_NOTE)
        isSame = type == GOMidiEvent::MIDI_NOTE && p.key == event.GetKey();
      else if (p.type == MIDI_M_CTRL_CHANGE)
        isSame
          = type == GOMidiEvent::MIDI_CTRL_CHANGE && p.key == event.GetKey();
      else if (p.type == MIDI_M_PGM_CHANGE)
        isSame
          = type == GOMidiEvent::MIDI_PGM_CHANGE && p.key == event.GetKey();
      if (isSame)
        found = targetI;
    }
  }
  return found;
}

void GOMidiSetupWizard::AcceptEvent(Target &target, const GOMidiEvent &event) {
  // The values are the same as the "Listen for event" button of the MIDI
  // event dialog produces, so the result behaves exactly like a manual setup
  GOMidiReceiverEventPattern p;

  p.deviceId = event.GetDevice();
  p.channel = event.GetChannel();
  p.debounce_time = 0;
  p.low_key = 0;
  p.high_key = 127;
  switch (target.m_kind) {
  case KIND_KEYBOARD:
    p.type = MIDI_M_NOTE;
    p.key = 0; // no transposition
    p.low_value = 1;
    p.high_value = 127;
    break;
  case KIND_CONTINUOUS:
    p.type = MIDI_M_CTRL_CHANGE;
    p.key = event.GetKey();
    p.low_value = 0;
    p.high_value = 127;
    break;
  case KIND_BUTTON:
    switch (event.GetMidiType()) {
    case GOMidiEvent::MIDI_NOTE:
      p.type = MIDI_M_NOTE;
      break;
    case GOMidiEvent::MIDI_CTRL_CHANGE:
      p.type = MIDI_M_CTRL_CHANGE;
      break;
    default:
      p.type = MIDI_M_PGM_CHANGE;
    }
    p.key = event.GetKey();
    p.low_value = 0;
    p.high_value = 1;
    break;
  }
  target.m_pattern = p;
  target.m_IsDetected = true;

  wxString what;

  switch (p.type) {
  case MIDI_M_NOTE:
    what = target.m_kind == KIND_KEYBOARD
      ? wxString(_("notes"))
      : wxString::Format(_("note %d"), p.key);
    break;
  case MIDI_M_CTRL_CHANGE:
    what = wxString::Format(_("controller CC%d"), p.key);
    break;
  default:
    what = wxString::Format(_("program change %d"), p.key);
  }
  target.m_DetectedText = wxString::Format(
    _("%s, channel %d, %s"), DeviceName(p.deviceId), (int)p.channel, what);
}

void GOMidiSetupWizard::ReadSelection() {
  for (unsigned n = m_TargetChecks.size(), checkI = 0; checkI < n; checkI++)
    m_targets[checkI].m_IsSelected = m_TargetChecks[checkI]->GetValue();
  for (unsigned studI = 0; studI < N_TOE_STUDS; studI++) {
    Target &t = m_targets[m_FirstToeStudTarget + studI];
    const int selection = m_ToeStudChoices[studI]->GetSelection();

    t.m_IsSelected = m_ToeStudChecks[studI]->GetValue() && selection >= 0;
    t.p_object = selection >= 0 ? m_ButtonChoices[selection].p_button : nullptr;
    if (t.p_object) {
      const wxString &function = m_ButtonChoices[selection].m_label;

      t.m_label = wxString::Format(_("Toe stud %u: %s"), studI + 1, function);
      t.m_instruction = wxString::Format(
        _("Press the toe stud (footswitch) number %u. It will act as \"%s\"."
          "\n\nOn a Behringer FCB1010 press footswitch %u."),
        studI + 1,
        function,
        studI + 1);
    }
  }
}

void GOMidiSetupWizard::StartLearning() {
  m_LearnOrder.clear();
  for (unsigned n = m_targets.size(), targetI = 0; targetI < n; targetI++)
    if (m_targets[targetI].m_IsSelected && m_targets[targetI].p_object)
      m_LearnOrder.push_back(targetI);
  // start from the first element that has not been detected yet
  m_LearnPos = 0;
  while (m_LearnPos < (int)m_LearnOrder.size()
         && m_targets[m_LearnOrder[m_LearnPos]].m_IsDetected)
    m_LearnPos++;
  if (m_LearnPos >= (int)m_LearnOrder.size())
    m_LearnPos = 0;
}

void GOMidiSetupWizard::ShowCurrentStep() {
  const int nSteps = m_LearnOrder.size();

  m_IsAdvancePending = false;
  m_AdvanceTimer.Stop();
  if (m_LearnPos >= 0 && m_LearnPos < nSteps) {
    const Target &t = m_targets[m_LearnOrder[m_LearnPos]];

    m_StepGauge->SetRange(nSteps);
    m_StepGauge->SetValue(m_LearnPos);
    m_StepTitle->SetLabel(wxString::Format(
      _("Step %d of %d: %s"), m_LearnPos + 1, nSteps, t.m_label));
    m_StepInstruction->SetLabel(t.m_instruction);
    m_StepInstruction->Wrap(m_book->GetClientSize().GetWidth() - 20);
    m_StepStatus->SetForegroundColour(
      t.m_IsDetected ? wxColour(0, 128, 0) : wxColour(160, 90, 0));
    m_StepStatus->SetLabel(
      t.m_IsDetected ? wxString::Format(_("Assigned: %s"), t.m_DetectedText)
                     : wxString(_("Waiting for MIDI input...")));
    m_PrevStepButton->Enable(m_LearnPos > 0);
    m_RepeatButton->Enable(t.m_IsDetected);
    m_book->GetPage(PAGE_LEARN)->Layout();
  }
}

void GOMidiSetupWizard::GoToStep(int pos) {
  if (pos >= (int)m_LearnOrder.size()) {
    FillSummary();
    SetPage(PAGE_SUMMARY);
  } else {
    m_LearnPos = pos < 0 ? 0 : pos;
    ShowCurrentStep();
  }
}

void GOMidiSetupWizard::OnMidiEvent(const GOMidiEvent &event) {
  UpdateSourceList(event);

  if (
    m_book->GetSelection() == PAGE_LEARN && !m_IsAdvancePending
    && m_LearnPos >= 0 && m_LearnPos < (int)m_LearnOrder.size()) {
    const unsigned targetI = m_LearnOrder[m_LearnPos];
    Target &t = m_targets[targetI];

    if (isEventSuitable(event, t.m_kind)) {
      const int assignedTo = FindAssignedTarget(event, targetI);

      if (assignedTo >= 0) {
        // do not complain about the repetitions of the previous element
        if (!(t.m_kind == KIND_CONTINUOUS
              && event.GetMidiType() == GOMidiEvent::MIDI_CTRL_CHANGE)) {
          m_StepStatus->SetForegroundColour(wxColour(192, 0, 0));
          m_StepStatus->SetLabel(wxString::Format(
            _("Ignored: %s is already assigned to \"%s\"."),
            DescribeEvent(event),
            m_targets[assignedTo].m_label));
          m_StepStatus->Wrap(TEXT_WRAP_WIDTH);
        }
      } else {
        AcceptEvent(t, event);
        m_StepStatus->SetForegroundColour(wxColour(0, 128, 0));
        m_StepStatus->SetLabel(
          wxString::Format(_("Detected: %s"), t.m_DetectedText));
        m_RepeatButton->Enable(true);
        m_IsAdvancePending = true;
        m_AdvanceTimer.StartOnce(
          t.m_kind == KIND_CONTINUOUS ? ADVANCE_DELAY_CONTINUOUS_MS
                                      : ADVANCE_DELAY_BUTTON_MS);
      }
    }
  }
}

void GOMidiSetupWizard::OnAdvanceTimer(wxTimerEvent &event) {
  m_IsAdvancePending = false;
  if (m_book->GetSelection() == PAGE_LEARN)
    GoToStep(m_LearnPos + 1);
}

void GOMidiSetupWizard::OnSkip(wxCommandEvent &event) {
  GoToStep(m_LearnPos + 1);
}

void GOMidiSetupWizard::OnPrevStep(wxCommandEvent &event) {
  GoToStep(m_LearnPos - 1);
}

void GOMidiSetupWizard::OnRepeat(wxCommandEvent &event) {
  if (m_LearnPos >= 0 && m_LearnPos < (int)m_LearnOrder.size()) {
    Target &t = m_targets[m_LearnOrder[m_LearnPos]];

    t.m_IsDetected = false;
    t.m_DetectedText.Clear();
    ShowCurrentStep();
  }
}

/*
 * Summary and applying
 */

void GOMidiSetupWizard::FillSummary() {
  m_SummaryList->DeleteAllItems();
  for (unsigned targetI : m_LearnOrder) {
    const Target &t = m_targets[targetI];
    const long row
      = m_SummaryList->InsertItem(m_SummaryList->GetItemCount(), t.m_label);

    m_SummaryList->SetItem(
      row,
      1,
      t.m_IsDetected ? t.m_DetectedText : wxString(_("(skipped - unchanged)")));
  }
}

void GOMidiSetupWizard::ApplyAssignments() {
  GOConfig &config = r_OrganController.GetSettings();
  const bool isToInitial = m_ToInitialCheck->GetValue();
  // several toe studs may be assigned to the same function
  std::map<GOMidiReceivingSendingObject *, std::vector<const Target *>>
    byObject;
  std::vector<GOMidiReceivingSendingObject *> objectOrder;
  bool isModified = false;

  for (unsigned targetI : m_LearnOrder) {
    const Target &t = m_targets[targetI];

    if (t.m_IsDetected && t.p_object) {
      if (byObject.find(t.p_object) == byObject.end())
        objectOrder.push_back(t.p_object);
      byObject[t.p_object].push_back(&t);
    }
  }

  for (GOMidiReceivingSendingObject *pObject : objectOrder) {
    GOMidiReceiver *pReceiver = pObject->GetMidiReceiver();

    if (pReceiver) {
      const std::vector<const Target *> &targets = byObject[pObject];
      GOMidiReceiver newReceiver(*pReceiver);

      newReceiver.ClearEvents();
      for (const Target *pTarget : targets)
        newReceiver.GetEvent(newReceiver.AddNewEvent()) = pTarget->m_pattern;
      if (pReceiver->RenewFrom(newReceiver))
        isModified = true;

      if (isToInitial) {
        GOConfigMidiObject *pBuiltin = targets.front()->p_BuiltinInitial;

        if (pBuiltin) {
          // manuals and enclosures: matched by MIDIInputNumber
          GOMidiReceiver *pInitialReceiver = pBuiltin->GetMidiReceiver();
          GOConfigMidiObject *pByPath
            = config.FindMidiInitialObject(pObject->GetPath());

          if (pInitialReceiver)
            pInitialReceiver->RenewFrom(newReceiver);
          // a user-added object with the same path would take precedence
          if (pByPath && pByPath != pBuiltin)
            pByPath->AssignFrom(*pObject);
        } else
          // other objects are matched by their path
          config.AssignToInitial(*pObject);
      }
    }
  }
  if (isModified)
    r_OrganController.SetOrganModified();
  if (isToInitial)
    config.Flush();
}

bool GOMidiSetupWizard::IsToSaveOrgan() const {
  return m_SaveOrganCheck->GetValue();
}

/*
 * Navigation
 */

void GOMidiSetupWizard::SetPage(unsigned page) {
  m_book->ChangeSelection(page);
  if (page == PAGE_LEARN)
    ShowCurrentStep();
  UpdateNavigation();
}

void GOMidiSetupWizard::UpdateNavigation() {
  const int page = m_book->GetSelection();

  m_BackButton->Enable(page != PAGE_DEVICES);
  m_NextButton->SetLabel(
    page == PAGE_SUMMARY   ? _("&Apply")
      : page == PAGE_LEARN ? _("&Finish >")
                           : _("&Next >"));
  m_NextButton->SetDefault();
}

void GOMidiSetupWizard::OnBack(wxCommandEvent &event) {
  switch (m_book->GetSelection()) {
  case PAGE_SELECT:
    SetPage(PAGE_DEVICES);
    break;
  case PAGE_LEARN:
    SetPage(PAGE_SELECT);
    break;
  case PAGE_SUMMARY:
    m_LearnPos = m_LearnOrder.empty() ? 0 : m_LearnOrder.size() - 1;
    SetPage(PAGE_LEARN);
    break;
  }
}

void GOMidiSetupWizard::OnNext(wxCommandEvent &event) {
  switch (m_book->GetSelection()) {
  case PAGE_DEVICES:
    SetPage(PAGE_SELECT);
    break;
  case PAGE_SELECT:
    ReadSelection();
    StartLearning();
    if (m_LearnOrder.empty())
      wxMessageBox(
        _("Please select at least one element to configure."),
        _("MIDI setup wizard"),
        wxOK | wxICON_INFORMATION,
        this);
    else
      SetPage(PAGE_LEARN);
    break;
  case PAGE_LEARN:
    m_AdvanceTimer.Stop();
    m_IsAdvancePending = false;
    FillSummary();
    SetPage(PAGE_SUMMARY);
    break;
  case PAGE_SUMMARY:
    ApplyAssignments();
    EndModal(wxID_OK);
    break;
  }
}

void GOMidiSetupWizard::OnRefreshDevices(wxCommandEvent &event) {
  FillDeviceList();
}
