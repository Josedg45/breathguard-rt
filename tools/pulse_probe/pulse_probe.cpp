// pulse_probe.cpp - M2: sondea a Pulse como paciente virtual.
//
//   pulse_probe internal <segundos> <salida.csv> [Pinsp PEEP rpm fraccion_insp]
//       Ventilador interno de Pulse (PCV)
//   pulse_probe external <segundos> <salida.csv> [Pinsp PEEP rpm fraccion_insp]
//       Presión aplicada desde fuera en cada paso
//
// El modo external es el modelo de acople de BreathGuard: alguien externo fija la presión en la
// vía aérea y Pulse devuelve volumen, flujo y signos vitales. Debe ejecutarse desde el directorio
// install/bin de Pulse, donde están los datos del paciente y las sustancias.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/PulseEngine.h"
#include "cdm/engine/SEPatientConfiguration.h"
#include "cdm/patient/actions/SEDyspnea.h"
#include "cdm/patient/actions/SEMechanicalVentilation.h"
#include "cdm/properties/SEScalar0To1.h"
#include "cdm/properties/SEScalarFrequency.h"
#include "cdm/properties/SEScalarPressure.h"
#include "cdm/properties/SEScalarPressureTimePerVolume.h"
#include "cdm/properties/SEScalarTime.h"
#include "cdm/properties/SEScalarVolume.h"
#include "cdm/properties/SEScalarVolumePerPressure.h"
#include "cdm/properties/SEScalarVolumePerTime.h"
#include "cdm/substance/SESubstanceFraction.h"
#include "cdm/substance/SESubstanceManager.h"
#include "cdm/system/environment/SEEnvironment.h"
#include "cdm/system/environment/SEEnvironmentalConditions.h"
#include "cdm/system/equipment/mechanical_ventilator/actions/SEMechanicalVentilatorPressureControl.h"
#include "cdm/system/physiology/SEBloodChemistrySystem.h"
#include "cdm/system/physiology/SECardiovascularSystem.h"
#include "cdm/system/physiology/SERespiratorySystem.h"

namespace {

// Ajustes de PCV. Por defecto, los mismos en ambos modos para poder compararlos.
struct PcvSettings {
  double pinsp_cmH2O = 20.0;
  double peep_cmH2O = 5.0;
  double rate_per_min = 12.0;
  double insp_fraction = 1.0 / 3.0;  // Relación I:E de 1:2
};
constexpr double kFiO2 = 0.21;

const char* const kStatePath = "./states/BreathGuard_StandardMale.json";

bool load_patient(PhysiologyEngine& pe)
{
  if (std::filesystem::exists(kStatePath) && pe.SerializeFromFile(kStatePath))
    return true;

  // Sin estado guardado: estabilizar al paciente desde cero y guardarlo. Tarda minutos.
  std::printf("Estabilizando paciente (solo la primera vez)...\n");
  SEPatientConfiguration config;
  config.SetPatientFile("StandardMale.json");
  if (!pe.InitializeEngine(config))
    return false;
  std::filesystem::create_directories("./states");
  return pe.SerializeToFile(kStatePath);
}

// Elimina el esfuerzo respiratorio propio: paciente sedado que depende del ventilador.
void remove_spontaneous_breathing(PhysiologyEngine& pe)
{
  SEDyspnea dyspnea;
  dyspnea.GetTidalVolumeSeverity().SetValue(1.0);
  dyspnea.GetRespirationRateSeverity().SetValue(1.0);
  pe.ProcessAction(dyspnea);
}

void start_internal_ventilator(PhysiologyEngine& pe, const PcvSettings& pcv)
{
  SEMechanicalVentilatorPressureControl pc;
  pc.SetConnection(eSwitch::On);
  pc.SetMode(eMechanicalVentilator_PressureControlMode::AssistedControl);
  pc.SetInspirationWaveform(eDriverWaveform::Square);
  pc.GetFractionInspiredOxygen().SetValue(kFiO2);
  pc.GetInspiratoryPeriod().SetValue(60.0 / pcv.rate_per_min * pcv.insp_fraction, TimeUnit::s);
  pc.GetInspiratoryPressure().SetValue(pcv.pinsp_cmH2O, PressureUnit::cmH2O);
  pc.GetPositiveEndExpiratoryPressure().SetValue(pcv.peep_cmH2O, PressureUnit::cmH2O);
  pc.GetRespirationRate().SetValue(pcv.rate_per_min, FrequencyUnit::Per_min);
  pe.ProcessAction(pc);
}

double percentile(std::vector<double> sorted_values, double pct)
{
  std::sort(sorted_values.begin(), sorted_values.end());
  auto rank = static_cast<size_t>(pct / 100.0 * static_cast<double>(sorted_values.size()));
  return sorted_values[std::min(rank, sorted_values.size() - 1)];
}

}  // namespace

int main(int argc, char** argv)
{
  if ((argc != 4 && argc != 8) || (std::string(argv[1]) != "internal" && std::string(argv[1]) != "external")) {
    std::fprintf(stderr, "uso: pulse_probe internal|external <segundos> <salida.csv> [Pinsp PEEP rpm fraccion_insp]\n");
    return EXIT_FAILURE;
  }
  const bool external = std::string(argv[1]) == "external";
  const double duration_s = std::atof(argv[2]);
  PcvSettings pcv;
  if (argc == 8) {
    pcv.pinsp_cmH2O = std::atof(argv[4]);
    pcv.peep_cmH2O = std::atof(argv[5]);
    pcv.rate_per_min = std::atof(argv[6]);
    pcv.insp_fraction = std::atof(argv[7]);
  }

  std::unique_ptr<PhysiologyEngine> pe = CreatePulseEngine();
  pe->GetLogger()->LogToConsole(false);
  std::string log_file = "./test_results/pulse_probe_";
  log_file += argv[1];
  log_file += ".log";
  pe->GetLogger()->SetLogFile(log_file);
  if (!load_patient(*pe)) {
    std::fprintf(stderr, "No se pudo cargar el paciente; revisa el log de Pulse\n");
    return EXIT_FAILURE;
  }
  remove_spontaneous_breathing(*pe);

  SEMechanicalVentilation mech;
  if (external) {
    mech.SetState(eSwitch::On);
    const SESubstanceManager& subs = pe->GetSubstanceManager();
    mech.GetGasFraction(*subs.GetSubstance("Oxygen")).GetFractionAmount().SetValue(kFiO2);
    mech.GetGasFraction(*subs.GetSubstance("CarbonDioxide")).GetFractionAmount().SetValue(4.0e-4);
    mech.GetGasFraction(*subs.GetSubstance("Nitrogen")).GetFractionAmount().SetValue(1.0 - kFiO2 - 4.0e-4);
  } else {
    start_internal_ventilator(*pe, pcv);
  }

  const double ambient_cmH2O = pe->GetEnvironment()->GetEnvironmentalConditions()->GetAtmosphericPressure(PressureUnit::cmH2O);
  const double dt_s = pe->GetTimeStep(TimeUnit::s);
  const auto steps = static_cast<size_t>(duration_s / dt_s);
  const double period_s = 60.0 / pcv.rate_per_min;

  std::FILE* csv = std::fopen(argv[3], "w");
  if (!csv) {
    std::perror(argv[3]);
    return EXIT_FAILURE;
  }
  std::fprintf(csv, "t_s,paw_set_cmH2O,lung_volume_mL,insp_flow_mL_s,tidal_volume_mL,rate_per_min,"
                    "spo2,etco2_mmHg,hr_per_min,compliance_mL_cmH2O,resistance_cmH2O_s_L,step_us\n");

  std::vector<double> step_us;
  step_us.reserve(steps);

  for (size_t i = 0; i < steps; i++) {
    const double t_s = static_cast<double>(i) * dt_s;
    const double phase = t_s / period_s - static_cast<double>(static_cast<long>(t_s / period_s));
    double paw = external ? (phase < pcv.insp_fraction ? pcv.pinsp_cmH2O : pcv.peep_cmH2O) : 0.0;

    const auto t0 = std::chrono::steady_clock::now();
    if (external) {
      mech.GetPressure().SetValue(paw, PressureUnit::cmH2O);
      pe->ProcessAction(mech);
    }
    pe->AdvanceModelTime();
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    step_us.push_back(us);

    const SERespiratorySystem* resp = pe->GetRespiratorySystem();
    // Pulse reporta presión absoluta; el ventilador trabaja relativo a la atmósfera.
    if (!external) paw = resp->GetAirwayPressure(PressureUnit::cmH2O) - ambient_cmH2O;
    std::fprintf(csv, "%.3f,%.3f,%.2f,%.2f,%.1f,%.2f,%.4f,%.2f,%.1f,%.2f,%.2f,%.1f\n",
                 t_s, paw,
                 resp->GetTotalLungVolume(VolumeUnit::mL),
                 resp->GetInspiratoryFlow(VolumePerTimeUnit::mL_Per_s),
                 resp->GetTidalVolume(VolumeUnit::mL),
                 resp->GetRespirationRate(FrequencyUnit::Per_min),
                 pe->GetBloodChemistrySystem()->GetOxygenSaturation(),
                 resp->GetEndTidalCarbonDioxidePressure(PressureUnit::mmHg),
                 pe->GetCardiovascularSystem()->GetHeartRate(FrequencyUnit::Per_min),
                 resp->GetRespiratoryCompliance(VolumePerPressureUnit::mL_Per_cmH2O),
                 resp->GetInspiratoryRespiratoryResistance(PressureTimePerVolumeUnit::cmH2O_s_Per_L),
                 us);
  }
  std::fclose(csv);

  double total_us = 0.0;
  for (double us : step_us) total_us += us;
  const double dt_us = dt_s * 1e6;
  std::printf("modo=%s dt=%.1f ms pasos=%zu\n", argv[1], dt_s * 1e3, steps);
  std::printf("paso (us): prom=%.0f p50=%.0f p99=%.0f max=%.0f\n",
              total_us / static_cast<double>(steps), percentile(step_us, 50), percentile(step_us, 99),
              *std::max_element(step_us.begin(), step_us.end()));
  std::printf("factor de tiempo real: %.1fx; pasos que superan dt: %ld\n",
              dt_us * static_cast<double>(steps) / total_us,
              static_cast<long>(std::count_if(step_us.begin(), step_us.end(),
                                              [dt_us](double us) { return us > dt_us; })));
  return EXIT_SUCCESS;
}
