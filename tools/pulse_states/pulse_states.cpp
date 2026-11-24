// pulse_states.cpp - builds the virtual-patient catalogue for BreathGuard RT (host tool, Linux).
//
// For each patient type: Pulse stabilizes the patient with its disease (conditions are applied during
// stabilization, which takes minutes), then ventilates it for warmup_s with square PCV 20/5 cmH2O, 12 bpm, I:E 1:2,
// so that Pulse's breath-averaged R and C have converged (CLAUDE.md, "R y C de Pulse ... son promedios"). The result
// is saved as states/BG_<id>.json. The QNX process `patient -S` loads it in well under a second.
//
// Build (once):
//   cmake -S tools/pulse_states -B ~/pulse/states-build -DCMAKE_PREFIX_PATH=$HOME/pulse/build/install
//   cmake --build ~/pulse/states-build
// Run from Pulse's data directory:
//   cd ~/pulse/build/install/bin && ~/pulse/states-build/pulse_states list|all|<id> [warmup_s]
// Then copy states/BG_*.json to the VM: scripts/bench.py deploy
//
// The catalogue (ids, labels) must match scripts/patients.py, which the dashboard uses.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/PulseEngine.h"
#include "cdm/engine/SEConditionManager.h"
#include "cdm/engine/SEPatientConfiguration.h"
#include "cdm/patient/SEPatient.h"
#include "cdm/patient/actions/SEAsthmaAttack.h"
#include "cdm/patient/actions/SEDyspnea.h"
#include "cdm/patient/actions/SEMechanicalVentilation.h"
#include "cdm/patient/conditions/SEAcuteRespiratoryDistressSyndrome.h"
#include "cdm/patient/conditions/SEChronicObstructivePulmonaryDisease.h"
#include "cdm/patient/conditions/SEPneumonia.h"
#include "cdm/properties/SEScalar0To1.h"
#include "cdm/properties/SEScalarLength.h"
#include "cdm/properties/SEScalarPressure.h"
#include "cdm/properties/SEScalarPressureTimePerVolume.h"
#include "cdm/properties/SEScalarTime.h"
#include "cdm/properties/SEScalarVolumePerPressure.h"
#include "cdm/substance/SESubstanceFraction.h"
#include "cdm/substance/SESubstanceManager.h"
#include "cdm/system/physiology/SEBloodChemistrySystem.h"
#include "cdm/system/physiology/SERespiratorySystem.h"

namespace {

struct PatientType {
  const char* id;
  const char* patient_file;
  const char* what;
  std::function<void(SEConditionManager&)> conditions;  // applied while stabilizing
  std::function<void(PhysiologyEngine&)> actions;        // applied after stabilizing (acute events)
};

const std::vector<PatientType>& catalogue()
{
  static const std::vector<PatientType> c = {
    {"healthy_male", "StandardMale.json", "healthy adult male, 180 cm", nullptr, nullptr},
    {"healthy_female", "StandardFemale.json", "healthy adult female, 163 cm", nullptr, nullptr},
    {"obese_male", "Overweight.json", "obese male, 180 cm, 98 kg (same ideal weight as healthy_male)", nullptr,
     nullptr},
    {"ards_moderate", "StandardMale.json", "ARDS, moderate (both lungs, severity 0.6): stiff lungs, shunt",
     [](SEConditionManager& cm) {
       auto& a = cm.GetAcuteRespiratoryDistressSyndrome();
       a.GetSeverity(eLungCompartment::LeftLung).SetValue(0.6);
       a.GetSeverity(eLungCompartment::RightLung).SetValue(0.6);
     },
     nullptr},
    {"ards_severe", "StandardMale.json", "ARDS, severe (both lungs, severity 0.9)",
     [](SEConditionManager& cm) {
       auto& a = cm.GetAcuteRespiratoryDistressSyndrome();
       a.GetSeverity(eLungCompartment::LeftLung).SetValue(0.9);
       a.GetSeverity(eLungCompartment::RightLung).SetValue(0.9);
     },
     nullptr},
    {"copd", "StandardMale.json", "COPD (bronchitis 0.5, emphysema 0.6): high resistance, gas trapping",
     [](SEConditionManager& cm) {
       auto& c = cm.GetChronicObstructivePulmonaryDisease();
       c.GetBronchitisSeverity().SetValue(0.5);
       c.GetEmphysemaSeverity(eLungCompartment::LeftLung).SetValue(0.6);
       c.GetEmphysemaSeverity(eLungCompartment::RightLung).SetValue(0.6);
     },
     nullptr},
    {"pneumonia", "StandardFemale.json", "pneumonia, right lung (severity 0.7)",
     [](SEConditionManager& cm) { cm.GetPneumonia().GetSeverity(eLungCompartment::RightLung).SetValue(0.7); },
     nullptr},
    {"asthma_attack", "StandardFemale.json", "acute asthma attack (severity 0.6): very high resistance", nullptr,
     [](PhysiologyEngine& pe) {
       SEAsthmaAttack a;
       a.GetSeverity().SetValue(0.6);
       pe.ProcessAction(a);
     }},
  };
  return c;
}

double now_s()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int build(const PatientType& p, double warmup_s)
{
  const std::string out = std::string("./states/BG_") + p.id + ".json";
  std::printf("%s: %s\n", p.id, p.what);
  std::fflush(stdout);
  const double t0 = now_s();
  std::unique_ptr<PhysiologyEngine> pe = CreatePulseEngine();
  pe->GetLogger()->LogToConsole(false);
  pe->GetLogger()->SetLogFile(std::string("./test_results/pulse_states_") + p.id + ".log");
  SEPatientConfiguration config;
  config.SetPatientFile(p.patient_file);
  if (p.conditions) p.conditions(config.GetConditions());
  if (!pe->InitializeEngine(config)) {
    std::fprintf(stderr, "%s: Pulse could not stabilize the patient (see test_results/)\n", p.id);
    return 1;
  }
  const double t_stab = now_s() - t0;
  if (p.actions) p.actions(*pe);

  // Sedated patient, no own breathing; ventilated from outside, as in src/patient.cpp.
  SEDyspnea dyspnea;
  dyspnea.GetTidalVolumeSeverity().SetValue(1.0);
  dyspnea.GetRespirationRateSeverity().SetValue(1.0);
  pe->ProcessAction(dyspnea);
  SEMechanicalVentilation mech;
  mech.SetState(eSwitch::On);
  const SESubstanceManager& subs = pe->GetSubstanceManager();
  const double fio2 = 0.21;
  mech.GetGasFraction(*subs.GetSubstance("Oxygen")).GetFractionAmount().SetValue(fio2);
  mech.GetGasFraction(*subs.GetSubstance("CarbonDioxide")).GetFractionAmount().SetValue(4.0e-4);
  mech.GetGasFraction(*subs.GetSubstance("Nitrogen")).GetFractionAmount().SetValue(1.0 - fio2 - 4.0e-4);
  const double dt = pe->GetTimeStep(TimeUnit::s);
  const long steps = static_cast<long>(warmup_s / dt);
  for (long i = 0; i < steps; i++) {
    const double t = static_cast<double>(i) * dt;
    const double phase = t / 5.0 - static_cast<double>(static_cast<long>(t / 5.0));
    mech.GetPressure().SetValue(phase < 1.0 / 3.0 ? 20.0 : 5.0, PressureUnit::cmH2O);
    pe->ProcessAction(mech);
    if (!pe->AdvanceModelTime()) {
      std::fprintf(stderr, "%s: Pulse failed while ventilating (t = %.1f s)\n", p.id, t);
      return 1;
    }
  }
  const SERespiratorySystem* resp = pe->GetRespiratorySystem();
  const SEPatient& pat = pe->GetPatient();
  std::printf("%s: stabilized in %.0f s, ventilated %.0f s in %.0f s; %s %.0f cm; R %.1f cmH2O/(L/s), "
              "C %.1f mL/cmH2O, SpO2 %.1f %% on room air\n",
              p.id, t_stab, warmup_s, now_s() - t0 - t_stab, pat.GetSex() == ePatient_Sex::Female ? "F" : "M",
              pat.GetHeight(LengthUnit::cm),
              resp->GetInspiratoryRespiratoryResistance(PressureTimePerVolumeUnit::cmH2O_s_Per_L),
              resp->GetRespiratoryCompliance(VolumePerPressureUnit::mL_Per_cmH2O),
              pe->GetBloodChemistrySystem()->GetOxygenSaturation() * 100);
  std::filesystem::create_directories("./states");
  if (!pe->SerializeToFile(out)) {
    std::fprintf(stderr, "%s: could not write %s\n", p.id, out.c_str());
    return 1;
  }
  std::printf("%s: wrote %s\n", p.id, out.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "use: pulse_states list|all|<id> [warmup_s]   (run from Pulse's bin directory)\n");
    return 1;
  }
  const double warmup = argc > 2 ? std::atof(argv[2]) : 120.0;
  int rc = 0, found = 0;
  for (const PatientType& p : catalogue()) {
    if (std::strcmp(argv[1], "list") == 0) {
      std::printf("%-15s %s\n", p.id, p.what);
      found = 1;
    } else if (std::strcmp(argv[1], "all") == 0 || std::strcmp(argv[1], p.id) == 0) {
      rc |= build(p, warmup);
      found = 1;
    }
  }
  if (!found) {
    std::fprintf(stderr, "unknown patient type '%s' (pulse_states list)\n", argv[1]);
    return 1;
  }
  return rc;
}
