// patient.cpp - M3: paciente virtual con Pulse, como proceso QNX acoplado a pneumatic_sim.
//
// Ciclo (cada ventana de 20 ms de tiempo de planta):
//   MsgSend(BG_MSG_PATIENT) con el resultado de la ventana anterior -> la planta responde con la
//   ventana siguiente (presión promedio) cuando se cierra; mientras tanto este proceso está bloqueado.
//   Pulse avanza 20 ms con esa presión y se lee volumen, flujo, R, C y signos vitales.
//
// Pulse corre por debajo de breath_ctrl y pneumatic_sim: si se atrasa (hay pasos de hasta 66 ms,
// M2), la planta sigue con el último R y C, y patient se pone al día después (va 7-9x más rápido que
// el tiempo real).
//
// Debe ejecutarse desde el directorio de datos de Pulse (/data/pulse en la VM).
// Uso: patient [-s segundos] [-p prio] [-o salida.csv]
//      patient -W segundos    crea el estado "ya ventilado" y termina (una sola vez)
//
// Por qué el estado ventilado: el R y la C que reporta Pulse son promedios por respiración que
// convergen en varias respiraciones (R: 1.5 -> 22 cmH2O/(L/s)). Si se arranca con el paciente que
// nunca se ventiló, el RC de la planta usa valores malos y las correcciones llegan a 39 mL por ventana.
#include <getopt.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
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
#include "cdm/system/physiology/SEBloodChemistrySystem.h"
#include "cdm/system/physiology/SECardiovascularSystem.h"
#include "cdm/system/physiology/SERespiratorySystem.h"

#include "bg_proto.h"
#include "rt_util.h"

namespace {

constexpr const char* kProg = "patient";
constexpr const char* kStatePath = "./states/BreathGuard_StandardMale.json";
constexpr const char* kVentStatePath = "./states/BreathGuard_StandardMale_PCV.json";
constexpr double kFiO2 = 0.21;

struct Row {
  uint32_t k, backlog, lost;
  double t_plant, t_wall, paw, v_ml, q_ml_s, r, c_ml, spo2, etco2, hr, step_us, last_dv_ml;
};

}  // namespace

int main(int argc, char** argv)
{
  double secs = 60, warmup = 0;
  int prio = 20, opt;
  const char* out_path = nullptr;
  while ((opt = getopt(argc, argv, "s:p:o:W:")) != -1) {
    switch (opt) {
    case 's': secs = std::atof(optarg); break;
    case 'W': warmup = std::atof(optarg); break;
    case 'p': prio = std::atoi(optarg); break;
    case 'o': out_path = optarg; break;
    default:
      std::fprintf(stderr, "uso: %s [-s segundos] [-p prio] [-o salida.csv]\n", kProg);
      return 1;
    }
  }

  // 1. Cargar el paciente ya estabilizado (M2). Esto tarda; se hace antes de conectarse.
  double t_load = rt_now_s();
  std::unique_ptr<PhysiologyEngine> pe = CreatePulseEngine();
  pe->GetLogger()->LogToConsole(false);
  pe->GetLogger()->SetLogFile("./test_results/patient.log");
  const char* state = (warmup == 0 && access(kVentStatePath, R_OK) == 0) ? kVentStatePath : kStatePath;
  if (!pe->SerializeFromFile(state)) {
    std::fprintf(stderr, "%s: no se pudo cargar %s\n", kProg, state);
    return 1;
  }
  // Paciente sedado: sin respiración propia.
  SEDyspnea dyspnea;
  dyspnea.GetTidalVolumeSeverity().SetValue(1.0);
  dyspnea.GetRespirationRateSeverity().SetValue(1.0);
  pe->ProcessAction(dyspnea);
  // Ventilación desde fuera: nosotros fijamos la presión en la vía aérea (relativa a la atmósfera).
  SEMechanicalVentilation mech;
  mech.SetState(eSwitch::On);
  const SESubstanceManager& subs = pe->GetSubstanceManager();
  mech.GetGasFraction(*subs.GetSubstance("Oxygen")).GetFractionAmount().SetValue(kFiO2);
  mech.GetGasFraction(*subs.GetSubstance("CarbonDioxide")).GetFractionAmount().SetValue(4.0e-4);
  mech.GetGasFraction(*subs.GetSubstance("Nitrogen")).GetFractionAmount().SetValue(1.0 - kFiO2 - 4.0e-4);
  const double dt_pulse = pe->GetTimeStep(TimeUnit::s);
  if (dt_pulse < BG_WINDOW_S - 1e-9 || dt_pulse > BG_WINDOW_S + 1e-9) {
    std::fprintf(stderr, "%s: Pulse avanza %.4f s por paso y la ventana es %.4f s\n", kProg, dt_pulse,
                 BG_WINDOW_S);
    return 1;
  }
  if (warmup > 0) {
    // PCV 20/5 cmH2O, 12 rpm, I:E 1:2 con presión cuadrada, como en M2.
    const auto steps = static_cast<long>(warmup / dt_pulse);
    for (long i = 0; i < steps; i++) {
      const double t = static_cast<double>(i) * dt_pulse;
      const double phase = t / 5.0 - static_cast<double>(static_cast<long>(t / 5.0));
      mech.GetPressure().SetValue(phase < 1.0 / 3.0 ? 20.0 : 5.0, PressureUnit::cmH2O);
      pe->ProcessAction(mech);
      pe->AdvanceModelTime();
    }
    const SERespiratorySystem* resp = pe->GetRespiratorySystem();
    std::printf("%s: %.0f s ventilado en %.1f s; R %.1f, C %.1f mL/cmH2O, SpO2 %.1f %%\n", kProg, warmup,
                rt_now_s() - t_load,
                resp->GetInspiratoryRespiratoryResistance(PressureTimePerVolumeUnit::cmH2O_s_Per_L),
                resp->GetRespiratoryCompliance(VolumePerPressureUnit::mL_Per_cmH2O),
                pe->GetBloodChemistrySystem()->GetOxygenSaturation() * 100);
    return pe->SerializeToFile(kVentStatePath) ? 0 : 1;
  }
  std::printf("%s: %s cargado en %.1f s\n", kProg, state, rt_now_s() - t_load);
  std::fflush(stdout);

  // 2. Conectarse a la planta por nombre.
  int coid = -1;
  for (int i = 0; i < 3000 && coid == -1; i++) {
    coid = name_open(BG_PNEUMATIC_NAME, 0);
    if (coid == -1) usleep(10000);
  }
  if (coid == -1) rt_die(kProg, "name_open(" BG_PNEUMATIC_NAME ")");

  rt_setup(kProg, prio);

  std::vector<Row> rows;
  rows.reserve(static_cast<size_t>(secs / BG_WINDOW_S) + 1000);

  bg_patient_msg_t msg{};
  msg.hdr.type = BG_MSG_PATIENT;
  msg.have_result = 0;
  const double t_start = rt_now_s();

  while (rt_now_s() - t_start < secs) {
    bg_window_t w;
    if (MsgSend(coid, &msg, sizeof msg, &w, sizeof w) == -1) {
      if (errno == ESRCH || errno == EBADF) break;  // la planta terminó
      rt_die(kProg, "MsgSend(PATIENT)");
    }
    const double t_got = rt_now_s();

    mech.GetPressure().SetValue(w.paw_mean, PressureUnit::cmH2O);
    pe->ProcessAction(mech);
    pe->AdvanceModelTime();
    const double step_us = (rt_now_s() - t_got) * 1e6;

    const SERespiratorySystem* resp = pe->GetRespiratorySystem();
    bg_patient_state_t& st = msg.st;
    st.v = resp->GetTotalLungVolume(VolumeUnit::mL) / 1e3;
    st.q = resp->GetInspiratoryFlow(VolumePerTimeUnit::mL_Per_s) / 1e3;
    st.r = resp->GetInspiratoryRespiratoryResistance(PressureTimePerVolumeUnit::cmH2O_s_Per_L);
    st.c = resp->GetRespiratoryCompliance(VolumePerPressureUnit::mL_Per_cmH2O) / 1e3;
    st.spo2 = pe->GetBloodChemistrySystem()->GetOxygenSaturation();
    st.etco2 = resp->GetEndTidalCarbonDioxidePressure(PressureUnit::mmHg);
    st.hr = pe->GetCardiovascularSystem()->GetHeartRate(FrequencyUnit::Per_min);
    st.fio2 = kFiO2;  // la mezcla que le damos a Pulse (aire ambiente; no hay mezclador de O2 en el modelo)
    msg.have_result = 1;
    msg.k = w.k;

    rows.push_back({w.k, w.backlog, w.lost, w.t_end, t_got - t_start, w.paw_mean, st.v * 1e3, st.q * 1e3,
                    st.r, st.c * 1e3, st.spo2, st.etco2, st.hr, step_us, w.last_dv * 1e3});
  }
  name_close(coid);
  rt_relax();

  if (rows.empty()) {
    std::fprintf(stderr, "%s: no se simuló ninguna ventana\n", kProg);
    return 1;
  }
  size_t late = 0;
  uint32_t max_backlog = 0;
  double max_step = 0;
  for (const Row& r : rows) {
    if (r.step_us > BG_WINDOW_S * 1e6) late++;
    if (r.backlog > max_backlog) max_backlog = r.backlog;
    if (r.step_us > max_step) max_step = r.step_us;
  }
  const Row& last = rows.back();
  std::printf("%s: %zu ventanas, pasos > 20 ms %zu, paso máx %.1f ms, atraso máx %u ventanas, perdidas %u; "
              "final: SpO2 %.1f %%, EtCO2 %.1f mmHg, FC %.0f\n",
              kProg, rows.size(), late, max_step / 1e3, max_backlog, last.lost, last.spo2 * 100, last.etco2,
              last.hr);

  if (out_path) {
    std::FILE* f = std::fopen(out_path, "w");
    if (!f) rt_die(kProg, out_path);
    std::fprintf(f, "k,t_plant,t_wall,backlog,lost,paw_mean,v_ml,q_ml_s,r,c_ml,spo2,etco2,hr,step_us,last_dv_ml\n");
    for (const Row& r : rows)
      std::fprintf(f, "%u,%.3f,%.4f,%u,%u,%.3f,%.2f,%.2f,%.2f,%.2f,%.4f,%.2f,%.1f,%.0f,%.4f\n", r.k, r.t_plant,
                   r.t_wall, r.backlog, r.lost, r.paw, r.v_ml, r.q_ml_s, r.r, r.c_ml, r.spo2, r.etco2, r.hr,
                   r.step_us, r.last_dv_ml);
    std::fclose(f);
  }
  return 0;
}
