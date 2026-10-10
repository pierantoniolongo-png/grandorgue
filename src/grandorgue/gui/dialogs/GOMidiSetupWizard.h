/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOMIDISETUPWIZARD_H
#define GOMIDISETUPWIZARD_H

#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "midi/GOMidiListener.h"
#include "midi/events/GOMidiCallback.h"
#include "midi/events/GOMidiEvent.h"
#include "midi/events/GOMidiReceiverEventPattern.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxGauge;
class wxListCtrl;
class wxSimplebook;
class wxStaticText;

class GOButtonControl;
class GOConfigMidiObject;
class GOMidiReceivingSendingObject;
class GOMidiSystem;
class GOOrganController;

/**
 * A step-by-step assistant that detects the connected MIDI devices and learns
 * which device/channel/controller drives every part of the console: the
 * pedalboard, the manuals, the expression pedals, the crescendo pedal and the
 * toe studs (e.g. the ten footswitches of a Behringer FCB1010).
 *
 * The learned assignments are applied to the loaded organ and, optionally,
 * stored in the Initial MIDI configuration so that every organ (sample set)
 * loaded later uses the same console mapping.
 */
class GOMidiSetupWizard : public wxDialog, private GOMidiCallback {
public:
  /** The kind of physical element a step is waiting for */
  enum TargetKind { KIND_KEYBOARD, KIND_CONTINUOUS, KIND_BUTTON };

  /** Number of toe studs offered (the FCB1010 has ten footswitches) */
  static constexpr unsigned N_TOE_STUDS = 10;

private:
  /** One element of the console to be learned */
  struct Target {
    wxString m_label;
    wxString m_instruction;
    TargetKind m_kind;
    GOMidiReceivingSendingObject *p_object = nullptr;
    // built-in Initial MIDI object matched by MIDIInputNumber (manuals and
    // enclosures). nullptr if the object is matched by path
    GOConfigMidiObject *p_BuiltinInitial = nullptr;
    bool m_IsSelected = true;
    bool m_IsDetected = false;
    GOMidiReceiverEventPattern m_pattern;
    wxString m_DetectedText;
  };

  /** A function that can be assigned to a toe stud */
  struct ButtonChoice {
    wxString m_label;
    GOButtonControl *p_button;
  };

  /** A (device, channel, message type) seen on the MIDI inputs */
  struct SeenSource {
    unsigned m_device;
    int m_channel;
    GOMidiEvent::MidiType m_type;
    int m_key;
    unsigned m_nEvents;
    wxString m_LastText;
  };

  enum { PAGE_DEVICES = 0, PAGE_SELECT, PAGE_LEARN, PAGE_SUMMARY };

  GOOrganController &r_OrganController;
  GOMidiSystem &r_MidiSystem;
  GOMidiListener m_listener;

  std::vector<Target> m_targets;
  std::vector<ButtonChoice> m_ButtonChoices;
  std::vector<SeenSource> m_SeenSources;
  // indices of the targets in m_targets that are selected for learning
  std::vector<unsigned> m_LearnOrder;
  int m_LearnPos;
  // index of the first toe stud target in m_targets
  unsigned m_FirstToeStudTarget;
  // the event that was accepted last: used for ignoring repetitions
  bool m_IsAdvancePending;

  wxTimer m_AdvanceTimer;

  wxSimplebook *m_book;
  wxButton *m_BackButton;
  wxButton *m_NextButton;

  // page "devices"
  wxListCtrl *m_DeviceList;
  wxListCtrl *m_SourceList;
  wxStaticText *m_NoDeviceText;

  // page "select"
  std::vector<wxCheckBox *> m_TargetChecks;
  std::vector<wxCheckBox *> m_ToeStudChecks;
  std::vector<wxChoice *> m_ToeStudChoices;

  // page "learn"
  wxStaticText *m_StepTitle;
  wxStaticText *m_StepInstruction;
  wxStaticText *m_StepStatus;
  wxGauge *m_StepGauge;
  wxButton *m_SkipButton;
  wxButton *m_RepeatButton;
  wxButton *m_PrevStepButton;

  // page "summary"
  wxListCtrl *m_SummaryList;
  wxCheckBox *m_ToInitialCheck;
  wxCheckBox *m_SaveOrganCheck;

  void BuildTargets();
  void BuildButtonChoices();

  wxWindow *CreateDevicesPage(wxWindow *parent);
  wxWindow *CreateSelectPage(wxWindow *parent);
  wxWindow *CreateLearnPage(wxWindow *parent);
  wxWindow *CreateSummaryPage(wxWindow *parent);

  void FillDeviceList();
  void UpdateSourceList(const GOMidiEvent &event);
  void FillSummary();

  void ReadSelection();
  void StartLearning();
  void ShowCurrentStep();
  void GoToStep(int pos);

  /** Whether the event may be accepted for a target of the given kind */
  static bool isEventSuitable(const GOMidiEvent &event, TargetKind kind);
  /**
   * Find the already learned target the event belongs to
   * @return the target index or -1 if the event is not assigned yet
   */
  int FindAssignedTarget(const GOMidiEvent &event, int exceptTarget) const;
  /** Fill the pattern of the target from the event */
  void AcceptEvent(Target &target, const GOMidiEvent &event);
  wxString DescribeEvent(const GOMidiEvent &event) const;
  wxString DeviceName(unsigned deviceId) const;

  void ApplyAssignments();

  void SetPage(unsigned page);
  void UpdateNavigation();

  void OnMidiEvent(const GOMidiEvent &event) override;

  void OnBack(wxCommandEvent &event);
  void OnNext(wxCommandEvent &event);
  void OnSkip(wxCommandEvent &event);
  void OnRepeat(wxCommandEvent &event);
  void OnPrevStep(wxCommandEvent &event);
  void OnAdvanceTimer(wxTimerEvent &event);
  void OnRefreshDevices(wxCommandEvent &event);

public:
  GOMidiSetupWizard(
    wxWindow *parent,
    GOOrganController &organController,
    GOMidiSystem &midiSystem);
  ~GOMidiSetupWizard();

  /** Whether the user asked to save the organ settings after applying */
  bool IsToSaveOrgan() const;

  DECLARE_EVENT_TABLE()
};

#endif /* GOMIDISETUPWIZARD_H */
